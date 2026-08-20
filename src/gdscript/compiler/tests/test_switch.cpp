// Dispatch: what a `match` on an integer opcode lowers to.
//
// A logic CPU in GDScript is a fetch-decode-execute loop whose execute step is
// one `match` on the opcode, so the cost of that `match` is the machine's cost.
// Two former costs are pinned down here:
//
//   - `const OP_ADD = 3` reached the arm as a LOAD_GLOBAL, which carries no
//     type, so each arm compared two untyped Variants through
//     Variant::evaluate(): one host syscall per arm.
//   - the arms were a chain of equality tests, so a sixteen-opcode machine paid
//     eight compares per instruction on average.
//
// A const now folds to a typed immediate, and a dense integer match lowers to
// SWITCH: a jump table entered in constant time.
//
// The table is a second lowering, not a replacement, so the key cases here are
// what it must not decide alone: a subject that is not an integer, or is out of
// range, must reach the same arm the compare chain would.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace gdscript;

// -= Helpers =-

static IRProgram compile_to_ir(const std::string& source, bool optimize = false) {
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

static int count_opcode(const IRFunction& func, IROpcode opcode) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

static const IRInstruction& only_switch(const IRFunction& func) {
	const IRInstruction* found = nullptr;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::SWITCH) {
			assert(found == nullptr && "more than one SWITCH in the function");
			found = &instr;
		}
	}
	assert(found != nullptr && "expected a SWITCH");
	return *found;
}

// The label a SWITCH sends `value` to, or "" if the value is out of range.
static std::string switch_target(const IRInstruction& sw, int64_t value) {
	const int64_t base = std::get<int64_t>(sw.operands.at(1).value);
	const int64_t count = std::get<int64_t>(sw.operands.at(2).value);
	if (value < base || value >= base + count) {
		return "";
	}
	return std::get<std::string>(sw.operands.at(3 + (value - base)).value);
}

static int64_t call_int(const IRProgram& ir, const std::string& function,
                        const std::vector<IRInterpreter::Value>& args = {}) {
	IRInterpreter interp(ir);
	return std::get<int64_t>(interp.call(function, args));
}

// Every optimizer pass must leave the IR verifiable. SWITCH table entries are
// branch targets like any other: a pass that deleted one of those labels would
// leave a jump into nothing.
static void verify_through_the_pipeline(const std::string& source) {
	IRProgram ir = compile_to_ir(source, /*optimize=*/false);
	for (const auto& func : ir.functions) {
		ir_verify(func, "codegen");
	}
	IROptimizer optimizer;
	optimizer.optimize(ir);
	for (const auto& func : ir.functions) {
		ir_verify(func, "the optimizer");
	}
}

// -= Constants =-

static void test_const_global_folds_to_an_immediate() {
	std::cout << "Testing that a const global reaches its use as an immediate..." << std::endl;

	const std::string source = R"(
const LIMIT = 42
const NAME = "cpu"
const RATIO = 0.5
const ENABLED = true

func test():
	return LIMIT
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& func = find_function(ir, "test");

	// The type matters more than the saved load: LOAD_GLOBAL carries no type,
	// and every native path downstream needs to know this is an integer.
	assert(count_opcode(func, IROpcode::LOAD_GLOBAL) == 0);
	assert(count_opcode(func, IROpcode::LOAD_IMM) == 1);
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM) {
			assert(std::get<int64_t>(instr.operands.at(1).value) == 42);
			assert(instr.type_hint == Variant::INT);
		}
	}

	assert(call_int(ir, "test") == 42);

	std::cout << "  ✓ A const integer is an immediate at its use" << std::endl;
}

static void test_const_container_stays_a_global() {
	std::cout << "Testing that a const container is still read from the globals..." << std::endl;

	// A container is a handle: every read must yield the same container, so
	// materialising a fresh one per read would be a different program.
	const std::string source = R"(
const TABLE = []

func test():
	return TABLE
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& func = find_function(ir, "test");
	assert(count_opcode(func, IROpcode::LOAD_GLOBAL) == 1);

	std::cout << "  ✓ A const array is read, not rebuilt" << std::endl;
}

