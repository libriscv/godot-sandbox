#include "api.hpp"
#include "function_signatures.hpp"

#include <compiler.h>
#include <compiler_exception.h>
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
	// Kept for the get_function_signatures() the caller makes next: the ELF
	// says which functions it exports, but not what they take.
	gdscript_remember_signatures(compiler);

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

// What compile() just produced, one Dictionary per exported function. See
// function_signatures.hpp for the shape and for why the ELF cannot carry it.
PUBLIC Variant get_function_signatures()
{
	return gdscript_signatures_to_variant();
}
