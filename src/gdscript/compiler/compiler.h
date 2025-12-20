#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace gdscript {

struct CompilerOptions {
	bool dump_tokens = false;
	bool dump_ast = false;
	bool dump_ir = false;
	bool output_elf = true;
	std::string output_path;
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
