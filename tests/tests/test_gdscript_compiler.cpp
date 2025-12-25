#include "api.hpp"

#include <compiler.h>
#include <string>
using namespace gdscript;

PUBLIC Variant compile_to_elf(String code)
{
	print("Compiling GDScript code to RISC-V ELF:", code);
	// Compile with all debug output
	CompilerOptions options;
	options.dump_tokens = false;
	options.dump_ast = false;
	options.dump_ir = false;
	options.output_elf = true;

	Compiler compiler;
	auto elf_data = compiler.compile(code.utf8(), options);

	if (elf_data.empty()) {
		print("ERROR: Compilation failed: ", compiler.get_error());
		print("ERROR DETAILS: ", String(compiler.get_error()));
		return PackedByteArray(std::vector<uint8_t>{}); // Return empty array on failure
	}

	return PackedByteArray(elf_data);
}
