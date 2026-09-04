#pragma once

#include "../godot/script_instance.h"
#include "../gdscript/compiler/property_signature.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <vector>

using namespace godot;

class SafeGDScript;

class SafeGDScriptPlaceholderInstance final : public ScriptInstanceExtension {
	Object *owner = nullptr;
	Ref<SafeGDScript> script;
	HashMap<StringName, Variant> values;
	HashMap<StringName, Variant> fallback_values;

public:
	bool set(const StringName &p_name, const Variant &p_value) override;
	bool get(const StringName &p_name, Variant &r_ret) const override;
	const GDExtensionPropertyInfo *get_property_list(uint32_t *r_count) const override;
	void free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count) const override;
	Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid) const override;
	bool validate_property(GDExtensionPropertyInfo &p_property) const override;
	bool property_can_revert(const StringName &p_name) const override;
	bool property_get_revert(const StringName &p_name, Variant &r_ret) const override;
	Object *get_owner() override;
	void get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) override;
	const GDExtensionMethodInfo *get_method_list(uint32_t *r_count) const override;
	void free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count) const override;
	bool has_method(const StringName &p_method) const override;
	GDExtensionInt get_method_argument_count(const StringName &p_method, bool &r_valid) const override;
	void callp(const StringName &p_method, const Variant **p_args, int p_argcount,
			Variant &r_return, GDExtensionCallError &r_error) override;
	void notification(int p_notification, bool p_reversed) override;
	String to_string(bool *r_valid) override;
	void refcount_incremented() override;
	bool refcount_decremented() override;
	Ref<Script> get_script() const override;
	bool is_placeholder() const override;
	void property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) override;
	Variant property_get_fallback(const StringName &p_name, bool *r_valid) override;
	ScriptLanguage *_get_language() override;

	void properties_changed(const std::vector<gdscript::PropertySignature> &p_old,
			const std::vector<gdscript::PropertySignature> &p_new);
	SafeGDScriptPlaceholderInstance(Object *p_owner, const Ref<SafeGDScript> &p_script);
	~SafeGDScriptPlaceholderInstance() override;
};
