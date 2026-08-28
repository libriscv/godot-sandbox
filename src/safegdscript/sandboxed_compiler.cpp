#include "compiler_backend.h"

#include "../elf/script_elf.h"
#include "../sandbox.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

namespace {

constexpr uint32_t COMPILER_MEMORY_MAX = 256;
constexpr uint32_t COMPILER_ALLOCATIONS_MAX = 64000;

class SandboxedCompilerBackend final : public GDScriptCompilerBackend {
public:
	const char *name() const override { return "sandboxed"; }
	bool available() override { return sandbox() != nullptr; }

	void set_restricted(bool p_restricted) override {
		Variant restricted = p_restricted;
		const Variant *args[] = { &restricted };
		call_setter("set_restricted", args, 1, "restriction flag");
	}

	void set_source_path(const String &p_path) override {
		Sandbox *compiler = sandbox();
		if (compiler == nullptr || !compiler->has_function("set_source_path")) {
			return;
		}
		Variant path = p_path;
		const Variant *args[] = { &path };
		call_setter("set_source_path", args, 1, "source path");
	}

	void set_autoloads(const PackedStringArray &p_names) override {
		Variant names = p_names;
		const Variant *args[] = { &names };
		call_setter("set_autoloads", args, 1, "autoload list");
	}

	void set_global_classes(const PackedStringArray &p_pairs) override {
		Variant pairs = p_pairs;
		const Variant *args[] = { &pairs };
		call_setter("set_global_classes", args, 1, "global class list");
	}

	void set_base_sources(const PackedStringArray &p_triples) override {
		Sandbox *compiler = sandbox();
		if (compiler == nullptr || !compiler->has_function("set_base_sources")) {
			if (!p_triples.is_empty()) {
				ERR_PRINT("SafeGDScript: the GDScript compiler ELF is too old to compile a base "
						  "script into a program; inherited members will be missing.");
			}
			return;
		}
		Variant triples = p_triples;
		const Variant *args[] = { &triples };
		call_setter("set_base_sources", args, 1, "base sources");
	}

	bool can_build_profiled() override { return has("compile_profiled"); }
	bool can_build_debug() override { return has("compile_debug"); }

	PackedByteArray compile(const String &p_source, const BuildOptions &p_options) override {
		m_call_error = String();
		m_metadata_valid = true;
		const char *entry_point = p_options.profiling
				? "compile_profiled"
				: (p_options.debug ? "compile_debug" : "compile");

		Variant source = p_source;
		Variant breakpoints = p_options.breakpoints;
		const Variant *args[] = { &source, &breakpoints };

		GDExtensionCallError error;
		const Variant result = vmcall(entry_point, args, p_options.debug ? 2 : 1, error);
		if (error.error != GDEXTENSION_CALL_OK) {
			m_call_error = "the GDScript compiler sandbox failed with call error " +
					itos(int(error.error));
			return PackedByteArray();
		}
		if (result.get_type() != Variant::PACKED_BYTE_ARRAY) {
			m_call_error = "the GDScript compiler did not return a PackedByteArray";
			return PackedByteArray();
		}
		return result;
	}

	bool validate(const String &p_source, Validation &r_result) override {
		if (!has("validate")) {
			return false;
		}
		Variant source = p_source;
		const Variant *args[] = { &source };
		GDExtensionCallError error;
		const Variant answer = vmcall("validate", args, 1, error);
		if (error.error != GDEXTENSION_CALL_OK || answer.get_type() != Variant::DICTIONARY) {
			return false;
		}

		const Dictionary reply = answer;
		r_result = Validation();
		r_result.valid = reply.get("valid", true);
		if (!r_result.valid) {
			r_result.line = reply.get("line", 0);
			r_result.column = reply.get("column", 0);
			r_result.message = reply.get("message", String());
			const String hint = reply.get("hint", String());
			if (!hint.is_empty()) {
				r_result.message += String(" (") + hint + String(")");
			}
			if (r_result.message.is_empty()) {
				r_result.message = "Compilation failed";
			}
		}
		return true;
	}

	bool can_analyze() override { return has("analyze"); }
	PackedByteArray analyze(const AnalysisRequest &p_request) override {
		if (!can_analyze()) {
			return PackedByteArray();
		}
		Variant source = p_request.source;
		Variant line = int64_t(p_request.caret_line);
		Variant column = int64_t(p_request.caret_column);
		Variant flags = int64_t(p_request.flags);
		const Variant *args[] = {&source, &line, &column, &flags};
		GDExtensionCallError error;
		const Variant answer = vmcall("analyze", args, 4, error);
		if (error.error != GDEXTENSION_CALL_OK || answer.get_type() != Variant::PACKED_BYTE_ARRAY) {
			return PackedByteArray();
		}
		const PackedByteArray bytes = answer;
		gdscript::SourceModel decoded;
		if (!gdscript::decode_source_model(bytes.ptr(), size_t(bytes.size()), decoded)) {
			m_metadata_valid = false;
			ERR_PRINT("SafeGDScript: the compiler returned a malformed source model.");
			return PackedByteArray();
		}
		return bytes;
	}

