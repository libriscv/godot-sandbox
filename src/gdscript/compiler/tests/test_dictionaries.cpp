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

// -= Scalar replacement =-

// Anything that reaches the host Dictionary: its construction or an access.
static int count_dictionary_traffic(const IRFunction& func) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		switch (instr.opcode) {
			case IROpcode::MAKE_DICTIONARY:
			case IROpcode::MAKE_DICTIONARY_KEYED:
			case IROpcode::DICT_SET:
			case IROpcode::DICT_GET_CONST:
			case IROpcode::DICT_SET_CONST:
			case IROpcode::DICT_SET_CONST_STR:
			case IROpcode::DICT_HAS_CONST:
				count++;
				break;
			case IROpcode::CALL_SYSCALL:
				count += instr.operands.size() >= 2 &&
					instr.operands[1].immediate() == ECALL_DICTIONARY_OPS;
				break;
			default:
				break;
		}
	}
	return count;
}

static void test_a_local_dictionary_with_constant_keys_lives_in_registers() {
	std::cout << "Testing scalar replacement of a constant-keyed Dictionary..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"func counters(n : int) -> int:\n"
		"\tvar d : Dictionary = {\"hp\": 0, \"mp\": 0}\n"
		"\tvar i : int = 0\n"
		"\twhile i < n:\n"
		"\t\td[\"hp\"] += 1\n"
		"\t\td.mp -= 1\n"
		"\t\ti += 1\n"
		"\treturn d[\"hp\"] + d[\"mp\"]\n"
		"\n"
		"func grown(x : int):\n"
		"\tvar d = {}\n"
		"\tvar before = d.size()\n"
		"\td[\"a\"] = x\n"
		"\td.b = 2\n"
		"\treturn [before, d.size(), d.has(\"a\"), \"c\" in d, d.get(\"c\", 7), d[\"a\"] + d.b]\n"
		"\n"
		"func integer_keys(k : int) -> int:\n"
		"\tvar d = {1: 10, 2: 20}\n"
		"\tfor i in k:\n"
		"\t\tif i & 1:\n"
		"\t\t\td[1] += i\n"
		"\tvar e = d\n"
		"\treturn e[1] + d.get(2, 0)\n"
		"\n"
		"func fresh_each_pass(n : int) -> int:\n"
		"\tvar acc := 0\n"
		"\tfor i in n:\n"
		"\t\tvar d = {\"v\": i}\n"
		"\t\td.v *= 2\n"
		"\t\tacc += d.v\n"
		"\treturn acc\n"
		"\n"
		"func no_parameters():\n"
		"\tvar d = {}\n"
		"\td[\"a\"] = 1\n"
		"\treturn d.size()\n", true);

	for (const char* name : { "counters", "grown", "integer_keys", "fresh_each_pass", "no_parameters" }) {
		assert(count_dictionary_traffic(find_function(ir, name)) == 0);
	}
	// has(), `in` and size() are answered while compiling.
	assert(count_opcode(find_function(ir, "grown"), IROpcode::LOAD_BOOL) == 2);

	std::cout << "  \u2713 a constant-keyed Dictionary lives in registers" << std::endl;
}

static void test_an_escaping_dictionary_stays_materialized() {
	std::cout << "Testing that a Dictionary that escapes is still built..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"func returned(x : int) -> Dictionary:\n"
		"\tvar d = {\"a\": x}\n"
		"\treturn d\n"
		"\n"
		"func passed(x : int):\n"
		"\tvar d = {\"a\": x}\n"
		"\treturn str(d)\n"
		"\n"
		"func stored(x : int):\n"
		"\tvar d = {\"a\": x}\n"
		"\treturn [d]\n"
		"\n"
		"func erased(x : int):\n"
		"\tvar d = {\"a\": x, \"b\": 1}\n"
		"\td.erase(\"a\")\n"
		"\treturn d.size()\n"
		"\n"
		"func computed_key(x : int, k):\n"
		"\tvar d = {\"a\": x}\n"
		"\treturn d[k]\n"
		"\n"
		"func absent_key(x : int):\n"
		"\tvar d = {\"a\": x}\n"
		"\treturn d[\"b\"]\n"
		"\n"
		"func added_on_one_path(c : bool):\n"
		"\tvar d = {\"a\": 1}\n"
		"\tif c:\n"
		"\t\td[\"b\"] = 2\n"
		"\treturn d.size()\n"
		"\n"
		"func bool_key():\n"
		"\tvar d = {1: \"int\", true: \"bool\"}\n"
		"\treturn d[1]\n"
		"\n"
		"func float_key():\n"
		"\tvar d = {1.0: \"float\"}\n"
		"\treturn d.has(1)\n"
		"\n"
		"func reassigned(c : bool) -> int:\n"
		"\tvar d = {\"a\": 1}\n"
		"\tif c:\n"
		"\t\td = {\"a\": 2}\n"
		"\treturn d[\"a\"]\n", true);

	for (const char* name : { "returned", "passed", "stored", "erased", "computed_key", "absent_key",
			"added_on_one_path", "bool_key", "float_key", "reassigned" }) {
		const IRFunction& func = find_function(ir, name);
		assert(count_opcode(func, IROpcode::MAKE_DICTIONARY) >= 1);
	}

	std::cout << "  \u2713 an escaping Dictionary is still built" << std::endl;
}