static void test_a_local_shadows_a_const() {
	std::cout << "Testing that a local wins over a const of the same name..." << std::endl;

	const std::string source = R"(
const VALUE = 1

func test():
	var VALUE = 9
	return VALUE
)";

	assert(call_int(compile_to_ir(source), "test") == 9);
	assert(call_int(compile_to_ir(source, /*optimize=*/true), "test") == 9);

	std::cout << "  ✓ Folding a const does not reach past a local of that name" << std::endl;
}

// -= When a jump table is built, and when it is not =-

// Sixteen opcodes addressed by consts: the target case.
static const char* OPCODE_MACHINE = R"(
const OP_HALT  = 0
const OP_LOADI = 1
const OP_MOV   = 2
const OP_ADD   = 3
const OP_SUB   = 4
const OP_MUL   = 5
const OP_AND   = 6
const OP_OR    = 7
const OP_XOR   = 8
const OP_SHL   = 9
const OP_SHR   = 10
const OP_LT    = 11
const OP_JMP   = 12
const OP_JZ    = 13
const OP_JNZ   = 14
const OP_OUT   = 15

func step(op : int, a : int, b : int) -> int:
	match op:
		OP_HALT:
			return 0
		OP_LOADI:
			return b
		OP_MOV:
			return a
		OP_ADD:
			return a + b
		OP_SUB:
			return a - b
		OP_MUL:
			return a * b
		OP_AND:
			return a & b
		OP_OR:
			return a | b
		OP_XOR:
			return a ^ b
		OP_SHL:
			return a << b
		OP_SHR:
			return a >> b
		OP_LT:
			return a < b
		OP_JMP:
			return b
		OP_JZ:
			return a
		OP_JNZ:
			return a
		OP_OUT:
			return a + 1000
		_:
			return -1

func test():
	var total = 0
	var op = 0
	while op < 18:
		total = total * 3 + step(op, 12, 3)
		op += 1
	return total
)";

static void test_dense_match_becomes_a_jump_table() {
	std::cout << "Testing that a dense opcode match becomes a jump table..." << std::endl;

	const IRProgram ir = compile_to_ir(OPCODE_MACHINE);
	const IRFunction& step = find_function(ir, "step");
	const IRInstruction& sw = only_switch(step);

	assert(std::get<int64_t>(sw.operands.at(1).value) == 0);   // base
	assert(std::get<int64_t>(sw.operands.at(2).value) == 16);  // one entry per opcode
	assert(sw.operands.size() == 3 + 16);

	// The subject is declared `int`, so the table decides the whole match and no
	// compare chain follows it.
	assert(sw.type_hint == Variant::INT);
	assert(count_opcode(step, IROpcode::CMP_EQ) == 0);

	// Sixteen distinct arms, all reachable, none the wildcard.
	std::set<std::string> targets;
	for (int64_t op = 0; op < 16; op++) {
		targets.insert(switch_target(sw, op));
	}
	assert(targets.size() == 16);

	std::cout << "  ✓ Sixteen opcodes, one table, no compares" << std::endl;
}

static void test_the_machine_still_computes_the_same_answers() {
	std::cout << "Testing that the machine answers the same optimized and not..." << std::endl;

	const int64_t plain = call_int(compile_to_ir(OPCODE_MACHINE), "test");
	const int64_t optimized = call_int(compile_to_ir(OPCODE_MACHINE, /*optimize=*/true), "test");
	assert(plain == optimized);

	// Spot-check individual arms: an off-by-one in the table still sums to
	// something plausible.
	const IRProgram ir = compile_to_ir(OPCODE_MACHINE, /*optimize=*/true);
	assert(call_int(ir, "step", { int64_t(0), int64_t(12), int64_t(3) }) == 0);      // HALT
	assert(call_int(ir, "step", { int64_t(3), int64_t(12), int64_t(3) }) == 15);     // ADD
	assert(call_int(ir, "step", { int64_t(4), int64_t(12), int64_t(3) }) == 9);      // SUB
	assert(call_int(ir, "step", { int64_t(5), int64_t(12), int64_t(3) }) == 36);     // MUL
	assert(call_int(ir, "step", { int64_t(9), int64_t(12), int64_t(3) }) == 96);     // SHL
	assert(call_int(ir, "step", { int64_t(10), int64_t(12), int64_t(3) }) == 1);     // SHR
	assert(call_int(ir, "step", { int64_t(15), int64_t(12), int64_t(3) }) == 1012);  // OUT
	assert(call_int(ir, "step", { int64_t(16), int64_t(12), int64_t(3) }) == -1);    // past the table
	assert(call_int(ir, "step", { int64_t(-1), int64_t(12), int64_t(3) }) == -1);    // below the table

	verify_through_the_pipeline(OPCODE_MACHINE);

	std::cout << "  ✓ Every arm answers what it did before" << std::endl;
}

