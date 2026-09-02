// Regression tests for compiler bugs that produced wrong code silently.
//
// Every test here pins down a behaviour that was previously wrong in a way that
// no existing test noticed: the program still compiled, it just did the wrong
// thing at run time. Each one names the mistake it guards against.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_optimizer.h"
#include "../ir_interpreter.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include "../instance_layout.h"
#include "../variant_layout.h"
#include "../syscall_numbers.h"
#include <cassert>
#include <climits>
#include <unordered_map>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

// -= Helpers =-

static IRProgram compile_to_ir(const std::string& source, bool optimize = true) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
	}
	return ir;
}

static const IRFunction& find_function(const IRProgram& ir, const std::string& name) {
	for (const auto& func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	throw std::runtime_error("Function not found: " + name);
}

static const IRGlobalVar& find_global(const IRProgram& ir, const std::string& name) {
	for (const auto& global : ir.globals) {
		if (global.name == name) {
			return global;
		}
	}
	throw std::runtime_error("Global not found: " + name);
}

static int count_opcode(const IRFunction& func, IROpcode opcode) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

static IRInterpreter::Value run(const std::string& source, const std::string& function,
	const std::vector<IRInterpreter::Value>& args = {})
{
	IRProgram ir = compile_to_ir(source);
	IRInterpreter interpreter(ir);
	return interpreter.call(function, args);
}

static int64_t run_int(const std::string& source, const std::string& function,
	const std::vector<IRInterpreter::Value>& args = {})
{
	IRInterpreter::Value value = run(source, function, args);
	if (std::holds_alternative<bool>(value)) {
		return std::get<bool>(value) ? 1 : 0;
	}
	return std::get<int64_t>(value);
}

// Returns true when compiling the source throws a CompilerException.
static bool rejects(const std::string& source) {
	try {
		compile_to_ir(source, false);
		return false;
	} catch (const CompilerException&) {
		return true;
	}
}

static std::vector<uint8_t> compile_to_code(const std::string& source) {
	IRProgram ir = compile_to_ir(source);
	RISCVCodeGen riscv{ VariantLayout(false) };
	return riscv.generate(ir);
}

static uint32_t word_at(const std::vector<uint8_t>& code, size_t off) {
	return uint32_t(code[off]) | (uint32_t(code[off + 1]) << 8) |
			(uint32_t(code[off + 2]) << 16) | (uint32_t(code[off + 3]) << 24);
}

// -= Tests =-

// Locals shadow globals. The lookup used to consult the global table first, so a
// local sharing a name with a global was never seen: reads and writes both went
// to the global, which the function then also corrupted.
static void test_locals_shadow_globals() {
	std::cout << "Testing that locals shadow globals..." << std::endl;

	const std::string source =
		"var counter = 10\n"
		"func test():\n"
		"\tvar counter = 1\n"
		"\tcounter = counter + 1\n"
		"\treturn counter\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& test = find_function(ir, "test");
	assert(count_opcode(test, IROpcode::LOAD_GLOBAL) == 0);
	assert(count_opcode(test, IROpcode::STORE_GLOBAL) == 0);
	assert(run_int(source, "test") == 2);

	// Parameters shadow globals too.
	const std::string param_source =
		"var value = 100\n"
		"func test(value):\n"
		"\treturn value\n";
	assert(count_opcode(find_function(compile_to_ir(param_source), "test"), IROpcode::LOAD_GLOBAL) == 0);

	// Without a local of that name the global is still reached.
	const std::string global_source =
		"var counter = 10\n"
		"func test():\n"
		"\tcounter = counter + 1\n"
		"\treturn counter\n";
	assert(count_opcode(find_function(compile_to_ir(global_source), "test"), IROpcode::STORE_GLOBAL) == 1);

	// Assigning to a global const is an error, as it is for a local const.
	assert(rejects("const LIMIT = 3\nfunc test():\n\tLIMIT = 4\n\treturn LIMIT\n"));

	std::cout << "  ✓ Locals shadow globals" << std::endl;
}

// The operand-role table. Passes used to assume "operand 0 is the destination,
// every other operand is a source", which is wrong for CALL (destination in
// operand 1) and for VSET/STORE_GLOBAL/RETURN/branches (operand 0 is read).
static void test_operand_roles() {
	std::cout << "Testing IR operand roles..." << std::endl;

	IRStringTable strings;

	{
		// CALL name, dst, argc, args...
		IRInstruction call(IROpcode::CALL);
		call.operands.push_back(IRValue::str(strings.intern("f")));
		call.operands.push_back(IRValue::reg(3)); // destination
		call.operands.push_back(IRValue::imm(1));
		call.operands.push_back(IRValue::reg(7)); // argument
		assert(ir_destination_register(call) == 3);
		assert(!ir_reads_operand(call, 1));
		assert(ir_reads_operand(call, 3));
	}

	{
		// VSET obj, name_idx, name_len, value - no destination, operand 0 is read.
		IRInstruction vset(IROpcode::VSET);
		vset.operands.push_back(IRValue::reg(2));
		vset.operands.push_back(IRValue::imm(0));
		vset.operands.push_back(IRValue::imm(4));
		vset.operands.push_back(IRValue::reg(5));
		assert(ir_destination_register(vset) == -1);
		assert(ir_reads_operand(vset, 0));
		assert(ir_reads_operand(vset, 3));
	}

	{
		// STORE_GLOBAL index, value - operand 0 is an immediate, not a register.
		IRInstruction store(IROpcode::STORE_GLOBAL, IRValue::imm(0), IRValue::reg(4));
		assert(ir_destination_register(store) == -1);
		assert(!ir_reads_operand(store, 0));
		assert(ir_reads_operand(store, 1));
	}

	{
		// A bare RETURN reads r0 implicitly.
		std::vector<int> reads;
		ir_collect_read_registers(IRInstruction(IROpcode::RETURN), reads);
		assert(reads.size() == 1 && reads[0] == 0);
	}

	std::cout << "  ✓ IR operand roles" << std::endl;
}

// A pending MOVE must not be delayed past an instruction that reads the register
// it writes. VSET reads its object out of operand 0, which the dead-store pass
// used to skip, so the VSET saw whatever the register held beforehand.
static void test_store_not_delayed_past_vset() {
	std::cout << "Testing that stores are not delayed past a VSET..." << std::endl;

	IRFunction func;
	func.name = "test";
	func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(0), IRValue::imm(1));
	func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(1), IRValue::reg(0));
	IRInstruction vset(IROpcode::VSET);
	vset.operands.push_back(IRValue::reg(1)); // object being written into
	vset.operands.push_back(IRValue::imm(0));
	vset.operands.push_back(IRValue::imm(1));
	vset.operands.push_back(IRValue::reg(0)); // value
	func.instructions.push_back(vset);
	func.instructions.emplace_back(IROpcode::RETURN);
	func.max_registers = 2;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	// Whatever else the optimizer does, r1 has to be defined before the VSET.
	size_t move_index = SIZE_MAX;
	size_t vset_index = SIZE_MAX;
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];
		if (instr.opcode == IROpcode::VSET) {
			vset_index = i;
		} else if (ir_destination_register(instr) == 1) {
			move_index = i;
		}
	}
	assert(vset_index != SIZE_MAX);
	assert(move_index != SIZE_MAX);
	assert(move_index < vset_index);

	std::cout << "  ✓ Stores are not delayed past a VSET" << std::endl;
}

