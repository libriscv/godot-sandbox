// String concatenation folding and ECALL_STRING_SIZE lowering.
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

static int count_vcalls(const IRFunction& func, const std::string& method) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::VCALL && instr.operands.size() >= 3 &&
			std::get<std::string>(instr.operands[2].value) == method) {
			count++;
		}
	}
	return count;
}

static int count_syscalls(const IRFunction& func, int64_t number) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 2 &&
			std::get<int64_t>(instr.operands[1].value) == number) {
			count++;
		}
	}
	return count;
}

static std::vector<int> str_call_arities(const IRFunction& func) {
	std::vector<int> arities;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::GLOBAL_CALL && instr.operands.size() >= 4 &&
			std::get<int64_t>(instr.operands[1].value) == static_cast<int64_t>(GlobalFn::STR)) {
			arities.push_back(static_cast<int>(std::get<int64_t>(instr.operands[3].value)));
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
		assert(count_vcalls(func, "length") == 0);
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
	assert(count_vcalls(untyped, "length") == 1);

	const IRFunction& an_array = find_function(ir, "an_array");
	assert(count_syscalls(an_array, ECALL_STRING_SIZE) == 0);

	compile_to_machine_code(source);

	std::cout << "  ✓ length() on an unknown value keeps the VCALL" << std::endl;
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
	assert(count_vcalls(test, "length") == 0);
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
		test_string_ops_survive_the_optimizer();
	} catch (const CompilerException& e) {
		std::cerr << "Unexpected compiler error: " << e.what() << std::endl;
		return 1;
	}

	std::cout << std::endl << "All string tests passed." << std::endl;
	return 0;
}
