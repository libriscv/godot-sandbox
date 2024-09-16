#include "script_instance.h"

#include "../cpp/script_cpp.h"
#include "../rust/script_rust.h"
#include "../sandbox.h"
#include "../zig/script_zig.h"
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

static void handle_language_warnings(Array &warnings, const Ref<ELFScript> &script) {
	const String language = script->get_elf_programming_language();
	if (language == "C++") {
		// Compare C++ version against Docker version
		const int docker_version = CPPScript::DockerContainerVersion();
		if (docker_version < 0) {
			warnings.push_back("C++ Docker container not found");
		} else {
			const int script_version = script->get_elf_api_version();
			if (script_version < docker_version) {
				String w = "C++ API version is newer (" + String::num_int64(script_version) + " vs " + String::num_int64(docker_version) + "), please rebuild the program";
				warnings.push_back(std::move(w));
			}
		}
	} else if (language == "Rust") {
		// Compare Rust version against Docker version
		const int docker_version = RustScript::DockerContainerVersion();
		if (docker_version < 0) {
			warnings.push_back("Rust Docker container not found");
		} else {
			const int script_version = script->get_elf_api_version();
			if (script_version < docker_version) {
				String w = "Rust API version is newer (" + String::num_int64(script_version) + " vs " + String::num_int64(docker_version) + "), please rebuild the program";
				warnings.push_back(std::move(w));
			}
		}
	} else if (language == "Zig") {
		// Compare Zig version against Docker version
		const int docker_version = ZigScript::DockerContainerVersion();
		if (docker_version < 0) {
			warnings.push_back("Zig Docker container not found");
		} else {
			const int script_version = script->get_elf_api_version();
			if (script_version < docker_version) {
				String w = "Zig API version is newer (" + String::num_int64(script_version) + " vs " + String::num_int64(docker_version) + "), please rebuild the program";
				warnings.push_back(std::move(w));
			}
		}
	}
}

bool ELFScriptInstance::set(const StringName &p_name, const Variant &p_value) {
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("ELFScriptInstance::set " + p_name);
	}

	auto [sandbox, created] = get_sandbox();
	if (sandbox) {
		return sandbox->set_property(p_name, p_value);
	}

	return false;
}

bool ELFScriptInstance::get(const StringName &p_name, Variant &r_ret) const {
	if (p_name == StringName("script")) {
		r_ret = script;
		return true;
	}
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("ELFScriptInstance::get " + p_name);
	}

	auto [sandbox, created] = get_sandbox();
	if (sandbox) {
		return sandbox->get_property(p_name, r_ret);
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
	}