	String error_message() override {
		if (!m_call_error.is_empty()) {
			return m_call_error;
		}
		if (!has("get_compiler_error")) {
			return String("compilation failed");
		}
		GDExtensionCallError error;
		const Variant message = vmcall("get_compiler_error", nullptr, 0, error);
		if (error.error != GDEXTENSION_CALL_OK || message.get_type() != Variant::STRING) {
			return String("compilation failed");
		}
		return message;
	}

	std::vector<gdscript::FunctionSignature> function_signatures() override {
		return decode_signatures("get_function_signatures", "function signature table");
	}

	std::vector<gdscript::FunctionSignature> signal_signatures() override {
		return decode_signatures("get_signal_signatures", "signal table");
	}

	std::vector<gdscript::RPCConfig> rpc_configs() override {
		std::vector<gdscript::RPCConfig> configs;
		const PackedByteArray bytes = blob("get_rpc_configs");
		if (bytes.is_empty()) {
			return configs;
		}
		if (!gdscript::decode_rpc_configs(bytes.ptr(), size_t(bytes.size()), configs)) {
			m_metadata_valid = false;
			ERR_PRINT("SafeGDScript: the compiler returned a malformed RPC configuration table.");
		}
		return configs;
	}

	std::vector<gdscript::ClassSignature> class_signatures() override {
		std::vector<gdscript::ClassSignature> classes;
		const PackedByteArray bytes = blob("get_class_signatures");
		if (bytes.is_empty()) {
			return classes;
		}
		if (!gdscript::decode_class_signatures(bytes.ptr(), size_t(bytes.size()), classes)) {
			m_metadata_valid = false;
			ERR_PRINT("SafeGDScript: the compiler returned a malformed class table.");
		}
		return classes;
	}

	std::vector<gdscript::ScriptConstant> script_constants() override {
		std::vector<gdscript::ScriptConstant> constants;
		const PackedByteArray bytes = blob("get_script_constants");
		if (bytes.is_empty()) {
			return constants;
		}
		if (!gdscript::decode_script_constants(bytes.ptr(), size_t(bytes.size()), constants)) {
			m_metadata_valid = false;
			ERR_PRINT("SafeGDScript: the compiler returned a malformed constant table.");
		}
		return constants;
	}

	std::vector<gdscript::PropertySignature> property_signatures() override {
		std::vector<gdscript::PropertySignature> properties;
		const PackedByteArray bytes = blob("get_property_signatures");
		if (bytes.is_empty()) {
			return properties;
		}
		if (!gdscript::decode_property_signatures(bytes.ptr(), size_t(bytes.size()), properties)) {
			m_metadata_valid = false;
			properties.clear();
			ERR_PRINT("SafeGDScript: the compiler returned a malformed property signature table.");
		}
		return properties;
	}

	std::vector<gdscript::DebugVariableRecord> debug_variables() override {
		std::vector<gdscript::DebugVariableRecord> variables;
		const PackedByteArray bytes = blob("get_debug_variables");
		if (bytes.is_empty()) {
			return variables;
		}
		if (!gdscript::decode_debug_variables(bytes.ptr(), size_t(bytes.size()), variables)) {
			m_metadata_valid = false;
			variables.clear();
			ERR_PRINT("SafeGDScript: the compiler returned a malformed debug variable table.");
		}
		return variables;
	}

	gdscript::LineTable line_table() override {
		gdscript::LineTable table;
		const PackedByteArray bytes = blob("get_line_table");
		if (bytes.is_empty()) {
			return table;
		}
		if (!gdscript::decode_line_table(bytes.ptr(), size_t(bytes.size()), table)) {
			m_metadata_valid = false;
			ERR_PRINT("SafeGDScript: the compiler returned a malformed line table.");
		}
		return table;
	}

	bool metadata_valid() const override { return m_metadata_valid; }

	PackedInt32Array installed_breakpoints() override {
		if (!has("get_breakpoint_lines")) {
			return PackedInt32Array();
		}
		GDExtensionCallError error;
		const Variant lines = vmcall("get_breakpoint_lines", nullptr, 0, error);
		if (error.error != GDEXTENSION_CALL_OK || lines.get_type() != Variant::PACKED_INT32_ARRAY) {
			m_metadata_valid = false;
			return PackedInt32Array();
		}
		return lines;
	}

