#include "placeholder_instance_safegdscript.h"

#include "../elf/script_instance_helper.h"
#include "script_safegdscript.h"
#include "script_language_safegdscript.h"

#include <godot_cpp/classes/global_constants.hpp>

namespace {
void fill_property_info(GDExtensionPropertyInfo &out, const PropertyInfo &property) {
	out.name = stringname_alloc(property.name);
	out.class_name = stringname_alloc(property.class_name);
	out.type = GDExtensionVariantType(property.type);
	out.hint = property.hint;
	out.hint_string = string_alloc(property.hint_string);
	out.usage = property.usage;
}
}

bool SafeGDScriptPlaceholderInstance::set(const StringName &p_name, const Variant &p_value) {
	if (script->find_property_signature(p_name) == nullptr) {
		return false;
	}
	if (!script->find_property_signature(p_name)->is_member) return false;
	values.insert(p_name, p_value);
	fallback_values.erase(p_name);
	return true;
}

bool SafeGDScriptPlaceholderInstance::get(const StringName &p_name, Variant &r_ret) const {
	if (const Variant *value = values.getptr(p_name)) {
		r_ret = *value;
		return true;
	}
	if (script->property_default(p_name, r_ret)) {
		return true;
	}
	return false;
}

const GDExtensionPropertyInfo *SafeGDScriptPlaceholderInstance::get_property_list(uint32_t *r_count) const {
	const std::vector<PropertyInfo> infos = script->member_property_infos();
	*r_count = uint32_t(infos.size());
	GDExtensionPropertyInfo *list = memnew_arr(GDExtensionPropertyInfo, *r_count);
	for (uint32_t at = 0; at < *r_count; at++) {
		fill_property_info(list[at], infos[at]);
	}
	return list;
}

void SafeGDScriptPlaceholderInstance::free_property_list(const GDExtensionPropertyInfo *p_list,
		uint32_t p_count) const {
	if (p_list != nullptr) {
		memdelete_arr(p_list);
	}
}

Variant::Type SafeGDScriptPlaceholderInstance::get_property_type(const StringName &p_name,
		bool *r_is_valid) const {
	const gdscript::PropertySignature *property = script->find_property_signature(p_name);
	*r_is_valid = property != nullptr;
	return property == nullptr || property->type < 0 ? Variant::NIL : Variant::Type(property->type);
}

bool SafeGDScriptPlaceholderInstance::validate_property(GDExtensionPropertyInfo &p_property) const {
	return true;
}

bool SafeGDScriptPlaceholderInstance::property_can_revert(const StringName &p_name) const {
	return script->property_defaults.has(p_name);
}

bool SafeGDScriptPlaceholderInstance::property_get_revert(const StringName &p_name,
		Variant &r_ret) const {
	return script->property_default(p_name, r_ret);
}

Object *SafeGDScriptPlaceholderInstance::get_owner() { return owner; }

void SafeGDScriptPlaceholderInstance::get_property_state(
		GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
	auto add = [&](const HashMap<StringName, Variant> &map) {
		for (const KeyValue<StringName, Variant> &entry : map) {
			p_add_func(reinterpret_cast<GDExtensionConstStringNamePtr>(&entry.key),
					reinterpret_cast<GDExtensionConstVariantPtr>(&entry.value), p_userdata);
		}
	};
	add(values);
	add(fallback_values);
}

const GDExtensionMethodInfo *SafeGDScriptPlaceholderInstance::get_method_list(uint32_t *r_count) const {
	*r_count = 0;
	return nullptr;
}
void SafeGDScriptPlaceholderInstance::free_method_list(const GDExtensionMethodInfo *, uint32_t) const {}
bool SafeGDScriptPlaceholderInstance::has_method(const StringName &) const { return false; }
GDExtensionInt SafeGDScriptPlaceholderInstance::get_method_argument_count(const StringName &,
		bool &r_valid) const {
	r_valid = false;
	return 0;
}
Variant SafeGDScriptPlaceholderInstance::callp(const StringName &, const Variant **, int,
		GDExtensionCallError &r_error) {
	r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
	return Variant();
}
void SafeGDScriptPlaceholderInstance::notification(int, bool) {}
String SafeGDScriptPlaceholderInstance::to_string(bool *r_valid) {
	*r_valid = true;
	return "<SafeGDScriptPlaceholder>";
}
void SafeGDScriptPlaceholderInstance::refcount_incremented() {}
bool SafeGDScriptPlaceholderInstance::refcount_decremented() { return false; }
Ref<Script> SafeGDScriptPlaceholderInstance::get_script() const { return script; }
bool SafeGDScriptPlaceholderInstance::is_placeholder() const { return true; }

void SafeGDScriptPlaceholderInstance::property_set_fallback(const StringName &p_name,
		const Variant &p_value, bool *r_valid) {
	fallback_values.insert(p_name, p_value);
	*r_valid = true;
}
Variant SafeGDScriptPlaceholderInstance::property_get_fallback(const StringName &p_name,
		bool *r_valid) {
	if (const Variant *value = fallback_values.getptr(p_name)) {
		*r_valid = true;
		return *value;
	}
	*r_valid = false;
	return Variant();
}
ScriptLanguage *SafeGDScriptPlaceholderInstance::_get_language() {
	return SafeGDScriptLanguage::get_singleton();
}

void SafeGDScriptPlaceholderInstance::properties_changed(
		const std::vector<gdscript::PropertySignature> &p_old,
		const std::vector<gdscript::PropertySignature> &p_new) {
	HashSet<StringName> current;
	for (const gdscript::PropertySignature &property : p_new) {
		if (!property.is_member) continue;
		const StringName name = String::utf8(property.name.c_str(), property.name.size());
		current.insert(name);
		if (Variant *value = values.getptr(name); value != nullptr && property.type >= 0 &&
				value->get_type() != Variant::Type(property.type) &&
				!Variant::can_convert(value->get_type(), Variant::Type(property.type))) {
			fallback_values.insert(name, *value);
			values.erase(name);
		}
		if (!values.has(name)) {
			if (const Variant *fallback = fallback_values.getptr(name)) {
				const bool compatible = property.type < 0 ||
						fallback->get_type() == Variant::Type(property.type) ||
						Variant::can_convert(fallback->get_type(), Variant::Type(property.type));
				if (compatible) {
					values.insert(name, *fallback);
					fallback_values.erase(name);
				}
			}
		}
	}
	for (const gdscript::PropertySignature &property : p_old) {
		if (!property.is_member) continue;
		const StringName name = String::utf8(property.name.c_str(), property.name.size());
		if (current.has(name)) {
			continue;
		}
		if (const Variant *value = values.getptr(name)) {
			fallback_values.insert(name, *value);
			values.erase(name);
		}
	}
	if (owner != nullptr) {
		owner->notify_property_list_changed();
	}
}

SafeGDScriptPlaceholderInstance::SafeGDScriptPlaceholderInstance(Object *p_owner,
		const Ref<SafeGDScript> &p_script) : owner(p_owner), script(p_script) {}

SafeGDScriptPlaceholderInstance::~SafeGDScriptPlaceholderInstance() {
	if (script.is_valid()) {
		script->remove_placeholder(this);
	}
}