retry_callp:
	if (script->functions.has(p_method)) {
		if (current_sandbox && current_sandbox->has_program_loaded()) {
			// Set the Sandbox instance tree base to the owner node
			current_sandbox->set_tree_base(godot::Object::cast_to<Node>(this->owner));
			// Perform the vmcall
			r_error.error = GDEXTENSION_CALL_OK;
			return current_sandbox->vmcall_fn(p_method, p_args, p_argument_count);
		}
	}

	// Handle internal methods
	if (p_method == StringName("_get_editor_name")) {
		r_error.error = GDEXTENSION_CALL_OK;
		return Variant("ELFScriptInstance");
	} else if (p_method == StringName("_hide_script_from_inspector")) {
		r_error.error = GDEXTENSION_CALL_OK;
		return false;
	} else if (p_method == StringName("_is_read_only")) {
		r_error.error = GDEXTENSION_CALL_OK;
		return false;
	} else if (p_method == StringName("_get_configuration_warnings")) {
		// Returns an array of strings with warnings about the script configuration
		Array warnings;
		if (script->functions.is_empty()) {
			warnings.push_back("No public functions found");
		}
		if (script->get_elf_programming_language() == "Unknown") {
			warnings.push_back("Unknown programming language");
		}
		handle_language_warnings(warnings, script);
		r_error.error = GDEXTENSION_CALL_OK;
		return warnings;
	}

	// When the script instance must have a sandbox as owner,
	// use _enter_tree to get the sandbox instance.
	// Also, avoid calling internal methods.
	if (!this->auto_created_sandbox) {
		if (p_method == StringName("_enter_tree")) {
			current_sandbox->set_program(script);
		}
	}

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("method called " + p_method);
	}

	// If the program has been loaded, but the method list has not been updated, update it and retry the vmcall
	if (!this->has_updated_methods) {
		if (current_sandbox) {
			// Only update methods if the program is already loaded
			if (current_sandbox->has_program_loaded()) {
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
		.name = stringname_alloc(method_info.name),
		.return_value = GDExtensionPropertyInfo{
				.type = GDEXTENSION_VARIANT_TYPE_OBJECT,
				.name = stringname_alloc(method_info.return_val.name),
				.class_name = stringname_alloc(method_info.return_val.class_name),
				.hint = method_info.return_val.hint,
				.hint_string = stringname_alloc(method_info.return_val.hint_string),
				.usage = method_info.return_val.usage },
		.flags = method_info.flags,
		.id = method_info.id,
		.argument_count = (uint32_t)method_info.arguments.size(),
		.arguments = nullptr,
		.default_argument_count = 0,
		.default_arguments = nullptr,
	};
}

void ELFScriptInstance::update_methods() const {
	if (script.is_null()) {
		return;
	}
	this->has_updated_methods = true;

	for (String &function : script->functions) {
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
	for (godot::MethodInfo &method_info : methods_info) {
		list[i] = create_method_info(method_info);
		i++;
	}

	*r_count = size;
	return list;
}

const GDExtensionPropertyInfo *ELFScriptInstance::get_property_list(uint32_t *r_count) const {
	auto [sandbox, created] = get_sandbox();
	if (!sandbox) {
		if constexpr (VERBOSE_LOGGING) {
			printf("ELFScriptInstance::get_property_list: no sandbox\n");
		}
		*r_count = 0;
		return nullptr;
	}

	std::vector<SandboxProperty> &properties = sandbox->get_properties();
	*r_count = properties.size();
	GDExtensionPropertyInfo *list = memnew_arr(GDExtensionPropertyInfo, properties.size());
	const GDExtensionPropertyInfo *list_ptr = list;

	for (SandboxProperty &property : properties) {
		if constexpr (VERBOSE_LOGGING) {
			printf("ELFScriptInstance::get_property_list %s\n", property.name().utf8().ptr());
		}
		list->name = stringname_alloc(property.name());
		list->class_name = stringname_alloc("Variant");
		list->type = (GDExtensionVariantType)property.type();
		list->hint = 0;
		list->hint_string = string_alloc("");
		list->usage = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_SCRIPT_VARIABLE;
		list++;
	}

	return list_ptr;
}
void ELFScriptInstance::free_property_list(const GDExtensionPropertyInfo *p_list) const {
	if (p_list) {
		memdelete_arr(p_list);
	}
}

Variant::Type ELFScriptInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	auto [sandbox, created] = get_sandbox();
	if (sandbox) {
		if (const SandboxProperty *prop = sandbox->find_property_or_null(p_name)) {
			*r_is_valid = true;
			return prop->type();
		}
	}
	*r_is_valid = false;
	return Variant::NIL;
}

void ELFScriptInstance::get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
}

bool ELFScriptInstance::validate_property(GDExtensionPropertyInfo &p_property) const {
	auto [sandbox, created] = get_sandbox();
	if (!sandbox) {
		if constexpr (VERBOSE_LOGGING) {
			printf("ELFScriptInstance::validate_property: no sandbox\n");
		}
		return false;
	}
	for (SandboxProperty &property : sandbox->get_properties()) {
		if (*(StringName *)p_property.name == StringName(property.name())) {
			if constexpr (VERBOSE_LOGGING) {
				printf("ELFScriptInstance::validate_property %s => true\n", property.name().utf8().ptr());
			}
			return true;
		}
	}
	if constexpr (VERBOSE_LOGGING) {
		printf("ELFScriptInstance::validate_property %s => false\n", String(*(StringName *)p_property.name).utf8().ptr());
	}
	return false;
}

