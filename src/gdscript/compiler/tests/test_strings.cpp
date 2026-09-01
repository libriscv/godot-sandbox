// String concatenation folding, ECALL_STRING_SIZE, ECALL_STRING_AT, and `for c in s`.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../globals.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include "../syscall_numbers.h"
#include <cassert>
#include <iostream>
#include <string>

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

static int count_syscalls(const IRFunction& func, int64_t number) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 2 &&
			instr.operands[1].immediate() == number) {
			count++;
		}
	}
	return count;
}

static std::vector<int> str_call_arities(const IRFunction& func) {
	std::vector<int> arities;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::GLOBAL_CALL && instr.operands.size() >= 4 &&
			instr.operands[1].immediate() == static_cast<int64_t>(GlobalFn::STR)) {
			arities.push_back(static_cast<int>(instr.operands[3].immediate()));
		}
	}
	return arities;
}

// Full pipeline through the RISC-V backend.
static void compile_to_machine_code(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	IROptimizer optimizer;
	optimizer.optimize(ir);
	ir_verify(ir, "the optimizer");
	RISCVCodeGen backend;
	const std::vector<uint8_t> code = backend.generate(ir);
	assert(!code.empty());
}

// -= Tests =-

static void test_concatenation_folds_into_str() {
	std::cout << "Testing that a concatenation with str() becomes one str()..." << std::endl;

	const std::string source =
		"func literal(i):\n"
		"\treturn \"n=\" + str(i)\n"
		"\n"
		"func both(i, j):\n"
		"\treturn str(i) + str(j)\n"
		"\n"
		"func leading(i, s : String):\n"
		"\treturn str(i) + s\n";

	const IRProgram ir = compile_to_ir(source);

	const IRFunction& literal = find_function(ir, "literal");
	assert(str_call_arities(literal) == std::vector<int>{ 2 });
	assert(count_opcode(literal, IROpcode::ADD) == 0);

	const IRFunction& both = find_function(ir, "both");
	assert(str_call_arities(both) == std::vector<int>{ 2 });
	assert(count_opcode(both, IROpcode::ADD) == 0);

	const IRFunction& leading = find_function(ir, "leading");
	assert(str_call_arities(leading) == std::vector<int>{ 2 });
	assert(count_opcode(leading, IROpcode::ADD) == 0);

	compile_to_machine_code(source);

	std::cout << "  ✓ a concatenation with str() becomes one str()" << std::endl;
}

static void test_a_chain_folds_into_one_call() {
	std::cout << "Testing that a chain of concatenations folds into one call..." << std::endl;

	const std::string source =
		"func test(i, j, k):\n"
		"\treturn \"a\" + str(i) + \"b\" + str(j) + str(k)\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& test = find_function(ir, "test");

	assert(str_call_arities(test) == std::vector<int>{ 5 });
	assert(count_opcode(test, IROpcode::ADD) == 0);

	compile_to_machine_code(source);

	std::cout << "  ✓ a chain of concatenations folds into one call" << std::endl;
}

static void test_two_plain_strings_keep_the_add() {
	std::cout << "Testing that two plain Strings keep the ADD..." << std::endl;

	const std::string source =
		"func typed(a : String, b : String):\n"
		"\treturn a + b\n"
		"\n"
		"func literals():\n"
		"\tvar a = \"x\"\n"
		"\tvar b = \"y\"\n"
		"\treturn a + b\n";

	const IRProgram ir = compile_to_ir(source);

	const IRFunction& typed = find_function(ir, "typed");
	assert(str_call_arities(typed).empty());
	assert(count_opcode(typed, IROpcode::ADD) == 1);

	const IRFunction& literals = find_function(ir, "literals");
	assert(str_call_arities(literals).empty());
	assert(count_opcode(literals, IROpcode::ADD) == 1);

	compile_to_machine_code(source);

	std::cout << "  ✓ two plain Strings keep the ADD" << std::endl;
}

