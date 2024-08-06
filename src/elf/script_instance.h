
#pragma once

#include <gdextension_interface.h>

#include <gdextension_interface.h>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/mutex.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/core/type_info.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/templates/pair.hpp>
#include <godot_cpp/templates/self_list.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

class ELFScript;

class ELFScriptInstance {
	Ref<ELFScript> script;
	Object *owner;
	// allocates list with an int at the front saying how long it is
	template <typename T>
	static T *alloc_with_len(int p_size) {
		uint64_t list_size = sizeof(T) * p_size;
		void *ptr = memalloc(list_size + sizeof(int));

		*((int *)ptr) = p_size;

		return (T *)((int *)ptr + 1);
	}

public:
	static void init_script_instance_info_common(GDExtensionScriptInstanceInfo2 &p_info);

	enum PropertySetGetError {
		PROP_OK,
		PROP_NOT_FOUND,
		PROP_WRONG_TYPE,
		PROP_READ_ONLY,
		PROP_WRITE_ONLY,
		PROP_GET_FAILED,
		PROP_SET_FAILED
	};

	void call(const StringName &p_method, const Variant *const *p_args, const GDExtensionInt p_argument_count, Variant *r_return, GDExtensionCallError *r_error);

	void notification(int32_t p_what);
	void to_string(GDExtensionBool *r_is_valid, String *r_out);
	bool set(const StringName &p_name, const Variant &p_value, PropertySetGetError *r_err = nullptr);
	bool get(const StringName &p_name, Variant &r_ret, PropertySetGetError *r_err = nullptr);

	void get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata);
	void get_property_state(List<Pair<StringName, Variant>> &p_list);

	GDExtensionPropertyInfo *get_property_list(uint32_t *r_count);
	void free_property_list(const GDExtensionPropertyInfo *p_list) const;
	bool validate_property(GDExtensionPropertyInfo *p_property) const { return false; }

	Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid) const;

	GDExtensionMethodInfo *get_method_list(uint32_t *r_count) const;
	void free_method_list(const GDExtensionMethodInfo *p_list) const;

	bool has_method(const StringName &p_name) const;

	Object *get_owner() const { return owner; };
	Ref<ELFScript> get_script() const { return script; };
	ScriptLanguage *get_language() const;

	ELFScriptInstance(const Ref<ELFScript> p_script, Object *p_owner);
	~ELFScriptInstance();
};