// -= Fused updates =-

static int count_fused(const IRFunction& func) {
	return count_opcode(func, IROpcode::DICT_OPERATE) +
		count_opcode(func, IROpcode::DICT_OPERATE_CONST);
}

static void test_a_compound_update_is_one_host_call() {
	std::cout << "Testing that d[k] op= v becomes one host call..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"var stats : Dictionary = {}\n"
		"\n"
		"func parameter(d : Dictionary, k, v):\n"
		"\td[k] += v\n"
		"\td[k] -= 1\n"
		"\td[k] *= 2\n"
		"\td[k] |= 1\n"
		"\treturn d\n"
		"\n"
		"func member():\n"
		"\tstats[\"hp\"] += 1\n"
		"\tstats.mp -= 2\n"
		"\n"
		"func computed(d : Dictionary, i : int):\n"
		"\td[i & 15] += 1\n"
		"\td[-(i & 3) + 7] ^= 2\n"
		"\treturn d\n", true);

	const IRFunction& parameter = find_function(ir, "parameter");
	assert(count_opcode(parameter, IROpcode::DICT_OPERATE) == 4);
	assert(count_dict_ops(parameter, Dictionary_Op::GET) == 0);
	assert(count_opcode(parameter, IROpcode::DICT_SET) == 0);

	const IRFunction& member = find_function(ir, "member");
	assert(count_opcode(member, IROpcode::DICT_OPERATE_CONST) == 2);
	assert(count_opcode(member, IROpcode::DICT_GET_CONST) == 0);
	// `stats["hp"]` writes a String key, `stats.mp` a StringName, as before.
	int string_keys = 0;
	for (const auto& instr : member.instructions) {
		if (instr.opcode == IROpcode::DICT_OPERATE_CONST) {
			string_keys += int(instr.operands[4].immediate());
		}
	}
	assert(string_keys == 1);

	assert(count_opcode(find_function(ir, "computed"), IROpcode::DICT_OPERATE) == 2);

	std::cout << "  \u2713 a compound update is one host call" << std::endl;
}

static void test_an_update_that_cannot_move_stays_apart() {
	std::cout << "Testing that an update which cannot move is left alone..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"func f() -> int:\n"
		"\treturn 1\n"
		"\n"
		"func divided(d : Dictionary, k):\n"
		"\td[k] /= 2\n"
		"\td[k] %= 2\n"
		"\td[k] <<= 1\n"
		"\treturn d\n"
		"\n"
		"func a_call_between(d : Dictionary, k):\n"
		"\td[k] += f()\n"
		"\treturn d\n"
		"\n"
		"func str_between(d : Dictionary, k, o):\n"
		"\td[k] += str(o)\n"
		"\treturn d\n"
		"\n"
		"func another_key(d : Dictionary, a, b):\n"
		"\td[a] = d[b] + 1\n"
		"\treturn d\n"
		"\n"
		"func operand_first(d : Dictionary, k):\n"
		"\td[k] = 1 + d[k]\n"
		"\treturn d\n"
		"\n"
		"func value_kept(d : Dictionary, k):\n"
		"\tvar old = d[k]\n"
		"\td[k] = old + 1\n"
		"\treturn old\n"
		"\n"
		"func formatted_between(d : Dictionary, o):\n"
		"\td[\"log\"] += \"%s\" % o\n"
		"\treturn d\n", true);

	// String % Object runs the object's _to_string(), which may touch d.
	for (const char* name : { "divided", "a_call_between", "str_between", "another_key",
			"operand_first", "value_kept", "formatted_between" }) {
		assert(count_fused(find_function(ir, name)) == 0);
	}

	std::cout << "  \u2713 an update that cannot move is left alone" << std::endl;
}

