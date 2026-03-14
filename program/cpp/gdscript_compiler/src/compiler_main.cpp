#include "api.hpp"

#include <compiler.h>
#include <string>
using namespace gdscript;

static String last_error = "";

PUBLIC Variant compile(String code)
{
	CompilerOptions options;
	options.dump_tokens = false;
	options.dump_ast = false;
	options.dump_ir = false;
	options.output_elf = true;

	Compiler compiler;
	auto elf_data = compiler.compile(code.utf8(), options);

	if (elf_data.empty()) {
		last_error = String(compiler.get_error());
		print("ERROR: Compilation failed: ", last_error);
		return PackedByteArray(std::vector<uint8_t>{});
	}

	return PackedByteArray(elf_data);
}

PUBLIC Variant compile_to_elf(String code)
{
	return compile(code);
}

PUBLIC Variant get_compiler_error()
{
	return last_error;
}
