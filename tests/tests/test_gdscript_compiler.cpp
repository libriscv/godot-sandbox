#include "api.hpp"
#include "function_signatures.hpp"

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
	// ELF carries symbols but not signatures, line tables, or breakpoint info.
	gdscript_remember_signatures(compiler);
	gdscript_remember_signals(compiler);
	gdscript_remember_line_table(compiler);
	gdscript_remember_breakpoints(compiler);

	if (elf_data.empty()) {
		print("ERROR: Compilation failed: ", compiler.get_error());
		print("ERROR DETAILS: ", String(compiler.get_error()));
		last_error = String(compiler.get_error());
		return PackedByteArray(std::vector<uint8_t>{}); // Return empty array on failure
	}

	return PackedByteArray(elf_data);
}

// The same compile with instrumentation emitted. A separate entry point rather
// than an argument on compile(): the Sandbox ABI hands the guest one Variant
// pointer per argument and no count, so widening a function Godot already calls
// with one argument would have it read a second from a null pointer.
PUBLIC Variant compile_profiled(String code)
{
	CompilerOptions options;
	options.output_elf = true;
	options.profiling = true;
	// Wall clock, which is what a profiler reports. The host installs an
	// rdtime that answers in nanoseconds before the program runs.
	options.profiling_clock = ProfilingClock::TIME;

	Compiler compiler;
	auto elf_data = compiler.compile(code.utf8(), options);
	gdscript_remember_signatures(compiler);
	gdscript_remember_signals(compiler);
	gdscript_remember_line_table(compiler);
	gdscript_remember_breakpoints(compiler);

	if (elf_data.empty()) {
		last_error = String(compiler.get_error());
		print("ERROR: Compilation failed: ", last_error);
		return PackedByteArray(std::vector<uint8_t>{});
	}

	return PackedByteArray(elf_data);
}

// Compile with shadow stack + breakpoints. Separate entry point: ABI is one
// Variant per argument, so widening compile() would read a null pointer.
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

	Compiler compiler;
	auto elf_data = compiler.compile(code.utf8(), options);
	gdscript_remember_signatures(compiler);
	gdscript_remember_signals(compiler);
	gdscript_remember_line_table(compiler);
	gdscript_remember_breakpoints(compiler);

	if (elf_data.empty()) {
		last_error = String(compiler.get_error());
		print("ERROR: Compilation failed: ", last_error);
		return PackedByteArray(std::vector<uint8_t>{});
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

// What compile() just produced, one Dictionary per exported function. See
// function_signatures.hpp for the shape and for why the ELF cannot carry it.
PUBLIC Variant get_function_signatures()
{
	return gdscript_signatures_to_variant();
}

PUBLIC Variant get_signal_signatures()
{
	return gdscript_signals_to_variant();
}

// Address-to-line table. Metadata (no code cost); every compile publishes one.
PUBLIC Variant get_line_table()
{
	return gdscript_line_table_to_variant();
}

// Breakpoint lines the last compile placed; optimizer-removed lines excluded.
PUBLIC Variant get_breakpoint_lines()
{
	return gdscript_breakpoints_to_variant();
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