static void test_folding_stops_at_a_non_string() {
	std::cout << "Testing that folding stops at an operand that is not a String..." << std::endl;

	const std::string source =
		"func untyped(i, j):\n"
		"\treturn i + str(j)\n"
		"\n"
		"func number(i : int, j):\n"
		"\treturn str(j) + i\n";

	const IRProgram ir = compile_to_ir(source);

	const IRFunction& untyped = find_function(ir, "untyped");
	assert(str_call_arities(untyped) == std::vector<int>{ 1 });
	assert(count_opcode(untyped, IROpcode::ADD) == 1);

	const IRFunction& number = find_function(ir, "number");
	assert(str_call_arities(number) == std::vector<int>{ 1 });
	assert(count_opcode(number, IROpcode::ADD) == 1);

	compile_to_machine_code(source);

	std::cout << "  ✓ folding stops at an operand that is not a String" << std::endl;
}

static void test_folding_does_not_cross_control_flow() {
	std::cout << "Testing that a str() is not lifted out of a conditional..." << std::endl;

	const std::string source =
		"func test(a, i, j):\n"
		"\treturn (str(i) if a else str(j)) + str(i)\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& test = find_function(ir, "test");

	const std::vector<int> arities = str_call_arities(test);
	assert(arities.size() == 3);
	for (int arity : arities) {
		assert(arity == 1 || arity == 2);
	}
	assert(count_opcode(test, IROpcode::ADD) == 0);

	compile_to_machine_code(source);

	std::cout << "  ✓ a str() is not lifted out of a conditional" << std::endl;
}

static void test_string_length_is_a_syscall() {
	std::cout << "Testing that length() on a known String is a syscall..." << std::endl;

	const std::string source =
		"func declared(s : String):\n"
		"\treturn s.length()\n"
		"\n"
		"func literal():\n"
		"\tvar s = \"hello\"\n"
		"\treturn s.length()\n"
		"\n"
		"func built(i):\n"
		"\treturn (\"n=\" + str(i)).length()\n"
		"\n"
		"func concatenated(a : String, b : String):\n"
		"\treturn (a + b).length()\n";

	const IRProgram ir = compile_to_ir(source);

	for (const char* name : { "declared", "literal", "built", "concatenated" }) {
		const IRFunction& func = find_function(ir, name);
		assert(count_syscalls(func, ECALL_STRING_SIZE) == 1);
		assert(count_vcalls(ir, func, "length") == 0);
	}

	compile_to_machine_code(source);

	std::cout << "  ✓ length() on a known String is a syscall" << std::endl;
}

static void test_unknown_receiver_keeps_the_vcall() {
	std::cout << "Testing that length() on an unknown value keeps the VCALL..." << std::endl;

	const std::string source =
		"func untyped(x):\n"
		"\treturn x.length()\n"
		"\n"
		"func an_array(a : Array):\n"
		"\treturn a.size()\n";

	const IRProgram ir = compile_to_ir(source);

	const IRFunction& untyped = find_function(ir, "untyped");
	assert(count_syscalls(untyped, ECALL_STRING_SIZE) == 0);
	assert(count_vcalls(ir, untyped, "length") == 1);

	const IRFunction& an_array = find_function(ir, "an_array");
	assert(count_syscalls(an_array, ECALL_STRING_SIZE) == 0);

	compile_to_machine_code(source);

	std::cout << "  ✓ length() on an unknown value keeps the VCALL" << std::endl;
}

static void test_subscript_is_a_syscall() {
	std::cout << "Testing that s[i] on a known String is a syscall..." << std::endl;

	const std::string source =
		"func first(s : String):\n"
		"\treturn s[0]\n"
		"\n"
		"func at(s : String, i : int):\n"
		"\treturn s[i]\n"
		"\n"
		"func last(s : String):\n"
		"\treturn s[-1]\n"
		"\n"
		"func literal():\n"
		"\treturn \"hello\"[1]\n";

	const IRProgram ir = compile_to_ir(source);

	for (const char* name : { "first", "at", "last", "literal" }) {
		const IRFunction& func = find_function(ir, name);
		assert(count_syscalls(func, ECALL_STRING_AT) == 1);
		assert(count_vcalls(ir, func, "get") == 0);
		assert(count_opcode(func, IROpcode::ARRAY_GET) == 0);
	}

	// Negative index normalised by host; no guest-side ECALL_ARRAY_SIZE needed.
	assert(count_syscalls(find_function(ir, "last"), ECALL_ARRAY_SIZE) == 0);

	compile_to_machine_code(source);

	std::cout << "  ✓ s[i] on a known String is a syscall" << std::endl;
}