bool ELFScriptInstance::has_method(const StringName &p_name) const {
	if (script.is_null()) {
		return true;
	}
	bool result = false;
	for (const std::string &function : godot_functions) {
		if (p_name == StringName(function.c_str())) {
			result = true;
			break;
		}
	}
	if (!result) {
		for (String &function : script->functions) {
			if (p_name == StringName(function)) {
				result = true;
				break;
			}
		}
	}

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
		memdelete_arr(p_list);
	}
}

bool ELFScriptInstance::property_can_revert(const StringName &p_name) const {
	auto [sandbox, created] = get_sandbox();
	if (!sandbox) {
		return false;
	}
	if (const SandboxProperty *prop = sandbox->find_property_or_null(p_name)) {
		return true;
	}
	return false;
}

bool ELFScriptInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
	auto [sandbox, created] = get_sandbox();
	if (!sandbox) {
		return false;
	}
	if (const SandboxProperty *prop = sandbox->find_property_or_null(p_name)) {
		r_ret = prop->default_value();
		return true;
	}
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
	this->current_sandbox = Object::cast_to<Sandbox>(p_owner);
	this->auto_created_sandbox = (this->current_sandbox == nullptr);
	if (auto_created_sandbox) {
		this->current_sandbox = create_sandbox(p_script);
		//ERR_PRINT("ELFScriptInstance: owner is not a Sandbox");
		//fprintf(stderr, "ELFScriptInstance: owner is instead a '%s'!\n", p_owner->get_class().utf8().get_data());
	}
	this->current_sandbox->set_tree_base(godot::Object::cast_to<godot::Node>(owner));

	for (std::string godot_function : godot_functions) {
		MethodInfo method_info = MethodInfo(
				Variant::NIL,
				StringName(godot_function.c_str()));
		methods_info.push_back(method_info);
	}
}

ELFScriptInstance::~ELFScriptInstance() {
}

// When a Sandbox needs to be automatically created, we instead share it
// across all instances of the same script. This is done to save an
// enormous amount of memory, as each Node using an ELFScriptInstance would
// otherwise have its own Sandbox instance.
static std::unordered_map<ELFScript *, Sandbox *> sandbox_instances;

std::tuple<Sandbox *, bool> ELFScriptInstance::get_sandbox() const {
	auto it = sandbox_instances.find(this->script.ptr());
	if (it != sandbox_instances.end()) {
		return { it->second, true };
	}

	Sandbox *sandbox_ptr = Object::cast_to<Sandbox>(this->owner);
	if (sandbox_ptr == nullptr) {
		ERR_PRINT("ELFScriptInstance: owner is not a Sandbox");
		if constexpr (VERBOSE_LOGGING) {
			fprintf(stderr, "ELFScriptInstance: owner is instead a '%s'!\n", this->owner->get_class().utf8().get_data());
		}
		return { nullptr, false };
	}

	return { sandbox_ptr, false };
}

Sandbox *ELFScriptInstance::create_sandbox(const Ref<ELFScript> &p_script) {
	auto it = sandbox_instances.find(p_script.ptr());
	if (it != sandbox_instances.end()) {
		return it->second;
	}

	Sandbox *sandbox_ptr = memnew(Sandbox);
	sandbox_ptr->set_program(p_script);
	sandbox_instances.insert_or_assign(p_script.ptr(), sandbox_ptr);
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("ELFScriptInstance: created sandbox for " + Object::cast_to<Node>(owner)->get_name());
	}

	return sandbox_ptr;
}
