// Dictionary access lowering: key form (codegen), emitted op (riscv_codegen),
// and scope planning. Engine-visible behaviour is in test_gdscript_compiler.gd.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include "../syscall_numbers.h"
#include <cassert>
#include <iostream>
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
		ir_verify(ir, "the optimizer");
	}
	return ir;
}

static const IRFunction& find_function(const IRProgram& ir, const std::string& name) {
	for (const auto& func : ir.functions) {
		if (func.name == name) return func;
	}
	throw std::runtime_error("Function not found: " + name);
}

static int count_opcode(const IRFunction& func, IROpcode opcode) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == opcode) count++;
	}
	return count;
}

static int count_dict_ops(const IRFunction& func, Dictionary_Op op) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 3 &&
			instr.operands[1].immediate() == ECALL_DICTIONARY_OPS &&
			instr.operands[2].immediate() == dictionary_op(op)) {
			count++;
		}
	}
	return count;
}

static int count_vcalls(const IRProgram& ir, const IRFunction& func, const std::string& method) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::VCALL && instr.operands.size() >= 3 &&
			ir.strings[instr.operands[2].string_id] == method) {
			count++;
		}
	}
	return count;
}

static std::vector<uint8_t> compile_to_machine_code(const std::string& source) {
	IRProgram ir = compile_to_ir(source, true);
	RISCVCodeGen backend;
	std::vector<uint8_t> code = backend.generate(ir);
	assert(!code.empty());
	return code;
}

// The backend picks the op, so scan the instruction stream for `li a0, <op>`.
static bool emits_li(const std::vector<uint8_t>& code, uint8_t reg, int32_t value) {
	for (size_t i = 0; i + 4 <= code.size(); i += 4) {
		uint32_t insn = uint32_t(code[i]) | uint32_t(code[i + 1]) << 8 |
			uint32_t(code[i + 2]) << 16 | uint32_t(code[i + 3]) << 24;
		if ((insn & 0x7f) != 0x13) continue;              // OP-IMM
		if (((insn >> 12) & 0x7) != 0) continue;          // ADDI
		if (((insn >> 15) & 0x1f) != 0) continue;         // rs1 == zero
		if (((insn >> 7) & 0x1f) != reg) continue;
		if (int32_t(insn) >> 20 == value) return true;
	}
	return false;
}

static bool emits_dict_op(const std::vector<uint8_t>& code, Dictionary_Op op) {
	return emits_li(code, 10 /* a0 */, int32_t(dictionary_op(op)));
}

// -= Key forms =-

static void test_an_integer_key_travels_as_a_number() {
	std::cout << "Testing that an integer key is not boxed..." << std::endl;

	// Int-key ops only fire inside loops (scalar residency requires a back edge).
	const std::vector<uint8_t> code = compile_to_machine_code(
		"func write(d : Dictionary, n : int, v):\n"
		"\tvar i : int = 0\n"
		"\twhile i < n:\n"
		"\t\td[i] = v\n"
		"\t\ti += 1\n"
		"\n"
		"func read(d : Dictionary, n : int):\n"
		"\tvar i : int = 0\n"
		"\twhile i < n:\n"
		"\t\td[i]\n"
		"\t\ti += 1\n"
		"\n"
		"func present(d : Dictionary, n : int) -> bool:\n"
		"\tvar i : int = 0\n"
		"\tvar found : bool = false\n"
		"\twhile i < n:\n"
		"\t\tfound = d.has(i)\n"
		"\t\ti += 1\n"
		"\treturn found\n");
	assert(emits_dict_op(code, Dictionary_Op::SET_INT_KEY));
	assert(emits_dict_op(code, Dictionary_Op::GET_INT_KEY));
	assert(emits_dict_op(code, Dictionary_Op::HAS_INT_KEY));

	// Float and untyped keys stay boxed.
	const std::vector<uint8_t> boxed = compile_to_machine_code(
		"func write(d : Dictionary, n : int, v):\n"
		"\tvar f : float = 0.0\n"
		"\twhile f < float(n):\n"
		"\t\td[f] = v\n"
		"\t\tf += 1.0\n"
		"\n"
		"func read(d : Dictionary, k, n : int):\n"
		"\tvar i : int = 0\n"
		"\twhile i < n:\n"
		"\t\td[k]\n"
		"\t\ti += 1\n");
	assert(!emits_dict_op(boxed, Dictionary_Op::SET_INT_KEY));
	assert(!emits_dict_op(boxed, Dictionary_Op::GET_INT_KEY));
	assert(emits_dict_op(boxed, Dictionary_Op::SET));
	assert(emits_dict_op(boxed, Dictionary_Op::GET));

	std::cout << "  \u2713 an integer key is not boxed" << std::endl;
}