static void test_a_non_integer_subscript_goes_through_int() {
	std::cout << "Testing that s[1.0] is the character at 1..." << std::endl;

	// Non-integer index goes through int() first.
	const IRProgram ir = compile_to_ir(
		"func f(s : String):\n"
		"\treturn s[1.0]\n"
		"\n"
		"func g(s : String, i):\n"
		"\treturn s[i]\n");

	for (const char* name : { "f", "g" }) {
		const IRFunction& func = find_function(ir, name);
		assert(count_syscalls(func, ECALL_STRING_AT) == 1);
		bool converted = false;
		for (const auto& instr : func.instructions) {
			converted |= instr.opcode == IROpcode::CONVERT || instr.opcode == IROpcode::GLOBAL_CALL;
		}
		assert(converted);
	}

	std::cout << "  ✓ a non-integer subscript is converted first" << std::endl;
}

static void test_an_unknown_subscript_uses_variant_get() {
	std::cout << "Testing that an untyped subscript uses Variant get..." << std::endl;

	// The generic Variant operation preserves the runtime distinction between a
	// String character lookup and every other indexed/keyed lookup.
	const IRProgram ir = compile_to_ir("func f(x, i : int):\n\treturn x[i]\n");
	const IRFunction& func = find_function(ir, "f");
	assert(count_syscalls(func, ECALL_VARIANT_GET) == 1);
	assert(count_syscalls(func, ECALL_STRING_AT) == 0);
	assert(count_vcalls(ir, func, "get") == 0);
	assert(count_opcode(func, IROpcode::TYPE_TEST) == 0);

	// Known Array/Dictionary: no tag test.
	for (const char* hint : { "Array", "Dictionary" }) {
		const IRProgram known = compile_to_ir(
			std::string("func f(c : ") + hint + ", i : int):\n\treturn c[i]\n");
		assert(count_syscalls(find_function(known, "f"), ECALL_STRING_AT) == 0);
		assert(count_opcode(find_function(known, "f"), IROpcode::TYPE_TEST) == 0);
	}

	compile_to_machine_code("func f(x, i : int):\n\treturn x[i]\n");

	std::cout << "  ✓ an untyped subscript uses Variant get" << std::endl;
}