static void test_a_short_match_keeps_the_compares() {
	std::cout << "Testing that a small match is not worth a table..." << std::endl;

	// Three arms: the table setup costs about what the compares do, and the
	// compares cost no code size.
	const std::string source = R"(
func pick(n : int) -> int:
	match n:
		0:
			return 10
		1:
			return 20
		2:
			return 30
		_:
			return 40

func test():
	return pick(0) + pick(1) + pick(2) + pick(3)
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& pick = find_function(ir, "pick");
	assert(count_opcode(pick, IROpcode::SWITCH) == 0);
	assert(count_opcode(pick, IROpcode::CMP_EQ) == 3);
	assert(call_int(ir, "test") == 100);

	std::cout << "  ✓ Three arms stay three compares" << std::endl;
}

static void test_a_sparse_match_keeps_the_compares() {
	std::cout << "Testing that a sparse match is not worth a table..." << std::endl;

	// Five values spread over ten thousand: the table would be nearly all holes.
	const std::string source = R"(
func pick(n : int) -> int:
	match n:
		0:
			return 1
		10:
			return 2
		100:
			return 3
		1000:
			return 4
		10000:
			return 5
		_:
			return 0

func test():
	return pick(0) + pick(10) + pick(100) + pick(1000) + pick(10000) + pick(7)
)";

	const IRProgram ir = compile_to_ir(source);
	assert(count_opcode(find_function(ir, "pick"), IROpcode::SWITCH) == 0);
	assert(call_int(ir, "test") == 15);

	std::cout << "  ✓ Five values across ten thousand stay compares" << std::endl;
}

static void test_a_non_constant_pattern_forbids_the_table() {
	std::cout << "Testing that one non-constant pattern rules out the table..." << std::endl;

	// `limit` may hold 2, in which case GDScript takes the first arm. A table
	// jumping straight to the `2:` body would run the wrong one, so one
	// unevaluable pattern disqualifies the whole match.
	const std::string source = R"(
func pick(n : int, limit : int) -> int:
	match n:
		limit:
			return 1
		0:
			return 2
		1:
			return 3
		2:
			return 4
		3:
			return 5
		_:
			return 6

func test():
	return pick(2, 2) * 10 + pick(2, 9)
)";

	const IRProgram ir = compile_to_ir(source);
	assert(count_opcode(find_function(ir, "pick"), IROpcode::SWITCH) == 0);
	assert(call_int(ir, "test") == 14); // first arm for n == limit, then the 2: arm

	std::cout << "  ✓ A variable pattern keeps the first-match order honest" << std::endl;
}

// -= What the table must not decide by itself =-

static void test_an_untyped_subject_keeps_the_chain_behind_the_table() {
	std::cout << "Testing that an untyped subject keeps its compares..." << std::endl;

	// `match 3.0` must reach the `3:` arm, and `match true` the `1:` arm. The
	// table handles only integers, so unless the subject is known to be one the
	// compare chain stays behind it and catches every fall-through.
	const std::string source = R"(
func pick(n):
	match n:
		0:
			return 10
		1:
			return 11
		2:
			return 12
		3:
			return 13
		4:
			return 14
		_:
			return -1

func test():
	return pick(3) * 1000000 + pick(3.0) * 10000 + pick(true) * 100 + pick(2.5)
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& pick = find_function(ir, "pick");

	const IRInstruction& sw = only_switch(pick);
	assert(sw.type_hint == IRInstruction::TypeHint_NONE); // so the backend tests the type
	assert(count_opcode(pick, IROpcode::CMP_EQ) == 5);    // and the chain is still there

	// 13 via the table, 13 via the chain for the float, 11 for the bool, and the
	// wildcard for a float equal to nothing.
	assert(call_int(ir, "test") == 13131100 - 1);

	verify_through_the_pipeline(source);

	std::cout << "  ✓ A float and a bool still find their integer arm" << std::endl;
}

