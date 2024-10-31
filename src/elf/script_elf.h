#pragma once

#include "../config.h"
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/templates/hash_set.hpp>
GODOT_NAMESPACE

class ELFScriptInstance;
class Sandbox;
namespace godot {
	class ScriptInstanceExtension;
}

class ELFScript : public ScriptExtension {
	GDCLASS(ELFScript, ScriptExtension);

protected:
	static void GODOT_CPP_FUNC (bind_methods)();
	PackedByteArray source_code;
	String global_name;
	String path;
	std::string std_path;
	int source_version = 0;
	int elf_api_version;
	String elf_programming_language;

	mutable HashSet<ELFScriptInstance *> instances;
	friend class ELFScriptInstance;

	static inline HashMap<String, HashSet<Sandbox *>> sandbox_map;

public:
	PackedStringArray functions;
	String get_elf_programming_language() const;
	int get_elf_api_version() const noexcept { return elf_api_version; }
	int get_source_version() const noexcept { return source_version; }
	String get_dockerized_program_path() const;
	const String &get_path() const noexcept { return path; }
	const std::string &get_std_path() const noexcept { return std_path; }

	/// @brief Retrieve a Sandbox instance based on a given owner object.
	/// @param p_for_object The owner object.
	/// @return The Sandbox instance, or nullptr if not found.
	Sandbox *get_sandbox_for(Object *p_for_object) const;

	/// @brief Retrieve all Sandbox instances using this ELF resource.
	/// @return An array of Sandbox instances.
	Array get_sandboxes() const;

	/// @brief Retrieve the content of the ELF resource as a byte array.
	/// @return An ELF program as a byte array.
	const PackedByteArray &get_content();

	void register_instance(Sandbox *p_sandbox) { sandbox_map[path].insert(p_sandbox); }
	void unregister_instance(Sandbox *p_sandbox) { sandbox_map[path].erase(p_sandbox); }

	virtual bool GODOT_CPP_FUNC (editor_can_reload_from_file)() override;
	virtual void GODOT_CPP_FUNC (placeholder_erased)(void *p_placeholder) override;
	virtual bool GODOT_CPP_FUNC (can_instantiate)() const override;
	virtual Ref<Script> GODOT_CPP_FUNC (get_base_script)() const override;
	virtual StringName GODOT_CPP_FUNC (get_global_name)() const override;
	virtual bool GODOT_CPP_FUNC (inherits_script)(const Ref<Script> &p_script) const override;
	virtual StringName GODOT_CPP_FUNC (get_instance_base_type)() const override;
	virtual void *_instance_create(Object *p_for_object) const override;
	virtual void *_placeholder_instance_create(Object *p_for_object) const override;
	virtual bool GODOT_CPP_FUNC (instance_has)(Object *p_object) const override;
	virtual bool GODOT_CPP_FUNC (has_source_code)() const override;
	virtual String GODOT_CPP_FUNC (get_source_code)() const override;
	virtual void GODOT_CPP_FUNC (set_source_code)(const String &p_code) override;
	virtual Error GODOT_CPP_FUNC (reload)(bool p_keep_state) override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (get_documentation)() const override;
	virtual String GODOT_CPP_FUNC (get_class_icon_path)() const override;
	virtual bool GODOT_CPP_FUNC (has_method)(const StringName &p_method) const override;
	virtual bool GODOT_CPP_FUNC (has_static_method)(const StringName &p_method) const override;
	virtual Dictionary GODOT_CPP_FUNC (get_method_info)(const StringName &p_method) const override;
	virtual bool GODOT_CPP_FUNC (is_tool)() const override;
	virtual bool GODOT_CPP_FUNC (is_valid)() const override;
	virtual bool GODOT_CPP_FUNC (is_abstract)() const override;
	virtual ScriptLanguage *_get_language() const override;
	virtual bool GODOT_CPP_FUNC (has_script_signal)(const StringName &p_signal) const override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (get_script_signal_list)() const override;
	virtual bool GODOT_CPP_FUNC (has_property_default_value)(const StringName &p_property) const override;
	virtual Variant GODOT_CPP_FUNC (get_property_default_value)(const StringName &p_property) const override;
	virtual void GODOT_CPP_FUNC (update_exports)() override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (get_script_method_list)() const override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (get_script_property_list)() const override;
	virtual int32_t GODOT_CPP_FUNC (get_member_line)(const StringName &p_member) const override;
	virtual Dictionary GODOT_CPP_FUNC (get_constants)() const override;
	virtual TypedArray<StringName> GODOT_CPP_FUNC (get_members)() const override;
	virtual bool GODOT_CPP_FUNC (is_placeholder_fallback_enabled)() const override;
	virtual Variant GODOT_CPP_FUNC (get_rpc_config)() const override;

	void set_file(const String &path);
	ELFScript() {}
	~ELFScript() {}
};