static void test_walking_a_string() {
	std::cout << "Testing 'for c in s'..." << std::endl;

	const IRProgram known = compile_to_ir(
		"func f(s : String):\n"
		"\tvar n = 0\n"
		"\tfor c in s:\n"
		"\t\tn += c.length()\n"
		"\treturn n\n");
	const IRFunction& f = find_function(known, "f");
	// length() on the walk's UTF-32 code point folds to 1. Code-point-only
	// body uses the raw guest buffer: no scoped Strings, no release per refill.
	assert(count_syscalls(f, ECALL_STRING_CODEPOINT_BATCH) == 1);
	assert(count_syscalls(f, ECALL_STRING_BATCH) == 0);
	assert(count_syscalls(f, ECALL_STRING_SIZE) == 0);
	assert(count_syscalls(f, ECALL_STRING_AT) == 0);
	assert(count_opcode(f, IROpcode::CODEPOINT_GET) == 1);

	const IRProgram reassigned = compile_to_ir(
		"func f(s : String):\n"
		"\tvar n = 0\n"
		"\tfor c in s:\n"
		"\t\tc = \"two\"\n"
		"\t\tn += c.length()\n"
		"\treturn n\n");
	const IRFunction& reassigned_function = find_function(reassigned, "f");
	assert(count_syscalls(reassigned_function, ECALL_STRING_SIZE) == 1);
	assert(count_syscalls(reassigned_function, ECALL_STRING_BATCH) == 1);
	assert(count_syscalls(reassigned_function, ECALL_STRING_CODEPOINT_BATCH) == 0);

	// A body runs many times: the assignment below the use still precedes it on
	// the next pass, so the fold has to go for the whole loop, not from the
	// assignment onwards.
	const IRProgram reassigned_in_a_loop = compile_to_ir(
		"func f(s : String, k : int):\n"
		"\tvar n = 0\n"
		"\tfor c in s:\n"
		"\t\tvar d = c\n"
		"\t\tvar i = 0\n"
		"\t\twhile i < k:\n"
		"\t\t\tn += d.length()\n"
		"\t\t\td = d + \"x\"\n"
		"\t\t\ti += 1\n"
		"\treturn n\n");
	assert(count_syscalls(find_function(reassigned_in_a_loop, "f"), ECALL_STRING_SIZE) == 1);
	assert(count_opcode(f, IROpcode::MAKE_SCOPED) == 0);
	assert(count_syscalls(f, ECALL_ARRAY_SIZE) == 0);
	assert(count_syscalls(f, ECALL_ARRAY_AT) == 0);
	assert(count_vcalls(known, f, "size") == 0);
	assert(count_vcalls(known, f, "get") == 0);
	assert(count_opcode(f, IROpcode::TYPE_TEST) == 0);
	// Also verify the frame layout and RISC-V load, not just the IR.
	compile_to_machine_code(
		"func f(s : String):\n"
		"\tvar n = 0\n"
		"\tfor c in s:\n"
		"\t\tn += c.length()\n"
		"\treturn n\n");
	const IRProgram ordinal = compile_to_ir(
		"func f(s : String):\n"
		"\tvar n = 0\n"
		"\tfor c in s:\n"
		"\t\tn += ord(c)\n"
		"\treturn n\n");
	const IRFunction& ordinal_function = find_function(ordinal, "f");
	assert(count_syscalls(ordinal_function, ECALL_STRING_CODEPOINT_BATCH) == 1);
	assert(count_opcode(ordinal_function, IROpcode::GLOBAL_CALL) == 0);
	compile_to_machine_code(
		"func f(s : String):\n"
		"\tvar n = 0\n"
		"\tfor c in s:\n"
		"\t\tn += ord(c)\n"
		"\treturn n\n");

	// Literal String also qualifies. Unused walk variable needs no boxing.
	const IRProgram literal = compile_to_ir(
		"func f():\n"
		"\tfor c in \"hello\":\n"
		"\t\tpass\n");
	assert(count_syscalls(find_function(literal, "f"), ECALL_STRING_CODEPOINT_BATCH) == 1);

	// Untyped: all four arms present (int, Array, String, VCALL).
	const IRProgram untyped = compile_to_ir(
		"func f(it):\n"
		"\tvar n = 0\n"
		"\tfor c in it:\n"
		"\t\tn += 1\n"
		"\treturn n\n");
	const IRFunction& u = find_function(untyped, "f");
	assert(count_syscalls(u, ECALL_STRING_SIZE) == 1);
	assert(count_syscalls(u, ECALL_STRING_AT) == 1);
	assert(count_syscalls(u, ECALL_ARRAY_SIZE) == 1);
	assert(count_syscalls(u, ECALL_ARRAY_AT) == 1);
	assert(count_vcalls(untyped, u, "size") == 1);
	assert(count_vcalls(untyped, u, "get") == 1);

	compile_to_machine_code(
		"func f(it, s : String):\n"
		"\tvar out = []\n"
		"\tfor c in s:\n"
		"\t\tout.append(c)\n"
		"\tfor c in it:\n"
		"\t\tout.append(c)\n"
		"\treturn out\n");

	std::cout << "  ✓ 'for c in s' walks on the String syscalls" << std::endl;
}

