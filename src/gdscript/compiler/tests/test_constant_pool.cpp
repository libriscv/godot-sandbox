#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../riscv_codegen.h"
#include "../elf_builder.h"
#include <iostream>
#include <cassert>

using namespace gdscript;

void test_64bit_constant_pool() {
	std::cout << "Testing 64-bit constant pool..." << std::endl;

	std::string source = R"(
func test():
	var x = 1311768467463790320
	return x
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	Parser parser(tokens);
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	RISCVCodeGen riscv;
	auto code = riscv.generate(ir);
	auto const_pool = riscv.get_constant_pool();

	// Verify constant pool contains our large constant
	assert(const_pool.size() == 1);
	assert(const_pool[0] == 1311768467463790320LL);

	// Build ELF and verify it includes the constant pool
	ElfBuilder elf;
	auto elf_data = elf.build(ir);

	// ELF should be larger than just the code (includes headers + constant pool)
	assert(elf_data.size() > code.size());

	std::cout << "  ✓ 64-bit constant pool test passed" << std::endl;
	std::cout << "    - Code size: " << code.size() << " bytes" << std::endl;
	std::cout << "    - Constant pool: " << const_pool.size() << " constants" << std::endl;
	std::cout << "    - ELF size: " << elf_data.size() << " bytes" << std::endl;
}

void test_multiple_constants() {
	std::cout << "Testing multiple large constants..." << std::endl;

	std::string source = R"(
func test():
	var a = 1311768467463790320
	var b = 5876543210123456789
	var c = 1234567890123456789
	return a + b + c
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	Parser parser(tokens);
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	RISCVCodeGen riscv;
	auto code = riscv.generate(ir);
	auto const_pool = riscv.get_constant_pool();

	// Should have 3 constants
	assert(const_pool.size() == 3);
	assert(const_pool[0] == 1311768467463790320LL);
	assert(const_pool[1] == 5876543210123456789LL);
	assert(const_pool[2] == 1234567890123456789LL);

	std::cout << "  ✓ Multiple constants test passed" << std::endl;
	std::cout << "    - Constant pool: " << const_pool.size() << " constants" << std::endl;
}

void test_constant_deduplication() {
	std::cout << "Testing constant deduplication..." << std::endl;

	std::string source = R"(
func test():
	var a = 1311768467463790320
	var b = 1311768467463790320
	var c = 1311768467463790320
	return a + b + c
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	Parser parser(tokens);
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	RISCVCodeGen riscv;
	auto code = riscv.generate(ir);
	auto const_pool = riscv.get_constant_pool();

	// Should only have 1 constant (deduplicated)
	assert(const_pool.size() == 1);
	assert(const_pool[0] == 1311768467463790320LL);

	std::cout << "  ✓ Constant deduplication test passed" << std::endl;
	std::cout << "    - Deduplicated 3 references to 1 constant" << std::endl;
}

void test_small_constants_not_pooled() {
	std::cout << "Testing small constants are not pooled..." << std::endl;

	std::string source = R"(
func test():
	var a = 42
	var b = 1000
	var c = -500
	return a + b + c
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	Parser parser(tokens);
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	RISCVCodeGen riscv;
	auto code = riscv.generate(ir);
	auto const_pool = riscv.get_constant_pool();

	// Small constants should not be in pool
	assert(const_pool.size() == 0);

	std::cout << "  ✓ Small constants not pooled test passed" << std::endl;
}

void test_float_constants() {
	std::cout << "Testing float constants..." << std::endl;

	std::string source = R"(
func test():
	var a = 3.14159
	var b = 2.71828
	return a + b
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	Parser parser(tokens);
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	RISCVCodeGen riscv;
	auto code = riscv.generate(ir);
	auto const_pool = riscv.get_constant_pool();

	// Float constants are stored as int64 bit patterns
	// They should be in the constant pool
	assert(const_pool.size() == 2);

	// Verify the bit patterns match the float values
	union { int64_t i; double d; } conv;
	conv.i = const_pool[0];
	assert(conv.d > 3.14 && conv.d < 3.15); // Approximately 3.14159
	conv.i = const_pool[1];
	assert(conv.d > 2.71 && conv.d < 2.72); // Approximately 2.71828

	std::cout << "  ✓ Float constants test passed" << std::endl;
	std::cout << "    - Float constants stored as int64 bit patterns" << std::endl;
}

int main() {
	std::cout << "=== Constant Pool Tests ===" << std::endl;

	try {
		test_64bit_constant_pool();
		test_multiple_constants();
		test_constant_deduplication();
		test_small_constants_not_pooled();
		test_float_constants();

		std::cout << std::endl;
		std::cout << "✅ All constant pool tests passed!" << std::endl;
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}
