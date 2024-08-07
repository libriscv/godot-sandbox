#include "script_instance.h"

#include "../register_types.h"
#include "script_elf.h"
#include "script_instance_helper.h"
#include <godot_cpp/templates/local_vector.hpp>

#define COMMON_SELF ((ELFScriptInstance *)p_self)

void ELFScriptInstance::init_script_instance_info_common(GDExtensionScriptInstanceInfo2 &p_info) {
	// Must initialize potentially unused struct fields to nullptr
	// (if not, causes segfault on MSVC).

	p_info.call_func = [](void *p_self, GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
		return COMMON_SELF->call(*((StringName *)p_method), (const Variant **)p_args, p_argument_count, (Variant *)r_return, r_error);
	};

	p_info.notification_func = [](void *p_self, int32_t p_what, GDExtensionBool p_reversed) {
		COMMON_SELF->notification(p_what);
	};

	p_info.to_string_func = [](void *p_self, GDExtensionBool *r_is_valid, GDExtensionStringPtr r_out) {
		COMMON_SELF->to_string(r_is_valid, (String *)r_out);
	};

	p_info.free_func = [](void *p_self) {
		memdelete(COMMON_SELF);
	};

	p_info.property_can_revert_func = nullptr;
	p_info.property_get_revert_func = nullptr;

	p_info.refcount_incremented_func = nullptr;
	p_info.refcount_decremented_func = [](void *) -> GDExtensionBool {
		// If false (default), object cannot die
		return true;
	};

	p_info.is_placeholder_func = nullptr;

	p_info.set_fallback_func = nullptr;
	p_info.get_fallback_func = nullptr;

	p_info.set_func = [](void *p_self, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) -> GDExtensionBool {
		return COMMON_SELF->set(*(const StringName *)p_name, *(const Variant *)p_value);
	};

	p_info.get_func = [](void *p_self, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) -> GDExtensionBool {
		return COMMON_SELF->get(*(const StringName *)p_name, *(Variant *)r_ret);
	};

	p_info.get_property_list_func = [](void *p_self, uint32_t *r_count) -> const GDExtensionPropertyInfo * {
		return COMMON_SELF->get_property_list(r_count);
	};

	p_info.free_property_list_func = [](void *p_self, const GDExtensionPropertyInfo *p_list) {
		COMMON_SELF->free_property_list(p_list);
	};

	p_info.validate_property_func = [](void *p_self, GDExtensionPropertyInfo *p_property) -> GDExtensionBool {
		return COMMON_SELF->validate_property(p_property);
	};

	p_info.get_owner_func = [](void *p_self) {
		return COMMON_SELF->get_owner()->_owner;
	};

	p_info.get_property_state_func = [](void *p_self, GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
		COMMON_SELF->get_property_state(p_add_func, p_userdata);
	};

	p_info.get_method_list_func = [](void *p_self, uint32_t *r_count) -> const GDExtensionMethodInfo * {
		return COMMON_SELF->get_method_list(r_count);
	};

	p_info.free_method_list_func = [](void *p_self, const GDExtensionMethodInfo *p_list) {
		COMMON_SELF->free_method_list(p_list);
	};

	p_info.get_property_type_func = [](void *p_self, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) -> GDExtensionVariantType {
		return (GDExtensionVariantType)COMMON_SELF->get_property_type(*(const StringName *)p_name, (bool *)r_is_valid);
	};

	p_info.has_method_func = [](void *p_self, GDExtensionConstStringNamePtr p_name) -> GDExtensionBool {
		return COMMON_SELF->has_method(*(const StringName *)p_name);
	};

	p_info.get_script_func = [](void *p_self) {
		return COMMON_SELF->get_script().ptr()->_owner;
	};

	p_info.get_language_func = [](void *p_self) {
		return COMMON_SELF->get_language()->_owner;
	};
}

bool ELFScriptInstance::set(const StringName &p_name, const Variant &p_value, PropertySetGetError *r_err) {
	// we don't set properties on ELF script
	return false;
}

bool ELFScriptInstance::get(const StringName &p_name, Variant &r_ret, PropertySetGetError *r_err) {
	// we don't get properties on ELF script
	return false;
}

void ELFScriptInstance::call(
		const StringName &p_method,
		const Variant *const *p_args, const GDExtensionInt p_argument_count,
		Variant *r_return, GDExtensionCallError *r_error) {
	const ELFScript *s = script.ptr();
	ERR_PRINT("method called " + p_method);
}

void ELFScriptInstance::to_string(GDExtensionBool *r_is_valid, String *r_out) {
	const ELFScript *s = script.ptr();
	*r_is_valid = true;
	*r_out = s->_get_global_name();
}

