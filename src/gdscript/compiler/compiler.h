#pragma once
#include "compiler_exception.h"
#include "function_signature.h"
#include "property_signature.h"
#include "debug_layout.h"
#include "line_table.h"
#include "profiling_layout.h"
#include "variant_layout.h"
#include <string>
#include <utility>
#include <vector>
#include <cstdint>

namespace gdscript {

struct CompilerOptions {
	enum class StructChecks : uint8_t {
		OFF,
		SHAPE,
		DEEP,
	};
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
	// Emits DebugLayout shadow stack (push/pop per call). Line table either way.
	bool debug_info = false;
	// Emits cooperative line polls for the editor step controls. This is
	// separate from debug_info so metadata-only builds do not gain syscalls.
	bool debug_step_points = false;
	// 1-based lines to break on. Non-empty implies debug_info.
	std::vector<uint32_t> breakpoint_lines;
	bool restricted = false;
	// Exact-key checks at host/untyped boundaries. Restricted compilation always
	// enables at least SHAPE. DEEP additionally validates typed field values.
	StructChecks struct_checks = StructChecks::SHAPE;
	// Resource path of the source being compiled. Used to resolve relative
	// constant load()/preload() paths the same way GDScript does.
	std::string source_path;
	std::vector<std::string> autoloads;
	std::vector<std::pair<std::string, std::string>> global_script_classes;

	struct BaseSource {
		std::string name;
		std::string path;
		std::string source;
	};
	std::vector<BaseSource> base_sources; // nearest base first
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
	const std::vector<FunctionSignature> &get_signal_signatures() const { return m_signals; }
	const std::vector<RPCConfig> &get_rpc_configs() const { return m_rpc_configs; }
	const std::vector<ClassSignature> &get_class_signatures() const { return m_class_signatures; }
	const std::vector<ScriptConstant> &get_script_constants() const { return m_constants; }
	const std::vector<PropertySignature> &get_property_signatures() const { return m_properties; }
	const std::vector<DebugVariableRecord> &get_debug_variables() const { return m_debug_variables; }
	const LineTable &get_line_table() const { return m_line_table; }
	bool is_tool() const { return m_is_tool; }
	const std::string &get_class_name() const { return m_class_name; }
	const std::string &get_base_class() const { return m_base_class; }
	bool base_is_path() const { return m_base_is_path; }
	const std::string &get_native_base_class() const { return m_native_base_class; }
	bool native_base_is_path() const { return m_native_base_is_path; }
	// Subset of breakpoint_lines that got a stop emitted.
	const std::vector<uint32_t> &get_installed_breakpoints() const { return m_installed_breakpoints; }

private:
	std::string m_error;
	CompilerError m_error_info;
	std::vector<FunctionSignature> m_signatures;
	std::vector<FunctionSignature> m_signals;
	std::vector<RPCConfig> m_rpc_configs;
	std::vector<ClassSignature> m_class_signatures;
	std::vector<ScriptConstant> m_constants;
	std::vector<PropertySignature> m_properties;
	std::vector<DebugVariableRecord> m_debug_variables;
	LineTable m_line_table;
	bool m_is_tool = false;
	std::string m_class_name;
	std::string m_base_class;
	bool m_base_is_path = false;
	std::string m_native_base_class;
	bool m_native_base_is_path = false;
	std::vector<uint32_t> m_installed_breakpoints;
};

} // namespace gdscript