// `for i in 2.5`: float counter, counts 0.0, 1.0, 2.0.
static void test_iterating_a_float() {
	std::cout << "Testing 'for i in 2.5'..." << std::endl;

	// Known float: counted loop with float compare and step, no syscall.
	const IRProgram known = compile_to_ir(
		"func f():\n"
		"\tvar n = 0\n"
		"\tfor i in 2.5:\n"
		"\t\tn += 1\n"
		"\treturn n\n"
		"\n"
		"func g(x : float):\n"
		"\tvar n = 0\n"
		"\tfor i in x:\n"
		"\t\tn += 1\n"
		"\treturn n\n");

	for (const char* name : { "f", "g" }) {
		const IRFunction& func = find_function(known, name);
		assert(count_opcode(func, IROpcode::CALL_SYSCALL) == 0);
		assert(count_opcode(func, IROpcode::TYPE_TEST) == 0);
		bool float_bound = false;
		bool float_step = false;
		for (const auto& instr : func.instructions) {
			const bool compare = ir_has_effect(instr.opcode, IR_COMPARISON) ||
				ir_has_effect(instr.opcode, IR_FUSED_BRANCH);
			if (compare && instr.type_hint == Variant::FLOAT) {
				float_bound = true;
			}
			if (instr.opcode == IROpcode::ADD && instr.type_hint == Variant::FLOAT) {
				float_step = true;
			}
		}
		assert(float_bound && "the bound compare was not a float compare");
		assert(float_step && "the step was not a float add");
	}

	// Untyped: float detected via TYPE_TEST, ceil'd into the integer arm.
	const IRProgram untyped = compile_to_ir("func f(it):\n\tfor i in it:\n\t\tpass\n");
	const IRFunction& u = find_function(untyped, "f");
	int float_tests = 0;
	for (const auto& instr : u.instructions) {
		if (instr.opcode == IROpcode::TYPE_TEST &&
			instr.operands.at(2).immediate() == Variant::FLOAT) {
			float_tests++;
		}
	}
	assert(float_tests == 1);
	// ceil() hoisted outside the loop.
	int ceil_calls = 0;
	size_t loop_header = u.instructions.size();
	for (size_t i = 0; i < u.instructions.size(); i++) {
		if (u.instructions[i].opcode == IROpcode::LABEL &&
			untyped.strings[u.instructions[i].operands[0].string_id].find("for_loop") !=
				std::string::npos) {
			loop_header = std::min(loop_header, i);
		}
		if (u.instructions[i].opcode == IROpcode::GLOBAL_CALL) {
			ceil_calls++;
			assert(i < loop_header || loop_header == u.instructions.size());
		}
	}
	assert(ceil_calls == 1);

	compile_to_machine_code(
		"func f(it, x : float):\n"
		"\tvar out = []\n"
		"\tfor i in 2.5:\n"
		"\t\tout.append(i)\n"
		"\tfor i in x:\n"
		"\t\tout.append(i)\n"
		"\tfor i in it:\n"
		"\t\tout.append(i)\n"
		"\treturn out\n");

	std::cout << "  ✓ a float bound counts in floats" << std::endl;
}

static void test_writing_a_character_is_refused() {
	std::cout << "Testing that s[0] = c is refused..." << std::endl;

	// String is a scoped handle; per-character write would alias.
	bool refused = false;
	try {
		compile_to_ir("func f(s : String):\n\ts[0] = \"x\"\n");
	} catch (const CompilerException&) {
		refused = true;
	}
	assert(refused);

	std::cout << "  ✓ assigning to a character is refused" << std::endl;
}

static void test_string_ops_survive_the_optimizer() {
	std::cout << "Testing that the folded calls survive the optimizer..." << std::endl;

	const std::string source =
		"func test(i, n : int):\n"
		"\tvar acc : int = 0\n"
		"\tvar k : int = 0\n"
		"\twhile k < n:\n"
		"\t\tvar s : String = \"value \" + str(k)\n"
		"\t\tacc += s.length()\n"
		"\t\tk += 1\n"
		"\treturn acc\n";

	const IRProgram ir = compile_to_ir(source, true);
	const IRFunction& test = find_function(ir, "test");

	assert(str_call_arities(test) == std::vector<int>{ 2 });
	assert(count_syscalls(test, ECALL_STRING_SIZE) == 1);
	assert(count_vcalls(ir, test, "length") == 0);
	// Every remaining ADD is typed-int (no VEVAL fallback).
	for (const auto& instr : test.instructions) {
		if (instr.opcode == IROpcode::ADD) {
			assert(instr.type_hint == Variant::INT);
		}
	}

	compile_to_machine_code(source);

	std::cout << "  ✓ the folded calls survive the optimizer" << std::endl;
}

int main() {
	std::cout << "=== String Operation Tests ===" << std::endl << std::endl;

	try {
		test_concatenation_folds_into_str();
		test_a_chain_folds_into_one_call();
		test_two_plain_strings_keep_the_add();
		test_folding_stops_at_a_non_string();
		test_folding_does_not_cross_control_flow();
		test_string_length_is_a_syscall();
		test_unknown_receiver_keeps_the_vcall();
		test_subscript_is_a_syscall();
		test_a_non_integer_subscript_goes_through_int();
		test_an_unknown_subscript_uses_variant_get();
		test_walking_a_string();
		test_iterating_a_float();
		test_writing_a_character_is_refused();
		test_string_ops_survive_the_optimizer();
	} catch (const CompilerException& e) {
		std::cerr << "Unexpected compiler error: " << e.what() << std::endl;
		return 1;
	}

	std::cout << std::endl << "All string tests passed." << std::endl;
	return 0;
}
