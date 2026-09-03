#include "../codegen.h"
#include "../compiler_exception.h"
#include "../function_signature.h"
#include "../lexer.h"
#include "../parser.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

using namespace gdscript;

static IRProgram compile_to_ir(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	return codegen.generate(program);
}

static const IRGlobalVar& global(const IRProgram& ir, const std::string& name) {
	for (const auto& entry : ir.globals) {
		if (entry.name == name) {
			return entry;
		}
	}
	throw std::runtime_error("Global not found: " + name);
}

static const FunctionSignature& find_signature(const IRProgram& ir, const std::string& name) {
	for (const auto& sig : ir.signatures) {
		if (sig.name == name) {
			return sig;
		}
	}
	throw std::runtime_error("Signature not found: " + name);
}

static const IRFunction& find_function(const IRProgram& ir, const std::string& name) {
	for (const auto& func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	throw std::runtime_error("Function not found: " + name);
}

static bool has_opcode(const IRFunction& func, IROpcode opcode) {
	for (const auto& instr : func.instructions) {
		if (instr.opcode == opcode) {
			return true;
		}
	}
	return false;
}

static std::string compile_error(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException& e) {
		return e.what();
	}
	return "";
}

static void test_an_operator_over_constants_is_a_constant() {
	const IRProgram ir = compile_to_ir(
		"const MASK = 1 << 3\n"
		"const HALF = 7 / 2\n"
		"const SCALED = 2.5 * 4\n"
		"const NAME = \"a\" + \"b\"\n"
		"const BOTH = true and false\n"
		"const SMALL = 3 < 4\n"
		"const CUBE = 2 ** 10\n");

	assert(global(ir, "MASK").init_type == IRGlobalVar::InitType::INT);
	assert(std::get<int64_t>(global(ir, "MASK").init_value) == 8);
	assert(std::get<int64_t>(global(ir, "HALF").init_value) == 3);
	assert(global(ir, "SCALED").init_type == IRGlobalVar::InitType::FLOAT);
	assert(std::get<double>(global(ir, "SCALED").init_value) == 10.0);
	assert(global(ir, "NAME").init_type == IRGlobalVar::InitType::STRING);
	assert(std::get<std::string>(global(ir, "NAME").init_value) == "ab");
	assert(global(ir, "BOTH").init_type == IRGlobalVar::InitType::BOOL);
	assert(std::get<bool>(global(ir, "BOTH").init_value) == false);
	assert(std::get<bool>(global(ir, "SMALL").init_value) == true);
	assert(std::get<int64_t>(global(ir, "CUBE").init_value) == 1024);

	assert(ir.global_init.instructions.empty());

	std::cout << "  ✓ an operator over constants folds, leaving no startup code" << std::endl;
}

static void test_a_constant_names_another_constant() {
	const IRProgram ir = compile_to_ir(
		"const A = 5\n"
		"const B = A * 2\n"
		"const C = B - A\n"
		"func f() -> int:\n"
		"\treturn C\n");

	assert(std::get<int64_t>(global(ir, "B").init_value) == 10);
	assert(std::get<int64_t>(global(ir, "C").init_value) == 5);
	assert(!has_opcode(find_function(ir, "f"), IROpcode::LOAD_GLOBAL));

	std::cout << "  ✓ a constant chain folds and its uses become immediates" << std::endl;
}

static void test_engine_constants_fold() {
	const IRProgram ir = compile_to_ir(
		"const HALF_PI = PI / 2\n"
		"const ESCAPE = KEY_ESCAPE\n"
		"const LEFT = Side.SIDE_LEFT\n");

	assert(global(ir, "HALF_PI").init_type == IRGlobalVar::InitType::FLOAT);
	assert(std::fabs(std::get<double>(global(ir, "HALF_PI").init_value) - 1.5707963267948966) < 1e-12);
	assert(global(ir, "ESCAPE").init_type == IRGlobalVar::InitType::INT);
	assert(global(ir, "LEFT").init_type == IRGlobalVar::InitType::INT);
	assert(std::get<int64_t>(global(ir, "LEFT").init_value) == 0);

	std::cout << "  ✓ @GlobalScope constants and enum members fold" << std::endl;
}

