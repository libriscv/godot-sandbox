#include "script_instance.h"

#include "../sandbox.h"
#include "script_elf.h"
#include "script_instance_helper.h"
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/local_vector.hpp>
static constexpr bool VERBOSE_LOGGING = false;

static const std::vector<std::string> godot_functions = {
	"_get_editor_name",
	"_hide_script_from_inspector",
	"_is_read_only",
};

bool ELFScriptInstance::set(const StringName &p_name, const Variant &p_value) {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("ELFScriptInstance::set " + p_name);
	}
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
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("callp: script is null");
		}
		r_error.error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
		return Variant();
	} else if (p_method == StringName("_get_editor_name")) {
		r_error.error = GDEXTENSION_CALL_OK;
		return Variant("ELFScriptInstance");
	} else if (p_method == StringName("_hide_script_from_inspector")) {
		r_error.error = GDEXTENSION_CALL_OK;
		return false;
	} else if (p_method == StringName("_is_read_only")) {
		r_error.error = GDEXTENSION_CALL_OK;
		return false;
	}

	// When the script instance must have a sandbox as owner,
	// use _enter_tree to get the sandbox instance.
	// Also, avoid calling internal methods.
	if (this->sandbox == nullptr) {
		if (p_method == StringName("_enter_tree")) {
			// Get the Sandbox from the owner node
			Sandbox *sandbox_ptr = Object::cast_to<Sandbox>(this->owner);
			if (sandbox_ptr) {
				sandbox_ptr->set_program(script);
				// XXX: Is this kind of Sandbox its own relative tree base?
				sandbox_ptr->set_tree_base(sandbox_ptr);
			} else {
				ERR_PRINT("enter_tree: owner was not a Sandbox");
			}
		}
	}

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("method called " + p_method);
	}

retry_callp:
	if (script->functions.has(p_method)) {
		Sandbox *sandbox_ptr = this->get_sandbox();
		if (sandbox_ptr && sandbox_ptr->has_program_loaded()) {
			r_error.error = GDEXTENSION_CALL_OK;
			return sandbox_ptr->vmcall_fn(p_method, p_args, p_argument_count);
		}
	}

	// If the program has been loaded, but the method list has not been updated, update it and retry the vmcall
	if (!this->has_updated_methods) {
		auto *sandbox_ptr = this->get_sandbox();
		if (sandbox_ptr) {
			// Only update methods if the program is already loaded
			if (sandbox_ptr->has_program_loaded()) {
				this->update_methods();
				goto retry_callp;
			}
		}
	}

	r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
	return Variant();
}

GDExtensionMethodInfo create_method_info(const MethodInfo &method_info) {
	return GDExtensionMethodInfo{
		name : stringname_alloc(method_info.name),
		return_value : GDExtensionPropertyInfo{
			type : GDEXTENSION_VARIANT_TYPE_OBJECT,
			name : stringname_alloc(method_info.return_val.name),
			class_name : stringname_alloc(method_info.return_val.class_name),
			hint : method_info.return_val.hint,
			hint_string : stringname_alloc(method_info.return_val.hint_string),
			usage : method_info.return_val.usage
		},
		flags : method_info.flags,
		id : method_info.id,
		argument_count : (uint32_t)method_info.arguments.size(),
		arguments : nullptr,
		default_argument_count : 0,
		default_arguments : nullptr,
	};
}

void ELFScriptInstance::update_methods() const {
	if (script.is_null()) {
		return;
	}
	this->has_updated_methods = true;

	for (auto &function : script->functions) {
		MethodInfo method_info = MethodInfo(
				Variant::NIL,
				StringName(function));
		this->methods_info.push_back(method_info);
	}
}

const GDExtensionMethodInfo *ELFScriptInstance::get_method_list(uint32_t *r_count) const {
	if (script.is_null()) {
		*r_count = 0;
		return nullptr;
	}

	if (!this->has_updated_methods) {
		this->update_methods();
	}

	const int size = methods_info.size();
	GDExtensionMethodInfo *list = memnew_arr(GDExtensionMethodInfo, size);
	int i = 0;
	for (auto &method_info : methods_info) {
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
	const bool result = true;

	if constexpr (VERBOSE_LOGGING) {
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

ScriptLanguage *ELFScriptInstance::_get_language() {
	return get_elf_language();
}

ELFScriptInstance::ELFScriptInstance(Object *p_owner, const Ref<ELFScript> p_script) :
		owner(p_owner), script(p_script) {
	if (!Object::cast_to<Sandbox>(p_owner)) {
		this->sandbox = memnew(Sandbox);
		auto *node = godot::Object::cast_to<godot::Node>(p_owner);
		if (node == nullptr) {
			ERR_PRINT("ELFScriptInstance: owner is not a Node");
			return;
		}
		this->sandbox->set_tree_base(node);
		this->sandbox->set_program(p_script);
		//ERR_PRINT("ELFScriptInstance: owner is not a Sandbox");
		//fprintf(stderr, "ELFScriptInstance: owner is instead a '%s'!\n", p_owner->get_class().utf8().get_data());
	}
	for (auto godot_function : godot_functions) {
		MethodInfo method_info = MethodInfo(
				Variant::NIL,
				StringName(godot_function.c_str()));
		methods_info.push_back(method_info);
	}
}

ELFScriptInstance::~ELFScriptInstance() {
	if (this->sandbox != nullptr) {
		memdelete(this->sandbox);
	}
}

Sandbox *ELFScriptInstance::get_sandbox() const {
	if (this->sandbox != nullptr) {
		return this->sandbox;
	}

	Sandbox *sandbox_ptr = Object::cast_to<Sandbox>(this->owner);
	if (sandbox_ptr == nullptr) {
		ERR_PRINT("ELFScriptInstance: owner is not a Sandbox");
		fprintf(stderr, "ELFScriptInstance: owner is instead a '%s'!\n", this->owner->get_class().utf8().get_data());
		return nullptr;
	}

	return sandbox_ptr;
}
