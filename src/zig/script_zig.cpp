#include "script_zig.h"

#include "script_language_zig.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

bool ZigScript::GODOT_CPP_FUNC (editor_can_reload_from_file)() {
	return true;
}
void ZigScript::GODOT_CPP_FUNC (placeholder_erased)(void *p_placeholder) {}
bool ZigScript::GODOT_CPP_FUNC (can_instantiate)() const {
	return false;
}
Ref<Script> ZigScript::GODOT_CPP_FUNC (get_base_script)() const {
	return Ref<Script>();
}
StringName ZigScript::GODOT_CPP_FUNC (get_global_name)() const {
	return StringName();
}
bool ZigScript::GODOT_CPP_FUNC (inherits_script)(const Ref<Script> &p_script) const {
	return false;
}
StringName ZigScript::GODOT_CPP_FUNC (get_instance_base_type)() const {
	return StringName();
}
void *ZigScript::GODOT_CPP_FUNC (instance_create)(Object *p_for_object) const {
	return nullptr;
}
void *ZigScript::GODOT_CPP_FUNC (placeholder_instance_create)(Object *p_for_object) const {
	return nullptr;
}
bool ZigScript::GODOT_CPP_FUNC (instance_has)(Object *p_object) const {
	return false;
}
bool ZigScript::GODOT_CPP_FUNC (has_source_code)() const {
	return true;
}
String ZigScript::GODOT_CPP_FUNC (get_source_code)() const {
	return source_code;
}
void ZigScript::GODOT_CPP_FUNC (set_source_code)(const String &p_code) {
	source_code = p_code;
}
Error ZigScript::GODOT_CPP_FUNC (reload)(bool p_keep_state) {
	return Error::OK;
}
TypedArray<Dictionary> ZigScript::GODOT_CPP_FUNC (get_documentation)() const {
	return TypedArray<Dictionary>();
}
String ZigScript::GODOT_CPP_FUNC (get_class_icon_path)() const {
	return String("res://addons/godot_sandbox/ZIGScript.svg");
}
bool ZigScript::GODOT_CPP_FUNC (has_method)(const StringName &p_method) const {
	return false;
}
bool ZigScript::GODOT_CPP_FUNC (has_static_method)(const StringName &p_method) const {
	return false;
}
Dictionary ZigScript::GODOT_CPP_FUNC (get_method_info)(const StringName &p_method) const {
	return Dictionary();
}
bool ZigScript::GODOT_CPP_FUNC (is_tool)() const {
	return true;
}
bool ZigScript::GODOT_CPP_FUNC (is_valid)() const {
	return true;
}
bool ZigScript::GODOT_CPP_FUNC (is_abstract)() const {
	return true;
}
ScriptLanguage *ZigScript::GODOT_CPP_FUNC (get_language)() const {
	return ZigScriptLanguage::get_singleton();
}
bool ZigScript::GODOT_CPP_FUNC (has_script_signal)(const StringName &p_signal) const {
	return false;
}
TypedArray<Dictionary> ZigScript::GODOT_CPP_FUNC (get_script_signal_list)() const {
	return TypedArray<Dictionary>();
}
bool ZigScript::GODOT_CPP_FUNC (has_property_default_value)(const StringName &p_property) const {
	return false;
}
Variant ZigScript::GODOT_CPP_FUNC (get_property_default_value)(const StringName &p_property) const {
	return Variant();
}
void ZigScript::GODOT_CPP_FUNC (update_exports)() {}
TypedArray<Dictionary> ZigScript::GODOT_CPP_FUNC (get_script_method_list)() const {
	return TypedArray<Dictionary>();
}
TypedArray<Dictionary> ZigScript::GODOT_CPP_FUNC (get_script_property_list)() const {
	return TypedArray<Dictionary>();
}
int32_t ZigScript::GODOT_CPP_FUNC (get_member_line)(const StringName &p_member) const {
	return 0;
}
Dictionary ZigScript::GODOT_CPP_FUNC (get_constants)() const {
	return Dictionary();
}
TypedArray<StringName> ZigScript::GODOT_CPP_FUNC (get_members)() const {
	return TypedArray<StringName>();
}
bool ZigScript::GODOT_CPP_FUNC (is_placeholder_fallback_enabled)() const {
	return false;
}
Variant ZigScript::GODOT_CPP_FUNC (get_rpc_config)() const {
	return Variant();
}

ZigScript::ZigScript() {
	source_code = R"C0D3(
// TODO: Implement me.
)C0D3";
}