// Copy propagation has to kill the register a CALL defines. Because CALL keeps
// its destination in operand 1, the kill used to be skipped and a constant that
// had previously lived in that register was propagated over the call result.
static void test_call_result_kills_constant() {
	std::cout << "Testing that a call result invalidates a tracked constant..." << std::endl;

	IRStringTable strings;
	IRFunction func;
	func.name = "test";
	func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(42));
	IRInstruction call(IROpcode::CALL);
	call.operands.push_back(IRValue::str(strings.intern("side")));
	call.operands.push_back(IRValue::reg(1)); // overwrites r1 with the call result
	call.operands.push_back(IRValue::imm(0));
	func.instructions.push_back(call);
	func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(0), IRValue::reg(1));
	func.instructions.emplace_back(IROpcode::RETURN);
	func.max_registers = 2;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	// The MOVE must not have become "LOAD_IMM r0, 42".
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM && ir_destination_register(instr) == 0) {
			assert(false && "call result was replaced by a stale constant");
		}
	}

	std::cout << "  ✓ A call result invalidates a tracked constant" << std::endl;
}

// Register types are per-function. Virtual register numbers restart at 0 in
// every function, so a type left on r0 by one function used to be inherited by
// the next, sending the backend down a native path for the wrong Variant type.
static void test_register_types_do_not_leak_between_functions() {
	std::cout << "Testing that register types do not leak between functions..." << std::endl;

	const std::string source =
		"func first():\n"
		"\treturn 1\n"
		"func second(a):\n"
		"\tif a:\n"
		"\t\treturn 1\n"
		"\treturn 0\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& second = find_function(ir, "second");
	for (const auto& instr : second.instructions) {
		if (instr.opcode == IROpcode::BRANCH_ZERO) {
			// 'a' is an untyped parameter, so nothing is known about its type.
			assert(instr.type_hint == IRInstruction::TypeHint_NONE);
		}
	}

	std::cout << "  ✓ Register types do not leak between functions" << std::endl;
}

