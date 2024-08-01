#include "script_elf.h"

#include <godot_cpp/classes/file_access.hpp>

PackedByteArray ELFScript::get_content() {
	String p_path = get_file();
	return FileAccess::get_file_as_bytes(p_path);
}

bool ELFScript::_editor_can_reload_from_file() {
	return false;
}
void ELFScript::_placeholder_erased(void *p_placeholder) {}
bool ELFScript::_can_instantiate() const {
	return false;
}
Ref<Script> ELFScript::_get_base_script() const {
	return Ref<Script>();
}
StringName ELFScript::_get_global_name() const {
	return StringName();
}
bool ELFScript::_inherits_script(const Ref<Script> &p_script) const {
	return false;
}
StringName ELFScript::_get_instance_base_type() const {
	return StringName();
}
void *ELFScript::_instance_create(Object *p_for_object) const {
	return nullptr;
}
void *ELFScript::_placeholder_instance_create(Object *p_for_object) const {
	return nullptr;
}
bool ELFScript::_instance_has(Object *p_object) const {
	return false;
}
bool ELFScript::_has_source_code() const {
	return false;
}
String ELFScript::_get_source_code() const {
	return String();
}
void ELFScript::_set_source_code(const String &p_code) {}
Error ELFScript::_reload(bool p_keep_state) {
	return Error::OK;
}
TypedArray<Dictionary> ELFScript::_get_documentation() const {
	return TypedArray<Dictionary>();
}
String ELFScript::_get_class_icon_path() const {
	return String();
}
bool ELFScript::_has_method(const StringName &p_method) const {
	return false;
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
	return false;
}
bool ELFScript::_is_valid() const {
	return false;
}
bool ELFScript::_is_abstract() const {
	return false;
}
//ScriptLanguage *ELFScript::_get_language() const {
	//return (ScriptLanguage *) nullptr;
//}
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
	return TypedArray<Dictionary>();
}
TypedArray<Dictionary> ELFScript::_get_script_property_list() const {
	return TypedArray<Dictionary>();
}
int32_t ELFScript::_get_member_line(const StringName &p_member) const {
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
	