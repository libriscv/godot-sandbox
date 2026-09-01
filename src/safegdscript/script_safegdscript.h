#pragma once

#include "../docker.h"
#include "../gdscript/compiler/function_signature.h"
#include "../gdscript/compiler/line_table.h"
#include "../gdscript/compiler/debug_layout.h"
#include "../gdscript/compiler/property_signature.h"
#include "../gdscript/compiler/source_model.h"
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/list.hpp>
#include <vector>

using namespace godot;
class Sandbox;
class GDScriptCompilerBackend;
class SafeGDScriptInstance;
class SafeGDScriptClass;
class SafeGDScriptPlaceholderInstance;
class ELFScript;

class SafeGDScript : public ScriptExtension {
	GDCLASS(SafeGDScript, ScriptExtension);

protected:
	// Breakpoints and break state. No GDExtension breakpoint callback from the
	// editor; a .sgd debugger drives this from script.
	static void _bind_methods();
	// "script/source" STORAGE property: duplicate() and the scene saver
	// carry STORAGE properties only. Name matches GDScript's .tscn convention.
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;

	String source_code;

public:
	enum class ReloadPolicy {
		DISCARD_STATE,
		KEEP_STATE,
	};
	virtual bool _editor_can_reload_from_file() override;
	virtual void _placeholder_erased(void *p_placeholder) override;
	virtual bool _can_instantiate() const override;
	virtual Ref<Script> _get_base_script() const override;
	virtual StringName _get_global_name() const override;
	virtual bool _inherits_script(const Ref<Script> &p_script) const override;
	virtual StringName _get_instance_base_type() const override;
	virtual void *_instance_create(Object *p_for_object) const override;
	virtual void *_placeholder_instance_create(Object *p_for_object) const override;
	virtual bool _instance_has(Object *p_object) const override;
	virtual bool _has_source_code() const override;
	virtual String _get_source_code() const override;
	virtual void _set_source_code(const String &p_code) override;
	virtual Error _reload(bool p_keep_state) override;
	virtual StringName _get_doc_class_name() const override;
	virtual TypedArray<Dictionary> _get_documentation() const override;
	virtual String _get_class_icon_path() const override;
	virtual bool _has_method(const StringName &p_method) const override;
	virtual bool _has_static_method(const StringName &p_method) const override;
	virtual Variant _get_script_method_argument_count(const StringName &p_method) const override;
	virtual Dictionary _get_method_info(const StringName &p_method) const override;
	virtual bool _is_tool() const override;
	virtual bool _is_valid() const override;
	virtual bool _is_abstract() const override;
	virtual ScriptLanguage *_get_language() const override;
	virtual bool _has_script_signal(const StringName &p_signal) const override;
	virtual TypedArray<Dictionary> _get_script_signal_list() const override;
	virtual bool _has_property_default_value(const StringName &p_property) const override;
	virtual Variant _get_property_default_value(const StringName &p_property) const override;
	virtual void _update_exports() override;
	virtual TypedArray<Dictionary> _get_script_method_list() const override;
	virtual TypedArray<Dictionary> _get_script_property_list() const override;
	virtual int32_t _get_member_line(const StringName &p_member) const override;
	virtual Dictionary _get_constants() const override;
	virtual TypedArray<StringName> _get_members() const override;
	virtual bool _is_placeholder_fallback_enabled() const override;
	virtual Variant _get_rpc_config() const override;

	void set_path(const String &p_path);
	SafeGDScriptInstance *get_safegdscript_script_instance() const;
	Object *owner_for_instance_base(uint64_t p_instance_base) const;
	// The declared signature of one exported function, or null when the script
	// does not export it.
	const godot::MethodInfo *find_method_info(const StringName &p_method) const;
	const String &get_path() const { return path; }
	// No standalone file: unsaved or scene sub-resource (path contains "::").
	bool is_built_in() const { return path.is_empty() || path.contains("::"); }
	const PackedByteArray &get_content() const { return elf_data; }
	bool compile_source_to_elf(bool p_profiling = false, bool p_debug = false,
			ReloadPolicy p_reload_policy = ReloadPolicy::DISCARD_STATE,
			bool p_shipping = false);
	String bake_translation();
	bool is_translation_baked() const;
	int64_t get_translation_hash() const;
	const std::vector<gdscript::PropertySignature> &get_property_signatures() const { return properties; }
	const std::vector<gdscript::DebugVariableRecord> &get_debug_variables() const { return debug_variables; }
	const gdscript::PropertySignature *find_property_signature(const StringName &p_name) const;
	PropertyInfo property_info(const gdscript::PropertySignature &p_signature) const;
	bool property_default(const StringName &p_name, Variant &r_value) const;
	bool is_profiled_build() const { return profiled_build; }

	// -= Breakpoints =-
	// Compile-time: setting/clearing recompiles. Returns false on failed
	// recompile; the requested set is kept either way.
	bool set_breakpoint(int32_t p_line, bool p_enabled);
	bool set_breakpoints(const PackedInt32Array &p_lines);
	bool clear_breakpoints();
	// Editor-owned set for the running program: rebuilds when the build cannot
	// honour it, then records what the line table says it could place.
	void update_runtime_breakpoints(const PackedInt32Array &p_lines);
	// Project-owned and currently editor-owned lines, ascending. Editor polling
	// never writes its lines into the persistent project-owned set.
	PackedInt32Array get_breakpoints() const;
	// Ours alone, without asking the editor for its own.
	const PackedInt32Array &get_project_breakpoints() const { return breakpoints; }
	// Subset the last compile could place; dead lines have no instructions.
	PackedInt32Array get_active_breakpoints() const;