// A global's address is `.globals + index * sizeof(Variant)`. Emitting that as
// `la` followed by `addi` truncates to a 12-bit signed immediate, so every
// global past #85 (or #51 in a double-precision build) addressed the wrong slot.
static void test_large_global_offsets() {
	std::cout << "Testing global addressing past the 12-bit immediate range..." << std::endl;

	std::string source;
	const int global_count = 200;
	for (int i = 0; i < global_count; i++) {
		source += "static var g" + std::to_string(i) + " = " + std::to_string(i) + "\n";
	}
	source += "func test():\n\treturn g199\n";

	const std::vector<uint8_t> code = compile_to_code(source);

	// No ADDI may carry a truncated global offset: every offset is folded into
	// the AUIPC/ADDI pair by the relocation instead. `static var` so the slots
	// are in the data area; a member is addressed off the base register, where
	// a wide offset is materialized by LUI + ADDI and 680 is a legitimate half.
	for (size_t off = 0; off + 4 <= code.size(); off += 4) {
		const uint32_t instr = word_at(code, off);
		if ((instr & 0x7F) != 0x13 || ((instr >> 12) & 7) != 0) {
			continue; // not an ADDI
		}
		const int32_t imm = int32_t(instr) >> 20;
		// 199 * 24 = 4776 does not fit a signed 12-bit immediate; it wraps to
		// 4776 - 4096 = 680. Finding that as an ADDI immediate would mean a
		// global offset had been silently truncated.
		assert(imm != 680);
	}

	// The same for members: no ADDI off the base register may carry the wrap.
	{
		std::string members;
		for (int i = 0; i < global_count; i++) {
			members += "var g" + std::to_string(i) + " = " + std::to_string(i) + "\n";
		}
		members += "func test():\n\treturn g199\n";
		const std::vector<uint8_t> member_code = compile_to_code(members);
		constexpr uint8_t REG_TP = 4;
		for (size_t off = 0; off + 4 <= member_code.size(); off += 4) {
			const uint32_t instr = word_at(member_code, off);
			if ((instr & 0x7F) != 0x13 || ((instr >> 12) & 7) != 0) {
				continue;
			}
			if (((instr >> 15) & 0x1F) != REG_TP) {
				continue;
			}
			assert((int32_t(instr) >> 20) != 680);
		}
	}

	// The data area covers all 200 Variants.
	IRProgram ir = compile_to_ir(source);
	RISCVCodeGen riscv{ VariantLayout(false) };
	riscv.generate(ir);
	assert(riscv.get_global_data_size() >= size_t(global_count) * 24);

	std::cout << "  ✓ Global addressing past the 12-bit immediate range" << std::endl;
}