void ELFScriptInstance::notification(int32_t p_what) {
}

GDExtensionPropertyInfo *ELFScriptInstance::get_property_list(uint32_t *r_count) {
	const ELFScript *script = get_script().ptr();
	auto property_list = script->_get_script_property_list();
	int size = property_list.size();
	GDExtensionPropertyInfo *list = alloc_with_len<GDExtensionPropertyInfo>(size);
	for (int i = 0; i < size; i++) {
		Dictionary dictionary = property_list[i];
		list[i] = create_property_type(dictionary);
	}
	*r_count = property_list.size();

	return list;
}

void ELFScriptInstance::get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
	// ! refer to script_language.cpp get_property_state
	// the default implementation is not carried over to GDExtension

	uint32_t count = 0;
	GDExtensionPropertyInfo *props = get_property_list(&count);

	for (int i = 0; i < count; i++) {
		StringName *name = (StringName *)props[i].name;

		if (props[i].usage & PROPERTY_USAGE_STORAGE) {
			Variant value;
			bool is_valid = get(*name, value);

			if (is_valid) {
				p_add_func(name, &value, p_userdata);
			}
		}
	}

	free_property_list(props);
}

void ELFScriptInstance::get_property_state(List<Pair<StringName, Variant>> &p_list) {
	get_property_state(add_to_state, &p_list);
}

void ELFScriptInstance::free_property_list(const GDExtensionPropertyInfo *p_list) const {
	if (!p_list)
		return;

	// don't ask.
	int size = get_len_from_ptr(p_list);

	for (int i = 0; i < size; i++) {
		free_prop(p_list[i]);
	}

	free_with_len((GDExtensionPropertyInfo *)p_list);
}

GDExtensionMethodInfo *ELFScriptInstance::get_method_list(uint32_t *r_count) const {
	const ELFScript *script = get_script().ptr();
	auto method_list = script->get_method_list();
	int size = method_list.size();
	GDExtensionMethodInfo *list = alloc_with_len<GDExtensionMethodInfo>(size);
	for (int i = 0; i < size; i++) {
		Dictionary dictionary = method_list[i];
		String method_name = dictionary["name"];
		list[i] = GDExtensionMethodInfo{
			name : stringname_alloc(method_name),
			return_value : create_property_type(dictionary["return"]),
			flags : GDEXTENSION_METHOD_FLAG_NORMAL,
			// TODO check what this is.
			id : 0,
			/* Arguments: `default_arguments` is an array of size `argument_count`. */
			argument_count : 0,
			arguments : nullptr,
			// TODO not all languages support default args.
			/* Default arguments: `default_arguments` is an array of size `default_argument_count`. */
			default_argument_count : 0,
			default_arguments : nullptr,
		};
	}
	*r_count = method_list.size();

	return list;
}

void ELFScriptInstance::free_method_list(const GDExtensionMethodInfo *p_list) const {
	if (!p_list)
		return;

	// don't ask.
	int size = get_len_from_ptr(p_list);

	for (int i = 0; i < size; i++) {
		const GDExtensionMethodInfo &method = p_list[i];

		memdelete((StringName *)method.name);

		free_prop(method.return_value);

		if (method.argument_count > 0) {
			for (int i = 0; i < method.argument_count; i++)
				free_prop(method.arguments[i]);

			memdelete(method.arguments);
		}

		if (method.default_argument_count > 0)
			memdelete((Variant *)method.default_arguments);
	}

	free_with_len((GDExtensionMethodInfo *)p_list);
}

Variant::Type ELFScriptInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	*r_is_valid = false;
	return Variant::NIL;
}

bool ELFScriptInstance::has_method(const StringName &p_name) const {
	const ELFScript *s = script.ptr();

	// TODO
	return true;
}

ScriptLanguage *ELFScriptInstance::get_language() const {
	return get_elf_language();
}

ELFScriptInstance::ELFScriptInstance(const Ref<ELFScript> p_script, Object *p_owner) :
		script(p_script), owner(p_owner) {
	// this usually occurs in _instance_create, but that is marked const for ScriptExtension
	{
		//MutexLock lock(*LuauLanguage::singleton->lock.ptr());
		// p_script->instances.insert(p_owner->get_instance_id(), this);
	}
}

ELFScriptInstance::~ELFScriptInstance() {
	if (script.is_valid() && owner) {
		//MutexLock lock(*LuauLanguage::singleton->lock.ptr());
		//script->instances.erase(owner->get_instance_id());
	}
}