static void test_holes_and_duplicates() {
	std::cout << "Testing holes, a negative base and a value named twice..." << std::endl;

	const std::string source = R"(
const LOWEST = -4

func classify(v : int) -> int:
	var out = 0
	match v:
		LOWEST:
			out = 1
		-2, -1:
			out = 2
		1:
			out = 3
		3:
			out = 4
		LOWEST:
			out = 999
	return out

func test():
	return 0
)";

	const IRProgram ir = compile_to_ir(source);
	const IRInstruction& sw = only_switch(find_function(ir, "classify"));

	assert(std::get<int64_t>(sw.operands.at(1).value) == -4);
	assert(std::get<int64_t>(sw.operands.at(2).value) == 8); // -4 .. 3

	// -2 and -1 share an arm; -3, 0 and 2 are holes and go where falling out of
	// the match goes; the second LOWEST is unreachable, as in the compare chain.
	assert(switch_target(sw, -2) == switch_target(sw, -1));
	assert(switch_target(sw, -3) == switch_target(sw, 0));
	assert(switch_target(sw, 0) == switch_target(sw, 2));
	assert(switch_target(sw, -4) != switch_target(sw, -3));
	assert(switch_target(sw, -4) != switch_target(sw, 3));
	assert(switch_target(sw, 4) == ""); // past the end of the table

	IRProgram optimized = compile_to_ir(source, /*optimize=*/true);
	for (int64_t v = -6; v < 6; v++) {
		const int64_t expected =
			(v == -4) ? 1 : (v == -2 || v == -1) ? 2 : (v == 1) ? 3 : (v == 3) ? 4 : 0;
		// A match with no wildcard and no arm for `v` falls out and runs what
		// follows, leaving `out` unchanged.
		assert(call_int(optimized, "classify", { v }) == expected);
	}

	std::cout << "  ✓ Holes fall out of the match and the first arm wins" << std::endl;
}

static void test_a_wildcard_is_not_a_table_entry() {
	std::cout << "Testing that the wildcard body is where holes go..." << std::endl;

	const std::string source = R"(
func pick(n : int) -> int:
	match n:
		0:
			return 1
		1:
			return 2
		3:
			return 3
		4:
			return 4
		_:
			return 99

func test():
	return 0
)";

	const IRProgram ir = compile_to_ir(source);
	const IRInstruction& sw = only_switch(find_function(ir, "pick"));
	assert(std::get<int64_t>(sw.operands.at(2).value) == 5); // 0 .. 4

	// The hole at 2 targets the wildcard's body, not an arm of its own.
	const std::string hole = switch_target(sw, 2);
	assert(!hole.empty());
	assert(hole != switch_target(sw, 0));
	assert(hole != switch_target(sw, 4));

	const IRProgram optimized = compile_to_ir(source, /*optimize=*/true);
	assert(call_int(optimized, "pick", { int64_t(2) }) == 99);
	assert(call_int(optimized, "pick", { int64_t(9) }) == 99);
	assert(call_int(optimized, "pick", { int64_t(-1) }) == 99);

	std::cout << "  ✓ A hole and an out-of-range subject both reach the wildcard" << std::endl;
}

static void test_break_and_return_inside_an_arm() {
	std::cout << "Testing control flow out of a table arm..." << std::endl;

	// The arms are emitted after the table rather than between the compares, so
	// `break` and `continue` in an arm must still refer to the enclosing loop.
	const std::string source = R"(
func test():
	var total = 0
	var i = 0
	while i < 20:
		i += 1
		match i:
			1:
				total += 1
			2:
				continue
			3:
				total += 100
			4:
				total += 1000
			5:
				break
			_:
				total += 10000
	return total * 10 + i
)";

	const IRProgram ir = compile_to_ir(source, /*optimize=*/true);
	assert(count_opcode(find_function(ir, "test"), IROpcode::SWITCH) == 1);
	assert(call_int(ir, "test") == 11015);

	verify_through_the_pipeline(source);

	std::cout << "  ✓ break and continue still mean the loop" << std::endl;
}

