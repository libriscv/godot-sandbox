#include "api.hpp"

#include <compiler.h>
#include <compiler_exception.h>
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

// Compile far enough to know whether the program is well-formed, and report
// what went wrong in a form an editor can point at. No ELF is built: the RISC-V
// backend's errors are compiler bugs rather than anything the user wrote, and
// skipping that phase keeps validation cheap enough to run while typing.
//
// The .sgd script language extension calls this on every edit, so the shape of
// the returned Dictionary is part of its contract: "valid" is always present,
// and the rest describes the first error when it is false.
PUBLIC Variant validate(String code)
{
	CompilerOptions options;
	options.output_elf = false;

	Compiler compiler;
	// With output_elf off a successful compile also returns nothing, so the
	// error record is the only thing that says whether it worked.
	compiler.compile(code.utf8(), options);
	const CompilerError &error = compiler.get_error_info();

	Dictionary d = Dictionary::Create();
	d["valid"] = !error.has_error;
	d["message"] = String(error.message);
	d["line"] = (int64_t)error.line;
	d["column"] = (int64_t)error.column;
	d["type"] = String(std::string_view(error_type_to_string(error.type)));
	d["function"] = String(error.function);
	d["hint"] = String(error.hint);
	return d;
}

PUBLIC Variant get_compiler_error()
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