	bool is_tool() override { return flag("is_tool", false); }

	ScriptClass script_class() override {
		ScriptClass declared;
		declared.class_name = text("get_script_class_name");
		declared.base_class = text("get_script_base_class");
		declared.base_is_path = flag("get_script_base_is_path", false);
		declared.native_base_class = text("get_script_native_base_class");
		declared.native_base_is_path = flag("get_script_native_base_is_path", false);
		return declared;
	}

private:
	Sandbox *sandbox() {
		if (m_sandbox != nullptr) {
			return m_sandbox;
		}
		const String compiler_path = "res://addons/godot_sandbox/gdscript.elf";
		if (!FileAccess::file_exists(compiler_path)) {
			ERR_PRINT("SafeGDScript: GDScript compiler ELF not found at " + compiler_path);
			return nullptr;
		}
		Ref<ELFScript> compiler_script = ResourceLoader::get_singleton()->load(compiler_path);
		if (!compiler_script.is_valid()) {
			ERR_PRINT("SafeGDScript: Failed to load GDScript compiler ELF resource.");
			return nullptr;
		}
		Sandbox *sandbox = memnew(Sandbox);
		// Both must be set before set_program().
		sandbox->set_memory_max(COMPILER_MEMORY_MAX);
		sandbox->set_allocations_max(COMPILER_ALLOCATIONS_MAX);
		sandbox->set_program(compiler_script);
		if (!sandbox->has_program_loaded()) {
			ERR_PRINT("SafeGDScript: Failed to initialize GDScript compiler sandbox.");
			memdelete(sandbox);
			return nullptr;
		}
		m_sandbox = sandbox;
		return m_sandbox;
	}

	bool has(const char *p_function) {
		Sandbox *compiler = sandbox();
		return compiler != nullptr && compiler->has_function(p_function);
	}

	Variant vmcall(const char *p_function, const Variant **p_args, int p_argcount,
			GDExtensionCallError &r_error) {
		Sandbox *compiler = sandbox();
		if (compiler == nullptr) {
			r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
			return Variant();
		}
		return compiler->vmcall_fn(p_function, p_args, p_argcount, r_error);
	}

	void call_setter(const char *p_function, const Variant **p_args, int p_argcount,
			const char *p_what) {
		if (!has(p_function)) {
			return;
		}
		GDExtensionCallError error;
		vmcall(p_function, p_args, p_argcount, error);
		if (error.error != GDEXTENSION_CALL_OK) {
			ERR_PRINT(String("SafeGDScript: the compiler refused the ") + p_what + ".");
		}
	}

	PackedByteArray blob(const char *p_function) {
		if (!has(p_function)) {
			return PackedByteArray();
		}
		GDExtensionCallError error;
		const Variant answer = vmcall(p_function, nullptr, 0, error);
		if (error.error != GDEXTENSION_CALL_OK || answer.get_type() != Variant::PACKED_BYTE_ARRAY) {
			m_metadata_valid = false;
			return PackedByteArray();
		}
		return answer;
	}

	std::vector<gdscript::FunctionSignature> decode_signatures(const char *p_function,
			const char *p_what) {
		std::vector<gdscript::FunctionSignature> signatures;
		const PackedByteArray bytes = blob(p_function);
		if (bytes.is_empty()) {
			return signatures;
		}
		if (!gdscript::decode_function_signatures(bytes.ptr(), size_t(bytes.size()), signatures)) {
			m_metadata_valid = false;
			ERR_PRINT(String("SafeGDScript: the compiler returned a malformed ") + p_what + ".");
		}
		return signatures;
	}

	String text(const char *p_function) {
		if (!has(p_function)) {
			return String();
		}
		GDExtensionCallError error;
		const Variant answer = vmcall(p_function, nullptr, 0, error);
		if (error.error != GDEXTENSION_CALL_OK || answer.get_type() != Variant::STRING) {
			return String();
		}
		return answer;
	}

	bool flag(const char *p_function, bool p_absent) {
		if (!has(p_function)) {
			return p_absent;
		}
		GDExtensionCallError error;
		const Variant answer = vmcall(p_function, nullptr, 0, error);
		if (error.error != GDEXTENSION_CALL_OK) {
			return p_absent;
		}
		return bool(answer);
	}

	Sandbox *m_sandbox = nullptr;
	String m_call_error;
	bool m_metadata_valid = true;
};

} // namespace

GDScriptCompilerBackend &sandboxed_compiler_backend() {
	static SandboxedCompilerBackend *backend = new SandboxedCompilerBackend();
	return *backend;
}
