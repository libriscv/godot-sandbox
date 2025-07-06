#include "script_cpp_instance.h"

#include "script_cpp.h"
#include "../elf/script_elf.h"
#include "../elf/script_instance.h"
#include "../elf/script_instance_helper.h" // register_types.h
#include "../scoped_tree_base.h"
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/local_vector.hpp>
static constexpr bool VERBOSE_LOGGING = false;

bool CPPScriptInstance::set(const StringName &p_name, const Variant &p_value) {
	if (elf_script_instance) {
		return elf_script_instance->set(p_name, p_value);
	}
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScriptInstance::set " + p_name);
	}
	return false;
}

bool CPPScriptInstance::get(const StringName &p_name, Variant &r_ret) const {
	if (elf_script_instance) {
		return elf_script_instance->get(p_name, r_ret);
	}
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScriptInstance::get " + p_name);
	}
	return false;
}

godot::String CPPScriptInstance::to_string(bool *r_is_valid) {
	return "<CPPScript>";
}

void CPPScriptInstance::notification(int32_t p_what, bool p_reversed) {
}

Variant CPPScriptInstance::callp(
		const StringName &p_method,
		const Variant **p_args, const int p_argument_count,
		GDExtensionCallError &r_error)
{
	if (elf_script_instance) {
		Ref<ELFScript> script = elf_script_instance->script;
		if (!script.is_valid()) {
			if constexpr (VERBOSE_LOGGING) {
				ERR_PRINT("CPPScriptInstance::callp: script is null");
			}
			r_error.error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
			return Variant();
		}

		// Try to call the method on the elf_script_instance, but use
		// this instance owner as the base for the Sandbox node-tree.
		if (script->function_names.has(p_method)) {
			auto [sandbox, auto_created] = elf_script_instance->get_sandbox();
			if (sandbox && sandbox->has_program_loaded()) {
				// Set the Sandbox instance tree base to the owner node
				ScopedTreeBase stb(sandbox, godot::Object::cast_to<Node>(this->owner));
				// Perform the vmcall
				return sandbox->vmcall_fn(p_method, p_args, p_argument_count, r_error);
			}
		}
		// Fallback: callp on the elf_script_instance directly
		return elf_script_instance->callp(p_method, p_args, p_argument_count, r_error);
	}
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScriptInstance::callp " + p_method);
	}
	r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
	return Variant();
}

void CPPScriptInstance::update_methods() const {
	if (elf_script_instance) {
		elf_script_instance->update_methods();
		return;
	}

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScriptInstance::update_methods called without elf_script_instance");
	}
}

const GDExtensionMethodInfo *CPPScriptInstance::get_method_list(uint32_t *r_count) const {
	if (elf_script_instance) {
		return elf_script_instance->get_method_list(r_count);
	}

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScriptInstance::get_method_list");
	}

	// If no methods are defined, return an empty list
	*r_count = 0;
	return nullptr;
}

const GDExtensionPropertyInfo *CPPScriptInstance::get_property_list(uint32_t *r_count) const {
	if (elf_script_instance) {
		return elf_script_instance->get_property_list(r_count);
	}

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScriptInstance::get_property_list");
	}

	// If no properties are defined, return an empty list
	*r_count = 0;
	return nullptr;
}
void CPPScriptInstance::free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count) const {
	if (p_list) {
		memdelete_arr(p_list);
	}
}

Variant::Type CPPScriptInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	if (elf_script_instance) {
		return elf_script_instance->get_property_type(p_name, r_is_valid);
	}
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScriptInstance::get_property_type " + p_name);
	}
	*r_is_valid = false;
	return Variant::NIL;
}

void CPPScriptInstance::get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
}

bool CPPScriptInstance::validate_property(GDExtensionPropertyInfo &p_property) const {
	if (elf_script_instance) {
		return elf_script_instance->validate_property(p_property);
	}
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScriptInstance::validate_property");
	}
	return false;
}

GDExtensionInt CPPScriptInstance::get_method_argument_count(const StringName &p_method, bool &r_valid) const {
	r_valid = false;
	return 0;
}

bool CPPScriptInstance::has_method(const StringName &p_name) const {
	if (elf_script_instance) {
		return elf_script_instance->has_method(p_name);
	}
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScriptInstance::has_method " + p_name);
	}
	return false;
}

void CPPScriptInstance::free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count) const {
	if (elf_script_instance) {
		elf_script_instance->free_method_list(p_list, p_count);
	}
}

bool CPPScriptInstance::property_can_revert(const StringName &p_name) const {
	if (elf_script_instance) {
		return elf_script_instance->property_can_revert(p_name);
	}
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScriptInstance::property_can_revert " + p_name);
	}
	return false;
}

bool CPPScriptInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
	if (elf_script_instance)
		return elf_script_instance->property_get_revert(p_name, r_ret);
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScriptInstance::property_get_revert " + p_name);
	}
	r_ret = Variant::NIL;
	return false;
}

void CPPScriptInstance::refcount_incremented() {
}

bool CPPScriptInstance::refcount_decremented() {
	return false;
}

Object *CPPScriptInstance::get_owner() {
	return owner;
}

Ref<Script> CPPScriptInstance::get_script() const {
	return script;
}

bool CPPScriptInstance::is_placeholder() const {
	return false;
}

void CPPScriptInstance::property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) {
	*r_valid = false;
}

Variant CPPScriptInstance::property_get_fallback(const StringName &p_name, bool *r_valid) {
	*r_valid = false;
	return Variant::NIL;
}

ScriptLanguage *CPPScriptInstance::_get_language() {
	return get_elf_language();
}

CPPScriptInstance::CPPScriptInstance(Object *p_owner, const Ref<CPPScript> p_script) :
		owner(p_owner), script(p_script)
{
}

CPPScriptInstance::~CPPScriptInstance() {
	if (this->script.is_valid()) {
		script->instances.erase(this);
	}
}
