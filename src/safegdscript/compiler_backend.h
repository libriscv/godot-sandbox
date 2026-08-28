#pragma once

#include "../gdscript/compiler/function_signature.h"
#include "../gdscript/compiler/debug_layout.h"
#include "../gdscript/compiler/line_table.h"
#include "../gdscript/compiler/property_signature.h"
#include "../gdscript/compiler/source_model.h"
#include <godot_cpp/variant/variant.hpp>
#include <vector>

using namespace godot;

class GDScriptCompilerBackend {
public:
	virtual ~GDScriptCompilerBackend() = default;

	virtual const char *name() const = 0;
	virtual bool available() = 0;

	virtual void set_restricted(bool p_restricted) = 0;
	virtual void set_source_path(const String &p_path) = 0;
	virtual void set_autoloads(const PackedStringArray &p_names) = 0;
	virtual void set_global_classes(const PackedStringArray &p_pairs) = 0;
	virtual void set_base_sources(const PackedStringArray &p_triples) = 0;

	struct BuildOptions {
		bool profiling = false;
		bool debug = false;
		PackedInt32Array breakpoints;
	};
	virtual PackedByteArray compile(const String &p_source, const BuildOptions &p_options) = 0;

	struct Validation {
		bool valid = true;
		int line = 0;
		int column = 0;
		String message;
	};
	virtual bool validate(const String &p_source, Validation &r_result) = 0;

	struct AnalysisRequest {
		String source;
		String path;
		int32_t caret_line = 0;
		int32_t caret_column = 0;
		uint32_t flags = gdscript::ANALYZE_ALL;
	};
	virtual bool can_analyze() = 0;
	virtual PackedByteArray analyze(const AnalysisRequest &p_request) = 0;

	virtual String error_message() = 0;
	virtual std::vector<gdscript::FunctionSignature> function_signatures() = 0;
	virtual std::vector<gdscript::FunctionSignature> signal_signatures() = 0;
	// Top-level @rpc methods; empty from a compiler that predates RPC metadata.
	virtual std::vector<gdscript::RPCConfig> rpc_configs() = 0;
	// Nested classes with an engine base; empty from a compiler that predates them.
	virtual std::vector<gdscript::ClassSignature> class_signatures() = 0;
	// File-scope `const` and `enum`; empty from a compiler that predates them.
	virtual std::vector<gdscript::ScriptConstant> script_constants() = 0;
	// Variables and folded defaults. Empty from a compiler ELF predating P0.
	virtual std::vector<gdscript::PropertySignature> property_signatures() = 0;
	virtual std::vector<gdscript::DebugVariableRecord> debug_variables() = 0;
	virtual gdscript::LineTable line_table() = 0;
	virtual bool metadata_valid() const = 0;
	virtual PackedInt32Array installed_breakpoints() = 0;
	virtual bool is_tool() = 0;

	struct ScriptClass {
		String class_name;
		String base_class;
		bool base_is_path = false;
		String native_base_class;
		bool native_base_is_path = false;
	};
	virtual ScriptClass script_class() = 0;

	virtual bool can_build_profiled() = 0;
	virtual bool can_build_debug() = 0;
};

namespace gdscript_compiler {

enum class Policy {
	ALWAYS_SANDBOXED,
	SANDBOX_RESTRICTED,
	ALWAYS_DIRECT,
};

Policy policy();
const char *policy_name();

GDScriptCompilerBackend &backend_for(bool p_restricted);

void prepare(GDScriptCompilerBackend &p_backend, bool p_restricted,
		const PackedStringArray &p_base_sources, const String &p_source_path = String());

} // namespace gdscript_compiler

GDScriptCompilerBackend *direct_compiler_backend();
GDScriptCompilerBackend &sandboxed_compiler_backend();
