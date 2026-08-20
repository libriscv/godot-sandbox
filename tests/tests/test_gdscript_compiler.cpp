#include "api.hpp"

#include <compiler.h>
#include <variant_layout.h>
#include <string>
using namespace gdscript;

static String last_error = "";

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
		last_error = String(compiler.get_error());
		return PackedByteArray(std::vector<uint8_t>{}); // Return empty array on failure
	}

	return PackedByteArray(elf_data);
}

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
		print("ERROR: Compilation failed: ", compiler.get_error());
		print("ERROR DETAILS: ", String(compiler.get_error()));
		last_error = String(compiler.get_error());
		return PackedByteArray(std::vector<uint8_t>{}); // Return empty array on failure
	}

	return PackedByteArray(elf_data);
}

PUBLIC Variant get_compiler_error(Compiler* compiler)
{
	return last_error;
}

// The generated code hard-codes the Variant layout, so the compiler's idea of it
// has to match the sandbox API it is running inside. They are derived from the
// same DOUBLE_PRECISION_REAL_T flag, but only a check ties them together: a
// mismatch would produce ELFs that read every vector component from the wrong
// offset, and single-precision hosts would never notice.
PUBLIC Variant compiler_variant_layout()
{
	const CompilerOptions options;
	const VariantLayout layout(options.double_precision);

	Dictionary d = Dictionary::Create();
	d["compiler_variant_size"] = (int64_t)layout.variant_size();
	d["compiler_real_size"] = (int64_t)layout.real_size();
	d["guest_variant_size"] = (int64_t)sizeof(Variant);
	d["guest_real_size"] = (int64_t)sizeof(real_t);
	d["double_precision"] = layout.double_precision;
	return d;
}
