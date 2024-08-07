#include "script_instance.h"

#include "script_elf.h"
#include "script_instance_helper.h"
#include <godot_cpp/templates/local_vector.hpp>

bool ELFScriptInstance::set(const StringName &p_name, const Variant &p_value) {
	if (p_name == StringName("script")) {
		script = p_value;
		return true;
	}

	// we don't set properties on ELF script
	return false;
}

bool ELFScriptInstance::get(const StringName &p_name, Variant &r_ret) const {
	if (p_name == StringName("script")) {
		r_ret = script;
		return true;
	}

	// we don't get properties on ELF script
	r_ret = Variant();
	return false;
}

godot::String ELFScriptInstance::to_string(bool *r_is_valid) {
	return "<ELFScript>";
}

void ELFScriptInstance::notification(int32_t p_what, bool p_reversed) {
}

Variant ELFScriptInstance::callp(
		const StringName &p_method,
		const Variant **p_args, const int p_argument_count,
		GDExtensionCallError &r_error) {
	if (script.is_null()) {
		r_error.error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
		return Variant();
	}

	ERR_PRINT("method called " + p_method);
	//return s->callv(p_method, p_args, p_argument_count);
	r_error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
	return Variant();
}

const GDExtensionMethodInfo *ELFScriptInstance::get_method_list(uint32_t *r_count) const {
	if (script.is_null()) {
		*r_count = 0;
		return nullptr;
	}

	int size = 0;

	*r_count = size;
	GDExtensionMethodInfo *list = memnew_arr(GDExtensionMethodInfo, size);

	return list;
}

const GDExtensionPropertyInfo *ELFScriptInstance::get_property_list(uint32_t *r_count) const {
	*r_count = 0;
	return nullptr;
}
void ELFScriptInstance::free_property_list(const GDExtensionPropertyInfo *p_list) const {
}

Variant::Type ELFScriptInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	*r_is_valid = false;
	return Variant::NIL;
}

void ELFScriptInstance::get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
}

bool ELFScriptInstance::validate_property(GDExtensionPropertyInfo &p_property) const {
	return false;
}

bool ELFScriptInstance::has_method(const StringName &p_name) const {
	const ELFScript *s = script.ptr();

	//fprintf(stderr, "method called %s\n", p_name.to_ascii_buffer().ptr());

	// TODO
	return false;
}

void ELFScriptInstance::free_method_list(const GDExtensionMethodInfo *p_list) const {
	// @todo We really need to know the size of the list... Unless we can store the info elsewhere and just use pointers?
	if (p_list) {
		//memfree((void *)p_list);
	}
}

bool ELFScriptInstance::property_can_revert(const StringName &p_name) const {
	return false;
}

bool ELFScriptInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
	return false;
}

void ELFScriptInstance::refcount_incremented() {
}

bool ELFScriptInstance::refcount_decremented() {
	return false;
}

Object *ELFScriptInstance::get_owner() {
	return owner;
}

Ref<Script> ELFScriptInstance::get_script() const {
	return script;
}

bool ELFScriptInstance::is_placeholder() const {
	return false;
}

void ELFScriptInstance::property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) {
	*r_valid = false;
}

Variant ELFScriptInstance::property_get_fallback(const StringName &p_name, bool *r_valid) {
	*r_valid = false;
	return Variant::NIL;
}

ScriptLanguage *ELFScriptInstance::get_language() {
	return get_elf_language();
}

ELFScriptInstance::ELFScriptInstance(Object *p_owner, const Ref<ELFScript> p_script) :
		script(p_script), owner(p_owner) {
}

ELFScriptInstance::~ELFScriptInstance() {
}
