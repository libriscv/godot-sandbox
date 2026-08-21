#pragma once

#include "../docker.h"
#include "../gdscript/compiler/function_signature.h"
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/list.hpp>
#include <vector>

using namespace godot;
class Sandbox;
class SafeGDScriptInstance;
class ELFScript;

class SafeGDScript : public ScriptExtension {
	GDCLASS(SafeGDScript, ScriptExtension);

protected:
	static void _bind_methods() {}
	// "script/source" STORAGE property: duplicate() and the scene saver
	// carry STORAGE properties only. Name matches GDScript's .tscn convention.
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;

	String source_code;

public:
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

	// The Sandbox running gdscript.elf, which everything that needs the compiler
	// shares: compiling a script when it is saved, and validating one while it
	// is being typed. Null when the compiler ELF is missing or fails to load.
	static Sandbox *get_compiler_sandbox();

	void set_path(const String &p_path);
	SafeGDScriptInstance *get_safegdscript_script_instance() const;
	// The declared signature of one exported function, or null when the script
	// does not export it.
	const godot::MethodInfo *find_method_info(const StringName &p_method) const;
	const String &get_path() const { return path; }
	// No standalone file: unsaved or scene sub-resource (path contains "::").
	bool is_built_in() const { return path.is_empty() || path.contains("::"); }
	const PackedByteArray &get_content() const { return elf_data; }
	bool compile_source_to_elf(bool p_profiling = false);
	bool is_profiled_build() const { return profiled_build; }
	const std::vector<gdscript::FunctionSignature> &get_signatures() const { return signatures; }
	static String get_compiler_error_message();
	// The parameter lists of the last compile, which the ELF does not carry.
	static std::vector<gdscript::FunctionSignature> get_compiler_function_signatures();
	void remove_instance(SafeGDScriptInstance *p_instance);

	static String PathToGlobalName(const String &p_path) {
		return "SafeGDScript_" + p_path.get_basename().replace("res://", "").replace("/", "_").replace("-", "_").capitalize().replace(" ", "");
	}

	SafeGDScript();
	~SafeGDScript();

private:
	void update_methods_info();

	String path;
	mutable HashSet<SafeGDScriptInstance *> instances;
	PackedByteArray elf_data;
	bool profiled_build = false;
	// IRProgram order; record i describes signatures[i].
	std::vector<gdscript::FunctionSignature> signatures;
	std::vector<godot::MethodInfo> methods_info;
	// Per-function editor metadata: declaration line and '##' description. Godot
	// requests both through separate virtuals rather than the method list, so
	// they are kept beside methods_info, keyed by name.
	struct MethodDocumentation {
		int32_t line = 0;
		String description;
	};
	HashMap<StringName, MethodDocumentation> methods_doc;
	friend class SafeGDScriptInstance;
};
