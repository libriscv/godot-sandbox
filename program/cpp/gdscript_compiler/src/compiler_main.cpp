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

	gdscript_apply_restrictions(options);

	Compiler compiler;
	auto elf_data = compiler.compile(code.utf8(), options);
	gdscript_remember_signatures(compiler);
	gdscript_remember_signals(compiler);
	gdscript_remember_line_table(compiler);
	gdscript_remember_breakpoints(compiler);
	gdscript_remember_is_tool(compiler);
	gdscript_remember_script_class(compiler);

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

	gdscript_apply_restrictions(options);

	Compiler compiler;
	auto elf_data = compiler.compile(code.utf8(), options);
	gdscript_remember_signatures(compiler);
	gdscript_remember_signals(compiler);
	gdscript_remember_line_table(compiler);
	gdscript_remember_breakpoints(compiler);
	gdscript_remember_is_tool(compiler);
	gdscript_remember_script_class(compiler);

	if (elf_data.empty()) {
		last_error = String(compiler.get_error());
		print("ERROR: Compilation failed: ", last_error);
		return PackedByteArray(std::vector<uint8_t>{});
	}

	return PackedByteArray(elf_data);
}

// Separate entry point: ABI has no argument count. Empty list = debug build
// with no breakpoints.
PUBLIC Variant compile_debug(String code, PackedInt32Array breakpoints)
{
	CompilerOptions options;
	options.output_elf = true;
	options.debug_info = true;
	for (int32_t line : breakpoints.fetch()) {
		if (line > 0) {
			options.breakpoint_lines.push_back(uint32_t(line));
		}
	}

	gdscript_apply_restrictions(options);

	Compiler compiler;
	auto elf_data = compiler.compile(code.utf8(), options);
	gdscript_remember_signatures(compiler);
	gdscript_remember_signals(compiler);
	gdscript_remember_line_table(compiler);
	gdscript_remember_breakpoints(compiler);
	gdscript_remember_is_tool(compiler);
	gdscript_remember_script_class(compiler);

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
	gdscript_apply_restrictions(options);

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

PUBLIC Variant get_signal_signatures()
{
	return gdscript_signals_to_variant();
}

PUBLIC Variant get_line_table()
{
	return gdscript_line_table_to_variant();
}

// Subset of requested lines where a break was actually emitted.
PUBLIC Variant get_breakpoint_lines()
{
	return gdscript_breakpoints_to_variant();
}

PUBLIC Variant is_tool()
{
	return gdscript_last_is_tool();
}

PUBLIC Variant set_restricted(bool restricted)
{
	gdscript_restricted() = restricted;
	return Nil;
}

PUBLIC Variant get_script_class_name()
{
	return String(gdscript_last_class_name());
}

PUBLIC Variant get_script_base_class()
{
	return String(gdscript_last_base_class());
}

PUBLIC Variant get_script_base_is_path()
{
	return gdscript_last_base_is_path();
}
