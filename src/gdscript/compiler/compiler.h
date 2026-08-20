#pragma once
#include "compiler_exception.h"
#include "variant_layout.h"
#include <string>
#include <vector>
#include <cstdint>

namespace gdscript {

struct CompilerOptions {
	bool dump_tokens = false;
	bool dump_ast = false;
	bool dump_ir = false;
	bool output_elf = true;
	// Run the IR optimizer. Turning it off is how the optimization-invariance
	// test gets a reference answer: an optimizer pass must not change what a
	// program computes.
	bool optimize = true;
	std::string output_path;
	// Emit code for a Godot build with real_t = double (REAL_T_IS_DOUBLE).
	// Defaults to whatever this compiler was built for, which is the right
	// answer both inside the sandbox and on the host.
	bool double_precision = native_variant_layout().double_precision;
};

// What the last failed compile knew about the error, kept apart from the
// formatted message so that a caller can point at the offending line. The
// editor uses it to underline the error in a .sgd file, which needs the line
// and column as numbers rather than as prose inside get_error().
struct CompilerError {
	bool has_error = false;
	ErrorType type = ErrorType::UNKNOWN_ERROR;
	std::string message;   // Primary message, without the "[TYPE]" prefix.
	int line = 0;          // 1-based, 0 when the error carries no location.
	int column = 0;        // 1-based, 0 when unknown.
	std::string function;  // Enclosing function, empty at global scope.
	std::string hint;      // How to fix it, when the thrower knew.
};

class Compiler {
public:
	Compiler();

	// Compile GDScript source to RISC-V ELF
	std::vector<uint8_t> compile(const std::string& source, const CompilerOptions& options = {});

	// Compile to file
	bool compile_to_file(const std::string& source, const std::string& output_path, const CompilerOptions& options = {});

	// Get last error message
	std::string get_error() const { return m_error; }

	// The same error with its location intact. Only meaningful after a compile
	// that returned no ELF; has_error is false otherwise.
	const CompilerError &get_error_info() const { return m_error_info; }

private:
	std::string m_error;
	CompilerError m_error_info;
};

} // namespace gdscript