static void test_a_constant_string_key_allocates_nothing() {
	std::cout << "Testing that a literal key skips its scoped String..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"func read(d : Dictionary):\n"
		"\treturn d[\"hp\"]\n"
		"\n"
		"func write(d : Dictionary, v):\n"
		"\td[\"hp\"] = v\n"
		"\n"
		"func bump(d : Dictionary):\n"
		"\td[\"hp\"] += 1\n");

	const IRFunction& read = find_function(ir, "read");
	assert(count_opcode(read, IROpcode::DICT_GET_CONST) == 1);
	assert(count_opcode(read, IROpcode::LOAD_STRING) == 0);
	assert(count_dict_ops(read, Dictionary_Op::GET) == 0);

	const IRFunction& write = find_function(ir, "write");
	assert(count_opcode(write, IROpcode::DICT_SET_CONST_STR) == 1);
	assert(count_opcode(write, IROpcode::DICT_SET) == 0);
	assert(count_opcode(write, IROpcode::LOAD_STRING) == 0);

	// A compound assignment reads and writes through the same constant key.
	const IRFunction& bump = find_function(ir, "bump");
	assert(count_opcode(bump, IROpcode::DICT_GET_CONST) == 1);
	assert(count_opcode(bump, IROpcode::DICT_SET_CONST_STR) == 1);
	assert(count_opcode(bump, IROpcode::LOAD_STRING) == 0);

	// Plain Dictionary keys are Strings; struct keys are StringNames.
	const std::vector<uint8_t> code = compile_to_machine_code(
		"func write(d : Dictionary, v):\n\td[\"hp\"] = v\n");
	assert(emits_dict_op(code, Dictionary_Op::SET_RAW_STR));
	assert(!emits_dict_op(code, Dictionary_Op::SET_RAW));

	// &"hp" (StringName) and ^"hp" (NodePath) are not String keys.
	const IRProgram typed_literals = compile_to_ir(
		"func read(d : Dictionary):\n"
		"\treturn d[&\"hp\"]\n");
	assert(count_opcode(find_function(typed_literals, "read"), IROpcode::DICT_GET_CONST) == 0);

	std::cout << "  \u2713 a literal key skips its scoped String" << std::endl;
}

static void test_get_with_a_default_has_its_own_op() {
	std::cout << "Testing that get(key, default) is not a VCALL..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"func fallback(d : Dictionary, k):\n"
		"\treturn d.get(k, 0)\n"
		"\n"
		"func unknown(d, k):\n"
		"\treturn d.get(k, 0)\n");

	const IRFunction& fallback = find_function(ir, "fallback");
	assert(count_dict_ops(fallback, Dictionary_Op::GET_OR_DEFAULT) == 1);
	assert(count_vcalls(ir, fallback, "get") == 0);
	// Not GET_OR_ADD: the default is answered, never inserted.
	assert(count_dict_ops(fallback, Dictionary_Op::GET_OR_ADD) == 0);

	// The receiver's type is what decides, not the argument count.
	const IRFunction& unknown = find_function(ir, "unknown");
	assert(count_vcalls(ir, unknown, "get") == 1);
	assert(count_dict_ops(unknown, Dictionary_Op::GET_OR_DEFAULT) == 0);

	compile_to_machine_code("func fallback(d : Dictionary, k):\n\treturn d.get(k, 0)\n");

	std::cout << "  \u2713 get(key, default) is not a VCALL" << std::endl;
}

// -= Scopes =-

static void test_a_numeric_read_loop_releases_nothing() {
	std::cout << "Testing that a read loop derives its dirty flag..." << std::endl;

	const std::string source =
		"func total(d : Dictionary, n : int) -> int:\n"
		"\tvar acc : int = 0\n"
		"\tvar i : int = 0\n"
		"\twhile i < n:\n"
		"\t\tacc += d[i]\n"
		"\t\ti += 1\n"
		"\treturn acc\n";

	const IRProgram ir = compile_to_ir(source, true);
	const IRFunction& total = find_function(ir, "total");
	assert(count_opcode(total, IROpcode::SCOPE_MARK) >= 1);
	// COERCE keeps `acc` a fixed scalar across the loop.
	assert(count_opcode(total, IROpcode::COERCE) >= 1);

	compile_to_machine_code(source);

	std::cout << "  \u2713 a read loop derives its dirty flag" << std::endl;
}

static void test_a_declared_scalar_converts_an_unknown_value() {
	std::cout << "Testing that an unknown value converts into a declared slot..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"var stored : int = 0\n"
		"\n"
		"func into_local(d : Dictionary):\n"
		"\tvar hp : int = d[\"hp\"]\n"
		"\treturn hp\n"
		"\n"
		"func into_member(d : Dictionary):\n"
		"\tstored = d[\"hp\"]\n"
		"\n"
		"func out_of_return(d : Dictionary) -> int:\n"
		"\treturn d[\"hp\"]\n"
		"\n"
		"func untyped(d : Dictionary):\n"
		"\tvar hp = d[\"hp\"]\n"
		"\treturn hp\n");

	assert(count_opcode(find_function(ir, "into_local"), IROpcode::COERCE) == 1);
	assert(count_opcode(find_function(ir, "into_member"), IROpcode::COERCE) == 1);
	assert(count_opcode(find_function(ir, "out_of_return"), IROpcode::COERCE) == 1);
	// Nothing was declared, so nothing is converted.
	assert(count_opcode(find_function(ir, "untyped"), IROpcode::COERCE) == 0);

	std::cout << "  \u2713 an unknown value converts into a declared slot" << std::endl;
}

int main() {
	std::cout << "=== Dictionary Access Tests ===" << std::endl << std::endl;

	try {
		test_an_integer_key_travels_as_a_number();
		test_a_constant_string_key_allocates_nothing();
		test_get_with_a_default_has_its_own_op();
		test_a_numeric_read_loop_releases_nothing();
		test_a_declared_scalar_converts_an_unknown_value();
	} catch (const CompilerException& e) {
		std::cerr << "Unexpected compiler error: " << e.what() << std::endl;
		return 1;
	}

	std::cout << std::endl << "All dictionary tests passed." << std::endl;
	return 0;
}