// Global initializers that are not compile-time constants. Every one of these
// used to fall through the initializer matcher silently and leave the global
// NIL, with no diagnostic.
static void test_global_initializer_forms() {
	std::cout << "Testing global initializer forms..." << std::endl;

	// Unary minus over a literal is a constant, not a runtime expression.
	{
		const IRProgram ir = compile_to_ir("var x = -5\nvar f = -2.5\nfunc test():\n\treturn x\n");
		const IRGlobalVar& x = find_global(ir, "x");
		assert(x.init_type == IRGlobalVar::InitType::INT);
		assert(std::get<int64_t>(x.init_value) == -5);
		const IRGlobalVar& f = find_global(ir, "f");
		assert(f.init_type == IRGlobalVar::InitType::FLOAT);
		assert(std::get<double>(f.init_value) == -2.5);
		assert(!ir.has_global_init); // both fold, so nothing runs at startup
	}

	// A reference to an earlier const folds to that const's value.
	{
		const IRProgram ir = compile_to_ir("const M = 10\nvar y = M\nfunc test():\n\treturn y\n");
		const IRGlobalVar& y = find_global(ir, "y");
		assert(y.init_type == IRGlobalVar::InitType::INT);
		assert(std::get<int64_t>(y.init_value) == 10);
	}

	// Referring to a global declared later would read NIL, so it is rejected.
	assert(rejects("var a = b\nvar b = 1\nfunc test():\n\treturn a\n"));

	// Non-empty containers, nesting and packed arrays run at startup.
	{
		const IRProgram ir = compile_to_ir(
			"var a = [1, 2, 3]\n"
			"var d = {\"k\": 1}\n"
			"var n = [[1, 2], {\"k\": 3}]\n"
			"var p = PackedInt32Array()\n"
			"func test():\n"
			"\treturn a\n");
		// Members, so they are built per instance rather than at startup.
		assert(ir.has_member_init);
		assert(!ir.has_global_init);
		for (const char* name : { "a", "d", "n", "p" }) {
			assert(find_global(ir, name).init_type == IRGlobalVar::InitType::RUNTIME);
		}
		// Untyped containers still get a Variant type for @export registration.
		assert(find_global(ir, "a").value_type == Variant::ARRAY);
		assert(find_global(ir, "d").value_type == Variant::DICTIONARY);
		assert(find_global(ir, "p").value_type == Variant::PACKED_INT32_ARRAY);
		assert(count_opcode(ir.member_init, IROpcode::STORE_GLOBAL) == 4);
	}

	// Empty containers stay compile-time constants: no startup code for them.
	{
		const IRProgram ir = compile_to_ir("var a = []\nvar d = {}\nfunc test():\n\treturn a\n");
		assert(find_global(ir, "a").init_type == IRGlobalVar::InitType::EMPTY_ARRAY);
		assert(find_global(ir, "d").init_type == IRGlobalVar::InitType::EMPTY_DICT);
		assert(!ir.has_global_init);
		assert(!ir.has_member_init);
	}

	// A type-hinted global with no initializer gets its type's default value,
	// the way GDScript does, rather than staying NIL.
	{
		const IRProgram ir = compile_to_ir(
			"var a: Array\nvar s: String\nvar n: int\nvar f: float\nvar p: PackedByteArray\n"
			"func test():\n\treturn n\n");
		assert(find_global(ir, "a").init_type == IRGlobalVar::InitType::EMPTY_ARRAY);
		assert(find_global(ir, "s").init_type == IRGlobalVar::InitType::STRING);
		assert(find_global(ir, "n").init_type == IRGlobalVar::InitType::INT);
		assert(find_global(ir, "f").init_type == IRGlobalVar::InitType::FLOAT);
		assert(find_global(ir, "p").init_type == IRGlobalVar::InitType::RUNTIME);
	}

	// Declaring the same global twice is an error rather than a silent shadow.
	assert(rejects("var a = 1\nvar a = 2\nfunc test():\n\treturn a\n"));

	std::cout << "  ✓ Global initializer forms" << std::endl;
}

// The global init function runs before any @export property is registered, so a
// property is registered holding the value it was declared with.
static void test_global_init_runs_before_property_registration() {
	std::cout << "Testing global init ordering against property registration..." << std::endl;

	const std::string source =
		"@export var items = [1, 2]\n"
		"func test():\n"
		"\treturn items\n";

	const IRProgram ir = compile_to_ir(source);
	assert(ir.has_member_init);
	assert(find_global(ir, "items").is_property);
	assert(find_global(ir, "items").value_type == Variant::ARRAY);

	// The init function is internal: it must not be exported as a callable.
	RISCVCodeGen riscv{ VariantLayout(false) };
	riscv.generate(ir);
	assert(riscv.get_function_offsets().count("__init_globals") == 0);
	assert(riscv.get_function_offsets().count(".init_globals") == 0);
	assert(riscv.get_function_offsets().count("__init_members") == 0);

	// One member, the shared return slot the initializers write into, and the
	// blob describing the record.
	assert(riscv.get_global_data_size() == 2 * 24 + size_t(InstanceLayout::BLOB_SIZE));

	std::cout << "  ✓ Global init ordering against property registration" << std::endl;
}

