#include "compiler.h"
#include <iostream>
#include <fstream>
#include <string>

using namespace gdscript;

int main(int argc, char** argv) {
	std::cout << "GDScript to RISC-V Compiler Test" << std::endl;
	std::cout << "=================================" << std::endl << std::endl;

	// Test program: simple function that adds two numbers
	std::string test_source = R"(
func add(a, b):
	return a + b

func test():
	return 123

func sum_to_n(n):
	return n

func main():
	var x = 10
	var y = 20
	return x + y
)";

	std::cout << "Source code:" << std::endl;
	std::cout << test_source << std::endl << std::endl;

	// Compile with all debug output
	CompilerOptions options;
	options.dump_tokens = true;
	options.dump_ast = true;
	options.dump_ir = true;
	options.output_elf = true;
	options.output_path = "test_output.elf";

	Compiler compiler;
	auto elf_data = compiler.compile(test_source, options);

	if (elf_data.empty()) {
		std::cerr << "Compilation failed: " << compiler.get_error() << std::endl;
		return 1;
	}

	std::cout << "=== COMPILATION SUCCESS ===" << std::endl;
	std::cout << "Generated ELF size: " << elf_data.size() << " bytes" << std::endl;

	// Write to file
	if (compiler.compile_to_file(test_source, options.output_path, options)) {
		std::cout << "Output written to: " << options.output_path << std::endl;
	} else {
		std::cerr << "Failed to write output: " << compiler.get_error() << std::endl;
		return 1;
	}

	return 0;
}
