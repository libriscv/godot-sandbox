#pragma once
#include "compiler_exception.h"
#include "function_signature.h"
#include "profiling_layout.h"
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
	// Off for optimization-invariance reference answers.
	bool optimize = true;
	std::string output_path;
	bool double_precision = native_variant_layout().double_precision;
	// Compile-time switch; off emits no instrumentation at all.
	bool profiling = false;
	ProfilingClock profiling_clock = ProfilingClock::TIME;
};

// Structured error for editor underlines; the formatted string is in get_error().
struct CompilerError {
	bool has_error = false;
	ErrorType type = ErrorType::UNKNOWN_ERROR;
	std::string message;
	int line = 0;
	int column = 0;
	std::string function;
	std::string hint;
};

class Compiler {
public:
	Compiler();

	std::vector<uint8_t> compile(const std::string& source, const CompilerOptions& options = {});
	bool compile_to_file(const std::string& source, const std::string& output_path, const CompilerOptions& options = {});
	std::string get_error() const { return m_error; }
	const CompilerError &get_error_info() const { return m_error_info; }
	// Populated by every compile that reaches codegen, including output_elf=false.
	const std::vector<FunctionSignature> &get_function_signatures() const { return m_signatures; }

private:
	std::string m_error;
	CompilerError m_error_info;
	std::vector<FunctionSignature> m_signatures;
};

} // namespace gdscript