// 'and' and 'or' short-circuit in GDScript: the right-hand side is not evaluated
// when the left already decides the result. Lowering them to a binary IR op ran
// the right side's side effects unconditionally.
static void test_logical_short_circuit() {
	std::cout << "Testing short-circuit evaluation..." << std::endl;

	const std::string source =
		"func side():\n"
		"\treturn 1\n"
		"func test(a):\n"
		"\tif a and side():\n"
		"\t\treturn 1\n"
		"\treturn 0\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& test = find_function(ir, "test");
	assert(count_opcode(test, IROpcode::AND) == 0);
	assert(count_opcode(test, IROpcode::OR) == 0);

	// The call has to sit between the two short-circuit tests, not before them.
	size_t first_branch = SIZE_MAX;
	size_t call_index = SIZE_MAX;
	for (size_t i = 0; i < test.instructions.size(); i++) {
		if (test.instructions[i].opcode == IROpcode::BRANCH_ZERO && first_branch == SIZE_MAX) {
			first_branch = i;
		} else if (test.instructions[i].opcode == IROpcode::CALL) {
			call_index = i;
		}
	}
	assert(first_branch != SIZE_MAX);
	assert(call_index != SIZE_MAX);
	assert(first_branch < call_index);

	// Values are what GDScript produces, and both operators are bools.
	assert(run_int("func test():\n\treturn 1 and 1\n", "test") == 1);
	assert(run_int("func test():\n\treturn 1 and 0\n", "test") == 0);
	assert(run_int("func test():\n\treturn 0 and 1\n", "test") == 0);
	assert(run_int("func test():\n\treturn 0 or 0\n", "test") == 0);
	assert(run_int("func test():\n\treturn 0 or 1\n", "test") == 1);
	assert(run_int("func test():\n\treturn 1 or 0\n", "test") == 1);
	assert(std::holds_alternative<bool>(run("func test():\n\treturn 1 and 1\n", "test")));

	// 5 and 3 is true, not 3: the operators booleanize.
	assert(run_int("func test():\n\treturn 5 and 3\n", "test") == 1);

	std::cout << "  ✓ Short-circuit evaluation" << std::endl;
}

// Truthiness follows Variant::booleanize(). Testing only the payload's low byte
// makes 256 false, and makes any scoped index whose low byte is zero false too.
static void test_truthiness() {
	std::cout << "Testing Variant truthiness..." << std::endl;

	assert(run_int("func test():\n\tif 256:\n\t\treturn 1\n\treturn 0\n", "test") == 1);
	assert(run_int("func test():\n\tif 0:\n\t\treturn 1\n\treturn 0\n", "test") == 0);
	assert(run_int("func test():\n\tif 0.5:\n\t\treturn 1\n\treturn 0\n", "test") == 1);
	assert(run_int("func test():\n\tif 0.0:\n\t\treturn 1\n\treturn 0\n", "test") == 0);

	// A comparison result is a bool, not an int of the operand's type. Tracking
	// it as INT made the backend read eight bytes of a one-byte payload.
	// Unoptimized: the optimizer fuses a comparison and its branch into a
	// BRANCH_LT, which is exactly the branch this is not about.
	const IRProgram ir = compile_to_ir(
		"func test(a: int, b: int):\n\tvar c = a < b\n\tif c:\n\t\treturn 1\n\treturn 0\n", false);
	const IRFunction& test = find_function(ir, "test");
	bool checked = false;
	for (const auto& instr : test.instructions) {
		if (instr.opcode == IROpcode::BRANCH_ZERO && instr.type_hint != IRInstruction::TypeHint_NONE) {
			assert(instr.type_hint == Variant::BOOL);
			checked = true;
		}
	}
	assert(checked);

	std::cout << "  ✓ Variant truthiness" << std::endl;
}