// -= The backend =-

static void test_the_table_reaches_riscv() {
	std::cout << "Testing that the machine compiles to RISC-V..." << std::endl;

	IRProgram ir = compile_to_ir(OPCODE_MACHINE, /*optimize=*/true);
	RISCVCodeGen codegen;
	const std::vector<uint8_t> code = codegen.generate(ir);
	assert(!code.empty());

	// The table is `count` consecutive jumps in the instruction stream, so the
	// dispatch must end in an indirect jump followed by sixteen JALs. Checking
	// the encoding here catches a wrong stride, which every higher-level test
	// would still pass.
	size_t jalr_at = 0;
	bool found = false;
	for (size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
		uint32_t instr = 0;
		std::memcpy(&instr, &code[offset], 4);
		if ((instr & 0x7F) != 0x67) {
			continue; // not a JALR
		}
		if (((instr >> 7) & 0x1F) != 0) {
			continue; // not `jr`: a JALR that keeps a return address is a call
		}
		size_t jumps = 0;
		for (size_t entry = offset + 4; entry + 4 <= code.size(); entry += 4) {
			uint32_t word = 0;
			std::memcpy(&word, &code[entry], 4);
			if ((word & 0x7F) != 0x6F) {
				break;
			}
			jumps++;
		}
		if (jumps >= 16) {
			jalr_at = offset;
			found = true;
			break;
		}
	}
	assert(found && "no jump table in the generated code");

	// Immediately before the indirect jump: sh2add, scaling the index by the 4
	// bytes per table entry. A wrong scale lands in the middle of the table.
	uint32_t sh2add = 0;
	std::memcpy(&sh2add, &code[jalr_at - 4], 4);
	assert((sh2add & 0x7F) == 0x33);          // OP
	assert(((sh2add >> 12) & 0x7) == 4);      // funct3
	assert(((sh2add >> 25) & 0x7F) == 0x10);  // funct7: Zba

	std::cout << "  ✓ The table is sixteen jumps behind an indirect branch" << std::endl;
}

static void test_a_huge_range_is_refused() {
	std::cout << "Testing that a spread-out match cannot ask for a huge table..." << std::endl;

	// Six arms hundreds of thousands apart: a table covering them would be most
	// of a megabyte of jumps for six destinations.
	const std::string source = R"(
func pick(n : int) -> int:
	match n:
		0:
			return 1
		100000:
			return 2
		200000:
			return 3
		300000:
			return 4
		400000:
			return 5
		500000:
			return 6
		_:
			return 0

func test():
	return pick(0) + pick(300000) + pick(7)
)";

	const IRProgram ir = compile_to_ir(source);
	assert(count_opcode(find_function(ir, "pick"), IROpcode::SWITCH) == 0);
	assert(call_int(ir, "test") == 5);

	std::cout << "  ✓ No table, and no megabyte of jumps" << std::endl;
}

int main() {
	std::cout << "=== Dispatch Tests ===" << std::endl << std::endl;

	test_const_global_folds_to_an_immediate();
	test_const_container_stays_a_global();
	test_a_local_shadows_a_const();

	test_dense_match_becomes_a_jump_table();
	test_the_machine_still_computes_the_same_answers();
	test_a_short_match_keeps_the_compares();
	test_a_sparse_match_keeps_the_compares();
	test_a_non_constant_pattern_forbids_the_table();

	test_an_untyped_subject_keeps_the_chain_behind_the_table();
	test_holes_and_duplicates();
	test_a_wildcard_is_not_a_table_entry();
	test_break_and_return_inside_an_arm();

	test_the_table_reaches_riscv();
	test_a_huge_range_is_refused();

	std::cout << std::endl << "All dispatch tests passed!" << std::endl;
	return 0;
}
