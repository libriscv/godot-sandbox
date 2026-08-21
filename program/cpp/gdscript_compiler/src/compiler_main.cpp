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
	// ELF carries names only; signatures are retrieved via get_function_signatures().
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

// Separate entry point: the Sandbox ABI has no argument count, so adding a
// parameter to compile() would dereference a null pointer on old callers.
PUBLIC Variant compile_profiled(String code)
{
	CompilerOptions options;
	options.output_elf = true;
	options.profiling = true;
	// Host installs a nanosecond rdtime before the program runs.
	options.profiling_clock = ProfilingClock::TIME;

	Compiler compiler;
	auto elf_data = compiler.compile(code.utf8(), options);
	gdscript_remember_signatures(compiler);

	if (elf_data.empty()) {
		last_error = String(compiler.get_error());
		print("ERROR: Compilation failed: ", last_error);
		return PackedByteArray(std::vector<uint8_t>{});
	}

	return PackedByteArray(elf_data);
}

// Frontend-only compile (no ELF). Returns a Dictionary with "valid" and error
// location. Called on every edit by the .sgd language extension.
PUBLIC Variant validate(String code)
{
	CompilerOptions options;
	options.output_elf = false;

	Compiler compiler;
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

PUBLIC Variant get_function_signatures()
{
	return gdscript_signatures_to_variant();
}