// A global whose every read takes its address directly has no frame copy. The
// branch's truthiness test dropped that base register and handed the host an
// SP-relative pointer anyway, so `if platform:` booleanized whatever sat at the
// bottom of the frame -- reliably false for an object member.
static void test_truthiness_of_a_global_read_in_place() {
	std::cout << "Testing truthiness of a global read in place..." << std::endl;

	// Declared by class name, so the slot carries no Variant type and the branch
	// has to ask the host. `r` keeps the branch off the return register, which is
	// excluded from the frame-copy elision.
	const std::string source =
		"var platform: Sprite2D\n"
		"func test(a):\n"
		"\tvar r = a\n"
		"\tif platform:\n"
		"\t\tr = 1\n"
		"\treturn r\n";

	assert(count_opcode(find_function(compile_to_ir(source), "test"), IROpcode::BRANCH_ZERO) == 1);

	constexpr uint8_t REG_SP = 2;
	constexpr uint8_t REG_A1 = 11;
	constexpr uint8_t REG_A7 = 17;
	const auto is_addi = [](uint32_t instr) {
		return (instr & 0x7F) == 0x13 && ((instr >> 12) & 7) == 0;
	};
	const auto rd_of = [](uint32_t instr) { return uint8_t((instr >> 7) & 0x1F); };
	const auto rs1_of = [](uint32_t instr) { return uint8_t((instr >> 15) & 0x1F); };

	const std::vector<uint8_t> code = compile_to_code(source);
	bool checked = false;
	for (size_t off = 0; off + 4 <= code.size(); off += 4) {
		const uint32_t instr = word_at(code, off);
		// li a7, ECALL_VEVAL
		if (!is_addi(instr) || rd_of(instr) != REG_A7 || rs1_of(instr) != 0 ||
			(int32_t(instr) >> 20) != ECALL_VEVAL) {
			continue;
		}
		// a1 is the operand pointer: the last write of it before the syscall.
		bool found = false;
		for (size_t back = off; back >= 4 && !found; back -= 4) {
			const uint32_t prev = word_at(code, back - 4);
			if (!is_addi(prev) || rd_of(prev) != REG_A1) {
				continue;
			}
			assert(rs1_of(prev) != REG_SP);
			found = true;
		}
		assert(found);
		checked = true;
	}
	assert(checked);

	std::cout << "  ✓ truthiness of a global read in place" << std::endl;
}

// A declared type is a promise about the value, not just a label on it.
static void test_declared_type_coercion() {
	std::cout << "Testing coercion to declared types..." << std::endl;

	// `var f: float = 0` holds 0.0. Folding it as an INT while registering the
	// global as FLOAT hands Godot a Variant of the wrong type.
	{
		const IRProgram ir = compile_to_ir("var f: float = 0\nfunc test():\n\treturn f\n");
		const IRGlobalVar& f = find_global(ir, "f");
		assert(f.init_type == IRGlobalVar::InitType::FLOAT);
		assert(std::get<double>(f.init_value) == 0.0);
		assert(f.value_type == Variant::FLOAT);
	}

	// The same for locals, where the mismatch made the backend read an integer
	// payload as a double.
	{
		const IRProgram ir = compile_to_ir("func test():\n\tvar g: float = 2\n\treturn g\n", false);
		const IRFunction& test = find_function(ir, "test");
		assert(count_opcode(test, IROpcode::CONVERT) == 1);
	}
	assert(std::get<double>(run("func test():\n\tvar g: float = 2\n\treturn g\n", "test")) == 2.0);

	// Mismatches GDScript rejects are rejected here rather than reinterpreted.
	assert(rejects("var s: String = 5\nfunc test():\n\treturn s\n"));
	assert(rejects("func test():\n\tvar i: int = 1.5\n\treturn i\n"));

	std::cout << "  ✓ Coercion to declared types" << std::endl;
}

// Bitwise and shift operators. These are implemented but were never covered, so
// the integer-only rules and the fallback path went unchecked.
static void test_bitwise_and_shifts() {
	std::cout << "Testing bitwise and shift operators..." << std::endl;

	assert(run_int("func test():\n\treturn 12 & 10\n", "test") == 8);
	assert(run_int("func test():\n\treturn 12 | 10\n", "test") == 14);
	assert(run_int("func test():\n\treturn 12 ^ 10\n", "test") == 6);
	assert(run_int("func test():\n\treturn ~0\n", "test") == -1);
	assert(run_int("func test():\n\treturn 1 << 10\n", "test") == 1024);
	assert(run_int("func test():\n\treturn 1024 >> 3\n", "test") == 128);
	assert(run_int("func test():\n\treturn -8 >> 1\n", "test") == -4);

	// Compound assignment forms lower to the same operators.
	assert(run_int("func test():\n\tvar x = 12\n\tx ^= 10\n\treturn x\n", "test") == 6);
	assert(run_int("func test():\n\tvar x = 1\n\tx <<= 4\n\treturn x\n", "test") == 16);
	assert(run_int("func test():\n\tvar x = 64\n\tx >>= 4\n\treturn x\n", "test") == 4);
	assert(run_int("func test():\n\tvar x = 12\n\tx &= 10\n\treturn x\n", "test") == 8);
	assert(run_int("func test():\n\tvar x = 12\n\tx |= 10\n\treturn x\n", "test") == 14);

	// Only two known integers take the native path; anything else falls back to
	// the host, which reports a type error at run time rather than producing
	// nonsense from a float payload.
	{
		const IRProgram typed_ir = compile_to_ir("func test(a: int, b: int):\n\treturn a ^ b\n", false);
		const IRFunction& typed = find_function(typed_ir, "test");
		for (const auto& instr : typed.instructions) {
			if (instr.opcode == IROpcode::BIT_XOR) {
				assert(instr.type_hint == Variant::INT);
			}
		}

		const IRProgram untyped_ir = compile_to_ir("func test(a, b):\n\treturn a ^ b\n", false);
		const IRFunction& untyped = find_function(untyped_ir, "test");
		for (const auto& instr : untyped.instructions) {
			if (instr.opcode == IROpcode::BIT_XOR) {
				assert(instr.type_hint == IRInstruction::TypeHint_NONE);
			}
		}
	}

	std::cout << "  ✓ Bitwise and shift operators" << std::endl;
}