static void test_a_ternary_over_constants_folds() {
	const IRProgram ir = compile_to_ir(
		"const DEBUG = true\n"
		"const LIMIT = 100 if DEBUG else 10\n"
		"const EMPTY = 1 if \"\" else 2\n");

	assert(std::get<int64_t>(global(ir, "LIMIT").init_value) == 100);
	assert(std::get<int64_t>(global(ir, "EMPTY").init_value) == 2);

	std::cout << "  ✓ a ternary with a constant condition folds to the taken arm" << std::endl;
}

static void test_a_script_enum_member_folds_into_a_constant() {
	const IRProgram ir = compile_to_ir(
		"enum Flags { A = 1, B = 2 }\n"
		"const BOTH = Flags.A | Flags.B\n");

	assert(std::get<int64_t>(global(ir, "BOTH").init_value) == 3);

	std::cout << "  ✓ a script enum member folds into a constant" << std::endl;
}

static void test_a_constant_folds_into_an_enum() {
	const IRProgram ir = compile_to_ir(
		"const BASE = 4\n"
		"enum Codes { A = BASE, B, C = BASE * 2 }\n"
		"func f() -> int:\n"
		"\treturn Codes.B + Codes.C\n");

	const IRFunction& func = find_function(ir, "f");
	assert(!has_opcode(func, IROpcode::LOAD_GLOBAL));

	bool saw_five = false;
	bool saw_eight = false;
	for (const auto& instr : func.instructions) {
		if (instr.opcode != IROpcode::LOAD_IMM || instr.operands.size() < 2) {
			continue;
		}
		saw_five = saw_five || instr.operands[1].immediate() == 5;
		saw_eight = saw_eight || instr.operands[1].immediate() == 8;
	}
	assert(saw_five && saw_eight);

	std::cout << "  ✓ an enum member may be written in terms of a constant" << std::endl;
}

static void test_a_class_constant_may_be_an_expression() {
	const IRProgram ir = compile_to_ir(
		"const SHIFT = 3\n"
		"class Foo:\n"
		"\tconst BITS = 1 << SHIFT\n"
		"\tconst DOUBLE = BITS * 2\n"
		"\tfunc bits() -> int:\n"
		"\t\treturn DOUBLE\n");

	const IRFunction& func = find_function(ir, "@Foo.bits");
	bool saw_sixteen = false;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM && instr.operands.size() > 1 &&
			instr.operands[1].immediate() == 16) {
			saw_sixteen = true;
		}
	}
	assert(saw_sixteen);

	std::cout << "  ✓ a class constant may be an expression over constants" << std::endl;
}

static void test_a_constant_default_argument_reaches_the_host() {
	const IRProgram ir = compile_to_ir(
		"const MASK = 1 << 3\n"
		"func f(a = MASK, b = MASK + 1):\n"
		"\treturn a\n");

	const FunctionSignature& sig = find_signature(ir, "f");
	assert(sig.required_arguments == 0);
	assert(sig.parameters[0].default_kind == FunctionParameter::DefaultKind::INT);
	assert(std::get<int64_t>(sig.parameters[0].default_value) == 8);
	assert(std::get<int64_t>(sig.parameters[1].default_value) == 9);

	std::cout << "  ✓ a computed constant default is handed to the host" << std::endl;
}

static void test_computed_constants_index_a_jump_table() {
	const IRProgram ir = compile_to_ir(
		"const A = 1 << 0\n"
		"const B = 1 << 1\n"
		"const C = 1 << 1 | 1\n"
		"const D = 1 << 2\n"
		"func pick(v: int) -> int:\n"
		"\tmatch v:\n"
		"\t\tA: return 10\n"
		"\t\tB: return 20\n"
		"\t\tC: return 30\n"
		"\t\tD: return 40\n"
		"\t\t_: return 0\n");

	assert(has_opcode(find_function(ir, "pick"), IROpcode::SWITCH));

	std::cout << "  ✓ computed constants are dense enough for a jump table" << std::endl;
}

