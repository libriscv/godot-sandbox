#include "script_cpp.h"

#include "../register_types.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

bool CPPScript::_editor_can_reload_from_file() {
	return true;
}
void CPPScript::_placeholder_erased(void *p_placeholder) {}
bool CPPScript::_can_instantiate() const {
	return false;
}
Ref<Script> CPPScript::_get_base_script() const {
	return Ref<Script>();
}
StringName CPPScript::_get_global_name() const {
	return StringName();
}
bool CPPScript::_inherits_script(const Ref<Script> &p_script) const {
	return false;
}
StringName CPPScript::_get_instance_base_type() const {
	return StringName();
}
void *CPPScript::_instance_create(Object *p_for_object) const {
	return nullptr;
}
void *CPPScript::_placeholder_instance_create(Object *p_for_object) const {
	return nullptr;
}
bool CPPScript::_instance_has(Object *p_object) const {
	return false;
}
bool CPPScript::_has_source_code() const {
	return true;
}
String CPPScript::_get_source_code() const {
	return source_code;
}
void CPPScript::_set_source_code(const String &p_code) {
	source_code = p_code;
}
Error CPPScript::_reload(bool p_keep_state) {
	return Error::OK;
}
TypedArray<Dictionary> CPPScript::_get_documentation() const {
	return TypedArray<Dictionary>();
}
String CPPScript::_get_class_icon_path() const {
	return String("res://addons/godot_sandbox/CPPScript.svg");
}
bool CPPScript::_has_method(const StringName &p_method) const {
	return false;
}
bool CPPScript::_has_static_method(const StringName &p_method) const {
	return false;
}
Dictionary CPPScript::_get_method_info(const StringName &p_method) const {
	return Dictionary();
}
bool CPPScript::_is_tool() const {
	return true;
}
bool CPPScript::_is_valid() const {
	return true;
}
bool CPPScript::_is_abstract() const {
	return true;
}
ScriptLanguage *CPPScript::_get_language() const {
	return get_cpp_language();
}
bool CPPScript::_has_script_signal(const StringName &p_signal) const {
	return false;
}
TypedArray<Dictionary> CPPScript::_get_script_signal_list() const {
	return TypedArray<Dictionary>();
}
bool CPPScript::_has_property_default_value(const StringName &p_property) const {
	return false;
}
Variant CPPScript::_get_property_default_value(const StringName &p_property) const {
	return Variant();
}
void CPPScript::_update_exports() {}
TypedArray<Dictionary> CPPScript::_get_script_method_list() const {
	return TypedArray<Dictionary>();
}
TypedArray<Dictionary> CPPScript::_get_script_property_list() const {
	return TypedArray<Dictionary>();
}
int32_t CPPScript::_get_member_line(const StringName &p_member) const {
	return 0;
}
Dictionary CPPScript::_get_constants() const {
	return Dictionary();
}
TypedArray<StringName> CPPScript::_get_members() const {
	return TypedArray<StringName>();
}
bool CPPScript::_is_placeholder_fallback_enabled() const {
	return false;
}
Variant CPPScript::_get_rpc_config() const {
	return Variant();
}

CPPScript::CPPScript() {
	source_code = R"C0D3(#include "api.hpp"

int main() {
	UtilityFunctions::print("Hello, ", 123, " world!\n");

	// do shit here...

	halt(); // Prevent stdout,stderr closing etc.
}

extern "C" void public_function(Variant arg) {
}
)C0D3";
}