// Uninitialized globals hold an "empty" payload of INT32_MIN, which is the only
// value VASSIGN treats as "no destination yet". Leaving it 0 makes the first
// assignment into a complex global assign through scoped variant 0 instead.
static void test_globals_start_empty() {
	std::cout << "Testing that globals start as empty Variants..." << std::endl;

	IRProgram ir = compile_to_ir("var a: Callable\nfunc test():\n\treturn a\n");
	RISCVCodeGen riscv{ VariantLayout(false) };
	const std::vector<uint8_t> code = riscv.generate(ir);
	const size_t data_size = riscv.get_global_data_size();
	assert(data_size >= 24);

	const size_t data_start = code.size() - data_size;
	int64_t payload = 0;
	for (int i = 7; i >= 0; i--) {
		payload = (payload << 8) | code[data_start + 8 + i];
	}
	assert(payload == int64_t(INT32_MIN));

	std::cout << "  ✓ Globals start as empty Variants" << std::endl;
}

// Loop-invariant code motion hoisted definitions that only run on one path
// through the loop body. A loop containing `if c: x = 1` and `else: x = 0` has
// two invariant definitions of the same register; hoisting either makes the
// register hold that value on both paths. A generated program found it, and it
// produced a wrong answer with nothing else to see.
static void test_licm_leaves_conditional_definitions_alone() {
	std::cout << "Testing that LICM leaves conditional definitions alone..." << std::endl;

	// `or` lowers to exactly that shape: a register set to 1 on the
	// short-circuit path and to 0 on the other, both inside the loop body.
	const std::string source = R"(
func test():
	var taken = 0
	var i = 4
	while i > 0:
		i = i - 1
		if true or false:
			taken = taken + 1
	return taken
)";

	assert(run_int(source, "test") == 4);

	// The same answer with loop-invariant code motion as the only pass, so that
	// a later pass cannot be the one making it right.
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program parsed = parser.parse();
	CodeGenerator codegen;
	IRProgram unoptimized = codegen.generate(parsed);

	IRProgram hoisted = unoptimized;
	IROptimizer optimizer;
	optimizer.set_enabled_passes({ "licm" });
	optimizer.optimize(hoisted);

	IRInterpreter interpreter(hoisted);
	const IRInterpreter::Value value = interpreter.call("test");
	assert(std::get<int64_t>(value) == 4);

	// Rotated loops have an entry test before the body label.  LICM may retain
	// an immediate in that entry path instead of moving it across the label;
	// the interpreter result above is the semantic guard for the conditional
	// definition this regression covers.

	std::cout << "  ✓ LICM leaves conditional definitions alone" << std::endl;
}

int main() {
	std::cout << "=== Compiler Regression Tests ===" << std::endl << std::endl;

	test_locals_shadow_globals();
	test_operand_roles();
	test_store_not_delayed_past_vset();
	test_call_result_kills_constant();
	test_register_types_do_not_leak_between_functions();
	test_large_global_offsets();
	test_global_initializer_forms();
	test_global_init_runs_before_property_registration();
	test_logical_short_circuit();
	test_truthiness();
	test_truthiness_of_a_global_read_in_place();
	test_declared_type_coercion();
	test_bitwise_and_shifts();
	test_globals_start_empty();
	test_licm_leaves_conditional_definitions_alone();

	std::cout << std::endl << "All regression tests passed!" << std::endl;
	return 0;
}