static void test_a_compound_key_is_evaluated_once() {
	std::cout << "Testing that a compound assignment evaluates its key once..." << std::endl;

	// bump() moves counter, so a second evaluation of the key would write d[2].
	const IRProgram ir = compile_to_ir(
		"var counter = 0\n"
		"func bump():\n"
		"\tcounter += 1\n"
		"\treturn 1\n"
		"\n"
		"func untyped(d):\n"
		"\td[counter + 1] += bump()\n"
		"\treturn d\n"
		"\n"
		"func typed(d : Dictionary):\n"
		"\td[-counter] += bump()\n"
		"\treturn d\n"
		"\n"
		"func nested(a, i : int, j : int):\n"
		"\ta[i + 1][j * 2] += bump()\n"
		"\treturn a\n", true);

	for (const char* name : { "untyped", "typed" }) {
		assert(count_opcode(find_function(ir, name), IROpcode::LOAD_GLOBAL) == 1);
	}
	const IRFunction& nested = find_function(ir, "nested");
	assert(count_opcode(nested, IROpcode::MUL) == 1);
	assert(count_opcode(nested, IROpcode::ADD) == 2); // i + 1, then the update itself

	// Only a subscript may be an expression; `(a + b)` is no target.
	bool refused = false;
	try {
		compile_to_ir("func f(a, b):\n\t(a + b) += 1\n", true);
	} catch (const CompilerException& e) {
		refused = std::string(e.what()).find("Invalid target for compound assignment") != std::string::npos;
	}
	assert(refused);

	std::cout << "  \u2713 a compound assignment evaluates its key once" << std::endl;
}

static void test_a_fused_update_picks_its_key_form() {
	std::cout << "Testing the key form of a fused update..." << std::endl;

	const std::vector<uint8_t> int_key = compile_to_machine_code(
		"func f(d : Dictionary, n : int):\n"
		"\tfor i in n:\n"
		"\t\td[i & 7] += 1\n");
	assert(emits_dict_op(int_key, Dictionary_Op::OPERATE_INT_KEY));
	assert(!emits_dict_op(int_key, Dictionary_Op::GET_INT_KEY));

	const std::vector<uint8_t> boxed_key = compile_to_machine_code(
		"func f(d : Dictionary, k):\n"
		"\td[k] += 1\n");
	assert(emits_dict_op(boxed_key, Dictionary_Op::OPERATE));

	const std::vector<uint8_t> raw_key = compile_to_machine_code(
		"func f(d : Dictionary):\n"
		"\td[\"hp\"] += 1\n"
		"\td.mp += 1\n");
	assert(emits_dict_op(raw_key, Dictionary_Op::OPERATE_RAW_STR));
	assert(emits_dict_op(raw_key, Dictionary_Op::OPERATE_RAW));

	std::cout << "  \u2713 a fused update picks its key form" << std::endl;
}

int main() {
	std::cout << "=== Dictionary Access Tests ===" << std::endl << std::endl;

	try {
		test_an_integer_key_travels_as_a_number();
		test_a_constant_string_key_allocates_nothing();
		test_get_with_a_default_has_its_own_op();
		test_a_numeric_read_loop_releases_nothing();
		test_a_declared_scalar_converts_an_unknown_value();
		test_a_local_dictionary_with_constant_keys_lives_in_registers();
		test_an_escaping_dictionary_stays_materialized();
		test_a_compound_update_is_one_host_call();
		test_an_update_that_cannot_move_stays_apart();
		test_a_fused_update_picks_its_key_form();
		test_a_compound_key_is_evaluated_once();
	} catch (const CompilerException& e) {
		std::cerr << "Unexpected compiler error: " << e.what() << std::endl;
		return 1;
	}

	std::cout << std::endl << "All dictionary tests passed." << std::endl;
	return 0;
}
