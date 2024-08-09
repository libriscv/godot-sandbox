#include "script_instance.h"

#include "script_elf.h"
#include "script_instance_helper.h"
#include "../sandbox.h"
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/core/object.hpp>
static constexpr bool VERBOSE_METHODS = false;

static std::vector<std::string> godot_functions = {
	"set_owner",
	"_get_editor_name",
	"_hide_script_from_inspector",
	"_is_read_only",
};

bool ELFScriptInstance::set(const StringName &p_name, const Variant &p_value) {
	ERR_PRINT("ELFScriptInstance::set " + p_name);
	return false;
}

bool ELFScriptInstance::get(const StringName &p_name, Variant &r_ret) const {
	if (p_name == StringName("script")) {
		r_ret = script;
		return true;
	}
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
	ERR_PRINT("ELFScriptInstance::callp " + p_method);
	if (p_method == StringName("set_owner")) {
		if constexpr (VERBOSE_METHODS) {
			printf("set_owner called %p -> %p\n", this->owner, p_args[0]->operator Object *());
		}
		this->owner = p_args[0]->operator Object *();
		Sandbox* sandbox = Object::cast_to<Sandbox>(this->owner);
		if (sandbox) {
			sandbox->set_program(script);
		} else {
			ERR_PRINT("set_owner: owner is not a Sandbox");
		}
		r_error.error = GDEXTENSION_CALL_OK;
		return true;
	} else if (p_method == StringName("_get_editor_name")) {
		r_error.error = GDEXTENSION_CALL_OK;
		return Variant("ELFScriptInstance");
	} else if (p_method == StringName("_hide_script_from_inspector")) {
		r_error.error = GDEXTENSION_CALL_OK;
		return false;
	} else if (p_method == StringName("_is_read_only")) {
		r_error.error = GDEXTENSION_CALL_OK;
		return true;
	}

	if constexpr (VERBOSE_METHODS) {
		ERR_PRINT("method called " + p_method);
	}

	Sandbox* sandbox = Object::cast_to<Sandbox>(this->owner);
	if (sandbox) {
		r_error.error = GDEXTENSION_CALL_OK;
		return sandbox->vmcall_fn(p_method, p_args, p_argument_count);
	} else {
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		ERR_PRINT("callp: owner is not a Sandbox");
		return Variant();
	}
}

GDExtensionMethodInfo create_method_info(const MethodInfo &method_info) {
	return GDExtensionMethodInfo{
		name: method_info.name._native_ptr(),
		return_value: GDExtensionPropertyInfo {
			// what to put here?
			type: GDEXTENSION_VARIANT_TYPE_STRING,
			name: method_info.return_val.name._native_ptr(),
			hint: method_info.return_val.hint,
			hint_string: method_info.return_val.hint_string._native_ptr(),
			class_name: method_info.return_val.class_name._native_ptr(),
			usage: method_info.return_val.usage
		},
		flags: method_info.flags,
		id: method_info.id,
		argument_count: 0,// TODO (uint32_t)method_info.arguments.size(),
		arguments: nullptr,
		default_argument_count: 0,
		default_arguments: nullptr,
	};
}

const GDExtensionMethodInfo *ELFScriptInstance::get_method_list(uint32_t *r_count) const {
	if (script.is_null()) {
		*r_count = 0;
		return nullptr;
	}

	const int size = godot_functions.size();
	GDExtensionMethodInfo *list = memnew_arr(GDExtensionMethodInfo, size);
	int i = 0;
	for (auto godot_function : godot_functions) {
		MethodInfo method_info = MethodInfo(StringName(godot_function.c_str()));
		list[i] = create_method_info(method_info);
		i++;
	}

	*r_count = size;

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
	bool result = false;
	if (p_name == StringName("set_owner") || p_name == StringName("_get_editor_name") || p_name == StringName("_hide_script_from_inspector") || p_name == StringName("_is_read_only")) {
		result = true;
	}

	if constexpr (VERBOSE_METHODS) {
		fprintf(stderr, "ELFScriptInstance::has_method %s => %d\n", p_name.to_ascii_buffer().ptr(), result);
	}
	return result;
}

void ELFScriptInstance::free_method_list(const GDExtensionMethodInfo *p_list) const {
	// @todo We really need to know the size of the list... Unless we can store the info elsewhere and just use pointers?
	// what godot does in their free_method list:

	//   /* TODO `GDExtensionClassFreePropertyList` is ill-defined, we need a non-const pointer to free this. */                                                                    \
	// ::godot::internal::free_c_property_list(const_cast<GDExtensionPropertyInfo *>(p_list));                                                                               
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
		owner(p_owner), script(p_script) {
}

ELFScriptInstance::~ELFScriptInstance() {
}
