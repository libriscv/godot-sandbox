#include "script_instance_safegdscript.h"

#include "../elf/script_elf.h"
#include "../elf/script_instance.h"
#include "../elf/script_instance_helper.h"
#include "../scoped_tree_base.h"
#include "script_safegdscript.h"
#include "script_language_safegdscript.h"
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/local_vector.hpp>
static constexpr bool VERBOSE_LOGGING = false;

bool SafeGDScriptInstance::set(const StringName &p_name, const Variant &p_value) {
	ScopedTreeBase stb(sandbox, godot::Object::cast_to<Node>(this->owner));
	sandbox->set(p_name, p_value);
	return true;
}

bool SafeGDScriptInstance::get(const StringName &p_name, Variant &r_ret) const {
	static const StringName s_script("script");
	if (p_name == s_script) {
		r_ret = this->script;
		return true;
	}

	ScopedTreeBase stb(sandbox, godot::Object::cast_to<Node>(this->owner));
	r_ret = sandbox->get(p_name);
	return true;
}

godot::String SafeGDScriptInstance::to_string(bool *r_is_valid) {
	return "<SafeGDScript>";
}

void SafeGDScriptInstance::notification(int32_t p_what, bool p_reversed) {
}

Variant SafeGDScriptInstance::callp(
		const StringName &p_method,
		const Variant **p_args, const int p_argument_count,
		GDExtensionCallError &r_error)
{
	const auto address = sandbox->cached_address_of(p_method.hash(), p_method);
	if (address == 0) {
		r_error.error = GDExtensionCallErrorType::GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}
	//WARN_PRINT("SafeGDScriptInstance::callp: Calling method " + p_method + " at address " + itos(address) + " with " + itos(p_argument_count) + " arguments.");
	ScopedTreeBase stb(sandbox, godot::Object::cast_to<Node>(this->owner));
	return sandbox->vmcall_address(address, p_args, p_argument_count, r_error);
}

const GDExtensionMethodInfo *SafeGDScriptInstance::get_method_list(uint32_t *r_count) const {
	const int size = script->methods_info.size();
	GDExtensionMethodInfo *list = memnew_arr(GDExtensionMethodInfo, size);
	int i = 0;
	for (const godot::MethodInfo &method_info : script->methods_info) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("ELFScriptInstance::get_method_list: method " + String(method_info.name));
		}
		list[i] = create_method_info(method_info);
		i++;
	}

	*r_count = size;
	return list;
}

static void set_property_info(
		GDExtensionPropertyInfo &p_info,
		const StringName &p_name,
		const StringName &p_class_name,
		GDExtensionVariantType p_type,
		uint32_t p_hint,
		const String &p_hint_string,
		uint32_t p_usage)
{
	p_info.name = stringname_alloc(p_name);
	p_info.class_name = stringname_alloc(p_class_name);
	p_info.type = p_type;
	p_info.hint = p_hint;
	p_info.hint_string = string_alloc(p_hint_string);
	p_info.usage = p_usage;
}

const GDExtensionPropertyInfo *SafeGDScriptInstance::get_property_list(uint32_t *r_count) const {
	std::vector<PropertyInfo> prop_list = sandbox->create_sandbox_property_list();

	// Sandboxed properties
	const std::vector<SandboxProperty> &properties = sandbox->get_properties();

	*r_count = properties.size() + prop_list.size();
	GDExtensionPropertyInfo *list = memnew_arr(GDExtensionPropertyInfo, *r_count + 2);
	const GDExtensionPropertyInfo *list_ptr = list;

	for (const SandboxProperty &property : properties) {
		if constexpr (VERBOSE_LOGGING) {
			printf("SafeGDScriptInstance::get_property_list %s\n", String(property.name()).utf8().ptr());
			fflush(stdout);
		}
		list->name = stringname_alloc(property.name());
		list->class_name = stringname_alloc("Variant");
		list->type = (GDExtensionVariantType)property.type();
		list->hint = 0;
		list->hint_string = string_alloc("");
		list->usage = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_SCRIPT_VARIABLE | PROPERTY_USAGE_NIL_IS_VARIANT;
		list++;
	}
	for (int i = 0; i < prop_list.size(); i++) {
		const PropertyInfo &prop = prop_list[i];
		if constexpr (VERBOSE_LOGGING) {
			printf("SafeGDScriptInstance::get_property_list %s\n", String(prop.name).utf8().ptr());
			fflush(stdout);
		}
		list->name = stringname_alloc(prop.name);
		list->class_name = stringname_alloc(prop.class_name);
		list->type = (GDExtensionVariantType) int(prop.type);
		list->hint = prop.hint;
		list->hint_string = string_alloc(prop.hint_string);
		list->usage = prop.usage;
		list++;
	}

	return list_ptr;
}
void SafeGDScriptInstance::free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count) const {
	if (p_list) {
		memdelete_arr(p_list);
	}
}

