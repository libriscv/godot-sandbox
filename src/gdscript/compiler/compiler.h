#pragma once
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

class Compiler {
public:
	Compiler();

	// Compile GDScript source to RISC-V ELF
	std::vector<uint8_t> compile(const std::string& source, const CompilerOptions& options = {});

	// Compile to file
	bool compile_to_file(const std::string& source, const std::string& output_path, const CompilerOptions& options = {});

	// Get last error message
	std::string get_error() const { return m_error; }

private:
	std::string m_error;
};

} // namespace gdscript
