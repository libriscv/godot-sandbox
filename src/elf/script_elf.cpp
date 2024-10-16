#include "script_elf.h"

#include "../docker.h"
#include "../register_types.h"
#include "../sandbox.h"
#include "script_instance.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

static constexpr bool VERBOSE_ELFSCRIPT = false;

static Dictionary prop_to_dict(const PropertyInfo &p_prop) {
	Dictionary d;
	d["name"] = p_prop.name;
	d["type"] = p_prop.type;
	d["class_name"] = p_prop.class_name;
	d["hint"] = p_prop.hint;
	d["hint_string"] = p_prop.hint_string;
	d["usage"] = p_prop.usage;
	return d;
}

static Dictionary method_to_dict(const MethodInfo &p_method) {
	Dictionary d;

	d["name"] = p_method.name;
	d["flags"] = p_method.flags;

	if (p_method.arguments.size() > 0) {
		Array args;
		for (const PropertyInfo &arg : p_method.arguments) {
			args.push_back(prop_to_dict(arg));
		}
		d["args"] = args;
	}

	if (p_method.default_arguments.size() > 0) {
		Array defaults;
		for (const Variant &value : p_method.default_arguments) {
			defaults.push_back(value);
		}
		d["default_args"] = defaults;
	}

	d["return"] = prop_to_dict(p_method.return_val);

	return d;
}

String ELFScript::_to_string() const {
	return "ELFScript::" + global_name;
}
bool ELFScript::_editor_can_reload_from_file() {
	return true;
}
void ELFScript::_placeholder_erased(void *p_placeholder) {}
bool ELFScript::_can_instantiate() const {
	return true;
}
Ref<Script> ELFScript::_get_base_script() const {
	return Ref<Script>();
}
StringName ELFScript::_get_global_name() const {
	return global_name;
}
bool ELFScript::_inherits_script(const Ref<Script> &p_script) const {
	return false;
}
StringName ELFScript::_get_instance_base_type() const {
	return StringName("Sandbox");
}
void *ELFScript::_instance_create(Object *p_for_object) const {
	ELFScriptInstance *instance = memnew(ELFScriptInstance(p_for_object, Ref<ELFScript>(this)));
	instances.insert(instance);
	return ScriptInstanceExtension::create_native_instance(instance);
}
void *ELFScript::_placeholder_instance_create(Object *p_for_object) const {
	return _instance_create(p_for_object);
}
bool ELFScript::_instance_has(Object *p_object) const {
	return false;
}
bool ELFScript::_has_source_code() const {
	return true;
}
String ELFScript::_get_source_code() const {
	if (source_code.is_empty()) {
		return String();
	}
	Array functions_array;
	for (String function : functions) {
		Dictionary function_dictionary;
		function_dictionary["name"] = function;
		function_dictionary["args"] = Array();
		functions_array.push_back(function_dictionary);
	}
	return JSON::stringify(functions_array, "  ");
}
void ELFScript::_set_source_code(const String &p_code) {
}
Error ELFScript::_reload(bool p_keep_state) {
	this->set_file(path);
	return Error::OK;
}
TypedArray<Dictionary> ELFScript::_get_documentation() const {
	return TypedArray<Dictionary>();
}
String ELFScript::_get_class_icon_path() const {
	return String("res://addons/godot_sandbox/Sandbox.svg");
}
bool ELFScript::_has_method(const StringName &p_method) const {
	bool result = functions.find(p_method) != -1;
	if (!result) {
		if (p_method == StringName("_init"))
			result = true;
	}
	if constexpr (VERBOSE_ELFSCRIPT) {
		printf("ELFScript::_has_method: method %s => %d\n", p_method.to_ascii_buffer().ptr(), result);
	}

	return result;
}
bool ELFScript::_has_static_method(const StringName &p_method) const {
	return false;
}
Dictionary ELFScript::_get_method_info(const StringName &p_method) const {
	TypedArray<Dictionary> functions_array;
	for (String function : functions) {
		if (function == p_method) {
			if constexpr (VERBOSE_ELFSCRIPT) {
				printf("ELFScript::_get_method_info: method %s\n", p_method.to_ascii_buffer().ptr());
			}
			Dictionary method;
			method["name"] = function;
			method["args"] = Array();
			method["default_args"] = Array();
			Dictionary type;
			type["name"] = "type";
			type["type"] = Variant::Type::OBJECT;
			type["class_name"] = "Object";
			type["hint"] = PropertyHint::PROPERTY_HINT_NONE;
			type["hint_string"] = String("Return value");
			type["usage"] = PROPERTY_USAGE_DEFAULT;
			method["return"] = type;
			method["flags"] = METHOD_FLAG_VARARG;
			return method;
		}
	}
	return Dictionary();
}
bool ELFScript::_is_tool() const {
	return true;
}
bool ELFScript::_is_valid() const {
	return true;
}
bool ELFScript::_is_abstract() const {
	return false;
}
ScriptLanguage *ELFScript::_get_language() const {
	return get_elf_language();
}
bool ELFScript::_has_script_signal(const StringName &p_signal) const {
	return false;
}
TypedArray<Dictionary> ELFScript::_get_script_signal_list() const {
	return TypedArray<Dictionary>();
}

