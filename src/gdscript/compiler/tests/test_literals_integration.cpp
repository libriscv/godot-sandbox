#include "../codegen.h"
#include "../compiler.h"
#include "../ir_interpreter.h"
#include "../lexer.h"
#include "../parser.h"
#include <cassert>
#include <iostream>
#include <variant>
#include <vector>

using namespace gdscript;

// Helper to compile and execute using IRInterpreter
// Note: IRInterpreter needs to support arrays/dicts, but for now we'll add basic support
IRInterpreter::Value execute(const std::string &source, const std::string &function = "main",
		const std::vector<IRInterpreter::Value> &args = {}) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	IRInterpreter interp(ir);
	return interp.call(function, args);
}

// For now, we'll use a simpler approach: compile to ELF and verify it's valid
// Full execution testing requires sandbox integration which is more complex
void test_array_literal_compiles() {
	std::cout << "Testing array literal compilation..." << std::endl;

	std::string source = R"(
func return_array():
	return [1, 2, 3]
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());
	assert(elf_data.size() > 100); // ELF should be substantial

	std::cout << "  ✓ Array literal compiles to ELF (size: " << elf_data.size() << " bytes)" << std::endl;
}

void test_empty_array_compiles() {
	std::cout << "Testing empty array compilation..." << std::endl;

	std::string source = R"(
func return_empty():
	return []
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Empty array compiles to ELF" << std::endl;
}

void test_dictionary_literal_compiles() {
	std::cout << "Testing dictionary literal compilation..." << std::endl;

	std::string source = R"(
func return_dict():
	return {"key1": "value1", "key2": 42}
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Dictionary literal compiles to ELF" << std::endl;
}

void test_empty_dictionary_compiles() {
	std::cout << "Testing empty dictionary compilation..." << std::endl;

	std::string source = R"(
func return_empty():
	return {}
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Empty dictionary compiles to ELF" << std::endl;
}

void test_nested_array_compiles() {
	std::cout << "Testing nested array compilation..." << std::endl;

	std::string source = R"(
func return_nested():
	return [[1, 2], [3, 4]]
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Nested array compiles to ELF" << std::endl;
}

void test_array_with_dict_compiles() {
	std::cout << "Testing array with dictionary compilation..." << std::endl;

	std::string source = R"(
func return_mixed():
	return [1, 2, {"a": 1, "b": 2}]
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Array with dictionary compiles to ELF" << std::endl;
}

void test_dict_with_array_compiles() {
	std::cout << "Testing dictionary with array compilation..." << std::endl;

	std::string source = R"(
func return_mixed():
	return {"arr": [1, 2, 3], "num": 42}
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Dictionary with array compiles to ELF" << std::endl;
}

void test_array_with_expressions_compiles() {
	std::cout << "Testing array with expressions compilation..." << std::endl;

	std::string source = R"(
func return_computed():
	return [1 + 2, 3 * 4, 10 - 5]
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Array with expressions compiles to ELF" << std::endl;
}

void test_dict_with_expressions_compiles() {
	std::cout << "Testing dictionary with expressions compilation..." << std::endl;

	std::string source = R"(
func return_computed():
	return {1 + 1: 2 * 2, "key": 10 - 5}
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Dictionary with expressions compiles to ELF" << std::endl;
}

void test_large_array_compiles() {
	std::cout << "Testing large array compilation..." << std::endl;

	std::string source = R"(
func return_large():
	return [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Large array compiles to ELF" << std::endl;
}

void test_deeply_nested_dict_compiles() {
	std::cout << "Testing deeply nested dictionary compilation..." << std::endl;

	std::string source = R"(
func return_nested():
	return {'a': {'b': {'c': {'d': 1}}}}
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Deeply nested dictionary compiles to ELF" << std::endl;
}

int main() {
	std::cout << "\n=== Running Array and Dictionary Literal Integration Tests ===" << std::endl;
	std::cout << "These tests verify that array and dictionary literals compile to valid ELF\n"
			  << std::endl;

	try {
		test_array_literal_compiles();
		test_empty_array_compiles();
		test_dictionary_literal_compiles();
		test_empty_dictionary_compiles();
		test_nested_array_compiles();
		test_array_with_dict_compiles();
		test_dict_with_array_compiles();
		test_array_with_expressions_compiles();
		test_dict_with_expressions_compiles();
		test_large_array_compiles();
		test_deeply_nested_dict_compiles();

		std::cout << "\n✅ All array and dictionary literal integration tests passed!" << std::endl;
		std::cout << "\nNote: Full runtime verification requires sandbox execution." << std::endl;
		std::cout << "      See tests/tests/test_gdscript_compiler.gd for runtime tests.\n"
				  << std::endl;
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}
