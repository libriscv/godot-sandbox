#include "script_cpp.h"

#include "script_language_cpp.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

bool CPPScript::GODOT_CPP_FUNC (editor_can_reload_from_file)() {
	return true;
}
void CPPScript::GODOT_CPP_FUNC (placeholder_erased)(void *p_placeholder) {}
bool CPPScript::GODOT_CPP_FUNC (can_instantiate)() const {
	return false;
}
Ref<Script> CPPScript::GODOT_CPP_FUNC (get_base_script)() const {
	return Ref<Script>();
}
StringName CPPScript::GODOT_CPP_FUNC (get_global_name)() const {
	return StringName();
}
bool CPPScript::GODOT_CPP_FUNC (inherits_script)(const Ref<Script> &p_script) const {
	return false;
}
StringName CPPScript::GODOT_CPP_FUNC (get_instance_base_type)() const {
	return StringName();
}
void *CPPScript::GODOT_CPP_FUNC (instance_create)(Object *p_for_object) const {
	return nullptr;
}
void *CPPScript::GODOT_CPP_FUNC (placeholder_instance_create)(Object *p_for_object) const {
	return nullptr;
}
bool CPPScript::GODOT_CPP_FUNC (instance_has)(Object *p_object) const {
	return false;
}
bool CPPScript::GODOT_CPP_FUNC (has_source_code)() const {
	return true;
}
String CPPScript::GODOT_CPP_FUNC (get_source_code)() const {
	return source_code;
}
void CPPScript::GODOT_CPP_FUNC (set_source_code)(const String &p_code) {
	source_code = p_code;
}
Error CPPScript::GODOT_CPP_FUNC (reload)(bool p_keep_state) {
	return Error::OK;
}
TypedArray<Dictionary> CPPScript::GODOT_CPP_FUNC (get_documentation)() const {
	return TypedArray<Dictionary>();
}
String CPPScript::GODOT_CPP_FUNC (get_class_icon_path)() const {
	return String("res://addons/godot_sandbox/CPPScript.svg");
}
bool CPPScript::GODOT_CPP_FUNC (has_method)(const StringName &p_method) const {
	return false;
}
bool CPPScript::GODOT_CPP_FUNC (has_static_method)(const StringName &p_method) const {
	return false;
}
Dictionary CPPScript::GODOT_CPP_FUNC (get_method_info)(const StringName &p_method) const {
	return Dictionary();
}
bool CPPScript::GODOT_CPP_FUNC (is_tool)() const {
	return true;
}
bool CPPScript::GODOT_CPP_FUNC (is_valid)() const {
	return true;
}
bool CPPScript::GODOT_CPP_FUNC (is_abstract)() const {
	return true;
}
ScriptLanguage *CPPScript::GODOT_CPP_FUNC (get_language)() const {
	return CPPScriptLanguage::get_singleton();
}
bool CPPScript::GODOT_CPP_FUNC (has_script_signal)(const StringName &p_signal) const {
	return false;
}
TypedArray<Dictionary> CPPScript::GODOT_CPP_FUNC (get_script_signal_list)() const {
	return TypedArray<Dictionary>();
}
bool CPPScript::GODOT_CPP_FUNC (has_property_default_value)(const StringName &p_property) const {
	return false;
}
Variant CPPScript::GODOT_CPP_FUNC (get_property_default_value)(const StringName &p_property) const {
	return Variant();
}
void CPPScript::GODOT_CPP_FUNC (update_exports)() {}
TypedArray<Dictionary> CPPScript::GODOT_CPP_FUNC (get_script_method_list)() const {
	return TypedArray<Dictionary>();
}
TypedArray<Dictionary> CPPScript::GODOT_CPP_FUNC (get_script_property_list)() const {
	return TypedArray<Dictionary>();
}
int32_t CPPScript::GODOT_CPP_FUNC (get_member_line)(const StringName &p_member) const {
	return 0;
}
Dictionary CPPScript::GODOT_CPP_FUNC (get_constants)() const {
	return Dictionary();
}
TypedArray<StringName> CPPScript::GODOT_CPP_FUNC (get_members)() const {
	return TypedArray<StringName>();
}
bool CPPScript::GODOT_CPP_FUNC (is_placeholder_fallback_enabled)() const {
	return false;
}
Variant CPPScript::GODOT_CPP_FUNC (get_rpc_config)() const {
	return Variant();
}

CPPScript::CPPScript() {
	source_code = R"C0D3(#include "api.hpp"

extern "C" Variant public_function(String arg) {
    print("Arguments: ", arg);
    return "Hello from the other side";
}
)C0D3";
}