bool ELFScript::_has_property_default_value(const StringName &p_property) const {
	return false;
}
Variant ELFScript::_get_property_default_value(const StringName &p_property) const {
	return Variant();
}
TypedArray<Dictionary> ELFScript::_get_script_property_list() const {
	TypedArray<Dictionary> properties;
	properties.push_back(
			prop_to_dict(PropertyInfo(Variant::INT, "memory_max", PropertyHint::PROPERTY_HINT_TYPE_STRING, "Maximum memory used by the sandboxed program", PROPERTY_USAGE_DEFAULT)));
	properties.push_back(
			prop_to_dict(PropertyInfo(Variant::INT, "execution_timeout", PropertyHint::PROPERTY_HINT_TYPE_STRING, "Maximum instructions executed by the sandboxed program", PROPERTY_USAGE_DEFAULT)));
	properties.push_back(
			prop_to_dict(PropertyInfo(Variant::INT, "references_max", PropertyHint::PROPERTY_HINT_TYPE_STRING, "Maximum references allowed by the sandboxed program", PROPERTY_USAGE_DEFAULT)));
	properties.push_back(
			prop_to_dict(PropertyInfo(Variant::BOOL, "use_unboxed_arguments", PropertyHint::PROPERTY_HINT_TYPE_STRING, "Use unboxed arguments for Sandbox function calls", PROPERTY_USAGE_DEFAULT)));
	properties.push_back(
			prop_to_dict(PropertyInfo(Variant::BOOL, "use_precise_simulation", PropertyHint::PROPERTY_HINT_TYPE_STRING, "Use precise simulation for VM execution", PROPERTY_USAGE_DEFAULT)));
	properties.push_back(
			prop_to_dict(PropertyInfo(Variant::BOOL, "restrictions", PropertyHint::PROPERTY_HINT_TYPE_STRING, "Enable restrictions for the sandboxed program", PROPERTY_USAGE_DEFAULT)));
	return properties;
}

void ELFScript::_update_exports() {}
TypedArray<Dictionary> ELFScript::_get_script_method_list() const {
	TypedArray<Dictionary> functions_array;
	for (String function : functions) {
		Dictionary method;
		method["name"] = function;
		method["args"] = Array();
		method["default_args"] = Array();
		Dictionary type;
		type["name"] = "type";
		type["type"] = Variant::Type::BOOL;
		type["class_name"] = "class";
		type["hint"] = PropertyHint::PROPERTY_HINT_NONE;
		type["hint_string"] = String();
		type["usage"] = PROPERTY_USAGE_DEFAULT;
		method["return"] = type;
		method["flags"] = METHOD_FLAG_VARARG;
		functions_array.push_back(method);
	}
	return functions_array;
}
int32_t ELFScript::_get_member_line(const StringName &p_member) const {
	PackedStringArray formatted_functions = _get_source_code().split("\n");
	for (int i = 0; i < formatted_functions.size(); i++) {
		if (formatted_functions[i].find(p_member) != -1) {
			return i + 1;
		}
	}
	return 0;
}
Dictionary ELFScript::_get_constants() const {
	return Dictionary();
}
TypedArray<StringName> ELFScript::_get_members() const {
	return TypedArray<StringName>();
}
bool ELFScript::_is_placeholder_fallback_enabled() const {
	return false;
}
Variant ELFScript::_get_rpc_config() const {
	return Variant();
}

const PackedByteArray &ELFScript::get_content() {
	return source_code;
}

String ELFScript::get_elf_programming_language() const {
	return elf_programming_language;
}

void ELFScript::set_file(const String &p_path) {
	path = p_path;
	source_code = FileAccess::get_file_as_bytes(path);
	global_name = "Sandbox_" + path.get_basename().replace("res://", "").replace("/", "_").capitalize().replace(" ", "");
	Sandbox::BinaryInfo info = Sandbox::get_program_info_from_binary(source_code);
	info.functions.sort();
	this->functions = std::move(info.functions);
	this->elf_programming_language = info.language;
	this->elf_api_version = info.version;

	for (ELFScriptInstance *instance : instances) {
		instance->reload(this);
	}
}

String ELFScript::get_dockerized_program_path() const {
	// Get the absolute path without the file name
	String path = get_path().get_base_dir().replace("res://", "") + "/";
	String foldername = Docker::GetFolderName(get_path().get_base_dir());
	// Return the path to the folder with the name of the folder + .elf
	// Eg. res://foldername becomes foldername/foldername.elf
	return path + foldername + String(".elf");
}