	// -= Break state =-
	// Global: a break blocks the guest's thread; only one program stops at a time.
	static bool is_stopped();
	static int64_t get_stopped_line();
	static PackedStringArray get_stopped_backtrace();
	// Safe to call from the breakpoint_hit handler (same thread).
	static void debug_continue();

	// Shadow stack emitted; faults get a call stack. Compile-time, not a switch.
	bool is_debug_build() const { return debug_build; }
	const std::vector<gdscript::FunctionSignature> &get_signatures() const { return signatures; }
	// Addresses are the loaded ELF's. Present in every build: it costs no code.
	const gdscript::LineTable &get_line_table() const { return line_table; }
	String get_compile_error() const;
	static PackedStringArray resolve_base_sources(const String &p_source,
			const String &p_self_path = String(), String *r_error = nullptr);
	static void poll_base_sources();
	static std::vector<SafeGDScript *> live_script_snapshot();
	// The nested class of that name, or null. One per class whose declared chain
	// reaches an engine class; the rest stay plain Dictionaries in the guest.
	Ref<SafeGDScriptClass> find_nested_class(const StringName &p_name) const;
	const String &get_script_class_name() const { return class_name; }
	const String &get_script_base_class() const { return base_class; }
	const String &get_script_native_base_class() const { return native_base_class; }
	bool uses_trait(const StringName &p_name) const;
	void class_restrictions_changed();
	void remove_instance(SafeGDScriptInstance *p_instance);
	void remove_placeholder(SafeGDScriptPlaceholderInstance *p_instance);
	Variant new_instance(const Variant **p_args, GDExtensionInt p_argcount, GDExtensionCallError &r_error);
	const Variant **pending_init_args = nullptr;
	int pending_init_argcount = 0;

	static void scan_class_header(const String &p_source, String *r_class_name, String *r_base,
			bool *r_is_trait = nullptr);

	static String PathToGlobalName(const String &p_path) {
		return "SafeGDScript_" + p_path.get_basename().replace("res://", "").replace("/", "_").replace("-", "_").capitalize().replace(" ", "");
	}

	SafeGDScript();
	~SafeGDScript();

private:
	void update_methods_info(GDScriptCompilerBackend &p_compiler);
	void update_constants(GDScriptCompilerBackend &p_compiler);
	void rebuild_nested_classes(GDScriptCompilerBackend &p_compiler);
	void rebuild_if_a_base_changed();
	// Rejects rebuild while this script is stopped at a breakpoint.
	bool refuse_while_stopped() const;
	bool fail_compile(const String &p_message);

	String path;
	mutable HashSet<SafeGDScriptInstance *> instances;
	mutable HashSet<SafeGDScriptPlaceholderInstance *> placeholders;
	PackedByteArray elf_data;
	// Per-script copy; the compiler's own message belongs to the last caller.
	String last_error;
	bool profiled_build = false;
	bool debug_build = false;
	bool source_compile_pending_reload = false;
	bool source_compile_succeeded = false;
	String class_name;
	String base_class;
	bool base_is_path = false;
	String native_base_class;
	bool native_base_is_path = false;
	// Loaded on demand: compile-time load would recurse through the resource loader.
	mutable Ref<Script> base_script;
	mutable bool base_script_resolved = false;
	PackedStringArray base_paths;
	Vector<uint64_t> base_stamps;
	bool compiled_restricted = false;
	bool class_access_restricted() const;
	// Ascending, no repeats. Non-empty enables shadow stack + stops.
	PackedInt32Array breakpoints;
	// What the last compile placed, a subset of the above.
	PackedInt32Array active_breakpoints;
	// Until source metadata says otherwise, match GDScript's safe default: a
	// script is not a tool script.  Treating a newly loaded resource as @tool
	// lets the editor create a live instance before compilation finishes.
	bool tool_script = false;
	bool abstract_script = false;
	String class_icon_path = "res://addons/godot_sandbox/SafeGDScript.svg";
	// IRProgram order; index i matches shadow-stack frame function_index.
	std::vector<gdscript::FunctionSignature> signatures;
	gdscript::LineTable line_table;
	std::vector<godot::MethodInfo> methods_info;
	std::vector<gdscript::PropertySignature> properties;
	std::vector<gdscript::DebugVariableRecord> debug_variables;
	gdscript::SourceModel source_model;
	std::vector<gdscript::PropertySignature> previous_properties_for_update;
	HashMap<StringName, Variant> property_defaults;
	Dictionary rpc_config;
	HashMap<StringName, Ref<SafeGDScriptClass>> nested_classes;
	std::vector<gdscript::ClassSignature> trait_signatures;
	HashSet<StringName> used_traits;
	std::vector<godot::MethodInfo> signals_info;
	// Declaration line and '##' description, keyed by member name.
	struct MethodDocumentation {
		int32_t line = 0;
		String description;
	};
	HashMap<StringName, MethodDocumentation> methods_doc;
	// File-scope `const` and `enum`, folded at compile time. The guest keeps no
	// storage for them, so a reader outside the script -- `Autoload.SOME_ENUM` --
	// is answered from here, the way GDScript answers out of Script::constants.
	HashMap<StringName, Variant> constants;
	friend class SafeGDScriptInstance;
	friend class SafeGDScriptPlaceholderInstance;
};
