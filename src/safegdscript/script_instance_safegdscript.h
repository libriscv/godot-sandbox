#pragma once

#include <gdextension_interface.h>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/mutex.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/core/type_info.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "../godot/script_instance.h"
using namespace godot;

class Sandbox;
class SafeGDScript;

class SafeGDScriptInstance : public ScriptInstanceExtension {
	Object *owner;
	Ref<SafeGDScript> script;
	Sandbox *current_sandbox = nullptr;
	ObjectID tree_base_id;
	ObjectID script_instance_owner_id;
	// This instance's members, in the shared Sandbox's guest memory. Zero when
	// the program keeps none. A guest address (gaddr_t), spelled uint64_t here
	// because this header only forward-declares Sandbox.
	// See Sandbox::create_instance_record(). Taken on first use after a reload,
	// which is why it is mutable: reading a property is a const operation that
	// still has to run against this instance's own members.
	mutable uint64_t instance_base = 0;
	// Which machine the record above lives in. A reload replaces the machine and
	// with it the record, so the base is renewed rather than reused.
	mutable uint64_t instance_generation = 0;
	void call_method(const MethodInfo *p_method, uint64_t p_address,
			const Variant **p_args, int p_argcount, Variant &r_return,
			GDExtensionCallError &r_error);

	friend class SafeGDScript;

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
	void call_init();
	void notification(int p_notification, bool p_reversed) override;
	String to_string(bool *r_valid) override;
	void refcount_incremented() override;
	bool refcount_decremented() override;
	Ref<Script> get_script() const override;
	bool is_placeholder() const override;
	void property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) override;
	Variant property_get_fallback(const StringName &p_name, bool *r_valid) override;
	ScriptLanguage *_get_language() override;

	void reset_to(const PackedByteArray &p_elf_data);
	// This instance's record in the machine that is running now, allocated if the
	// machine has been replaced since it was last asked for.
	uint64_t current_instance_base() const;
	SafeGDScriptInstance(Object *p_owner, const Ref<SafeGDScript> p_script);
	~SafeGDScriptInstance();
};

class SafeGDScriptStaticInstance final : public ScriptInstanceExtension {
	SafeGDScript *script = nullptr;
	Sandbox *sandbox = nullptr;

	Sandbox *acquire();

public:
	bool set(const StringName &p_name, const Variant &p_value) override;
	bool get(const StringName &p_name, Variant &r_ret) const override;
	const GDExtensionPropertyInfo *get_property_list(uint32_t *r_count) const override;
	void free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count) const override;
	Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid) const override;
	bool validate_property(GDExtensionPropertyInfo &p_property) const override;
	bool get_class_category(GDExtensionPropertyInfo &r_class_category) const override;
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

	void release_sandbox();

	explicit SafeGDScriptStaticInstance(SafeGDScript *p_script);
	~SafeGDScriptStaticInstance() override;
};