Variant::Type SafeGDScriptInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScriptInstance::get_property_type " + p_name);
	}
	if (const SandboxProperty *prop = sandbox->find_property_or_null(p_name)) {
		*r_is_valid = true;
		return prop->type();
	}
	*r_is_valid = false;
	return Variant::NIL;
}

void SafeGDScriptInstance::get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
}

bool SafeGDScriptInstance::validate_property(GDExtensionPropertyInfo &p_property) const {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScriptInstance::validate_property");
	}
	return true;
}

GDExtensionInt SafeGDScriptInstance::get_method_argument_count(const StringName &p_method, bool &r_valid) const {
	r_valid = false;
	return 0;
}

bool SafeGDScriptInstance::has_method(const StringName &p_name) const {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScriptInstance::has_method " + p_name);
	}
	for (const godot::MethodInfo &method_info : script->methods_info) {
		if (method_info.name == p_name) {
			return true;
		}
	}
	return false;
}

void SafeGDScriptInstance::free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count) const {
	if (p_list) {
		for (uint32_t i = 0; i < p_count; i++) {
			const GDExtensionMethodInfo &method_info = p_list[i];
			if (method_info.arguments) {
				memdelete_arr(method_info.arguments);
			}
		}
		memdelete_arr(p_list);
	}
}

bool SafeGDScriptInstance::property_can_revert(const StringName &p_name) const {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScriptInstance::property_can_revert " + p_name);
	}
	return false;
}

bool SafeGDScriptInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScriptInstance::property_get_revert " + p_name);
	}
	r_ret = Variant();
	return false;
}

void SafeGDScriptInstance::refcount_incremented() {
}

bool SafeGDScriptInstance::refcount_decremented() {
	return false;
}

Object *SafeGDScriptInstance::get_owner() {
	return owner;
}

Ref<Script> SafeGDScriptInstance::get_script() const {
	return script;
}

bool SafeGDScriptInstance::is_placeholder() const {
	return false;
}

void SafeGDScriptInstance::property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) {
	*r_valid = false;
}

Variant SafeGDScriptInstance::property_get_fallback(const StringName &p_name, bool *r_valid) {
	*r_valid = false;
	return Variant::NIL;
}

ScriptLanguage *SafeGDScriptInstance::_get_language() {
	return SafeGDScriptLanguage::get_singleton();
}

SafeGDScriptInstance::SafeGDScriptInstance(Object *p_owner, const Ref<SafeGDScript> p_script) :
		owner(p_owner), script(p_script)
{
	sandbox = memnew(Sandbox);
	sandbox->set_tree_base(Object::cast_to<Node>(p_owner));
	sandbox->set_unboxed_arguments(false);
	sandbox->load_buffer(script->elf_data);

	script->update_methods_info(sandbox);
}

SafeGDScriptInstance::~SafeGDScriptInstance() {
	memdelete(sandbox);
}

void SafeGDScriptInstance::reset_to(const PackedByteArray &p_elf_data) {
	sandbox->load_buffer(p_elf_data);
}
