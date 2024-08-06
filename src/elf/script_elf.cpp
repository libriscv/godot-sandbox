#include "script_elf.h"

#include "../register_types.h"
#include "../sandbox.hpp"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

class ScriptInstance {
public:
	Object *object;
	ScriptInstance() {}
	~ScriptInstance() {}
};

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
	return StringName("Node");
}
void *ELFScript::_instance_create(Object *p_for_object) const {
	return nullptr;
}
void *ELFScript::_placeholder_instance_create(Object *p_for_object) const {
	return nullptr;
}
bool ELFScript::_instance_has(Object *p_object) const {
	return true;
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
	set_file(path);
	return Error::OK;
}
TypedArray<Dictionary> ELFScript::_get_documentation() const {
	return TypedArray<Dictionary>();
}
String ELFScript::_get_class_icon_path() const {
	return String("res://addons/godot_sandbox/ELFScript.svg");
}
bool ELFScript::_has_method(const StringName &p_method) const {
	return true;
}
bool ELFScript::_has_static_method(const StringName &p_method) const {
	return false;
}
Variant ELFScript::_get_script_method_argument_count(const StringName &p_method) const {
	return Variant();
}
Dictionary ELFScript::_get_method_info(const StringName &p_method) const {
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
		method["flags"] = METHOD_FLAGS_DEFAULT;
		functions_array.push_back(method);
	}
	return functions_array;
}
TypedArray<Dictionary> ELFScript::_get_script_property_list() const {
	return TypedArray<Dictionary>();
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

PackedByteArray ELFScript::get_content() {
	return source_code;
}

void ELFScript::set_file(const String &p_path) {
	path = p_path;
	source_code = FileAccess::get_file_as_bytes(path);
	global_name = "ELF_" + path.get_basename().replace("res://", "").replace("/", "_");
	PackedStringArray functions_array = Sandbox::get_functions_from_binary(source_code);
	functions_array.sort();
	functions = functions_array;
}