static void test_a_local_shadows_a_constant_in_a_pattern() {
	const IRProgram ir = compile_to_ir(
		"const A = 1\n"
		"const B = 2\n"
		"const C = 3\n"
		"const D = 4\n"
		"func pick(v: int) -> int:\n"
		"\tvar A = 9\n"
		"\tmatch v:\n"
		"\t\tA: return 10\n"
		"\t\tB: return 20\n"
		"\t\tC: return 30\n"
		"\t\tD: return 40\n"
		"\t\t_: return 0\n");

	assert(!has_opcode(find_function(ir, "pick"), IROpcode::SWITCH));

	std::cout << "  ✓ a local shadowing a constant keeps its patterns off the table" << std::endl;
}

static void test_a_run_time_value_is_still_a_run_time_value() {
	const IRProgram ir = compile_to_ir(
		"const ZERO = Vector2.ZERO\n"
		"const BAD = 1 / 0\n"
		"const NAMED = &\"walk\"\n");

	assert(global(ir, "ZERO").init_type == IRGlobalVar::InitType::RUNTIME);
	assert(global(ir, "BAD").init_type == IRGlobalVar::InitType::RUNTIME);
	assert(global(ir, "NAMED").init_type == IRGlobalVar::InitType::RUNTIME);

	std::cout << "  ✓ a value the compiler cannot hold is left to startup" << std::endl;
}

static void test_a_class_constant_still_has_to_fold() {
	const std::string message = compile_error(
		"class Foo:\n"
		"\tconst SIZE = Vector2.ZERO\n"
		"\tfunc f():\n"
		"\t\treturn SIZE\n");

	assert(message.find("not a compile-time value") != std::string::npos);

	std::cout << "  ✓ a class constant that cannot fold is refused" << std::endl;
}

static void test_a_power_folds_the_way_the_host_evaluates_it() {
	const IRProgram ir = compile_to_ir(
		"const SMALL = 2 ** 10\n"
		"const WIDE = 7 ** 20\n"
		"const NEGATIVE = 2 ** -1\n"
		"const HUGE = 2 ** 100000000000\n"
		"const OVER = 2 ** 64\n");

	assert(std::get<int64_t>(global(ir, "SMALL").init_value) == 1024);
	assert(std::get<int64_t>(global(ir, "WIDE").init_value) == 79792266297612000LL);
	assert(std::get<int64_t>(global(ir, "NEGATIVE").init_value) == 0);
	assert(global(ir, "HUGE").init_type == IRGlobalVar::InitType::RUNTIME);
	assert(global(ir, "OVER").init_type == IRGlobalVar::InitType::RUNTIME);

	const std::string message = compile_error("enum E { A = 2 ** 100000000000 }\n");
	assert(message.find("outside the range of an integer") != std::string::npos);

	std::cout << "  ✓ a power folds to the host's value, or not at all" << std::endl;
}

int main() {
	std::cout << "=== Constant Folding Tests ===" << std::endl;

	try {
		test_an_operator_over_constants_is_a_constant();
		test_a_constant_names_another_constant();
		test_engine_constants_fold();
		test_a_ternary_over_constants_folds();
		test_a_script_enum_member_folds_into_a_constant();
		test_a_constant_folds_into_an_enum();
		test_a_class_constant_may_be_an_expression();
		test_a_constant_default_argument_reaches_the_host();
		test_computed_constants_index_a_jump_table();
		test_a_local_shadows_a_constant_in_a_pattern();
		test_a_run_time_value_is_still_a_run_time_value();
		test_a_class_constant_still_has_to_fold();
		test_a_power_folds_the_way_the_host_evaluates_it();

		std::cout << std::endl;
		std::cout << "✅ All constant folding tests passed!" << std::endl;
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}
