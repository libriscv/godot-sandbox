#include "script_rust.h"

#include "script_language_rust.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

bool RustScript:: GODOT_CPP_FUNC (editor_can_reload_from_file)() {
	return true;
}
void RustScript:: GODOT_CPP_FUNC (placeholder_erased)(void *p_placeholder) {}
bool RustScript:: GODOT_CPP_FUNC (can_instantiate)() const {
	return false;
}
Ref<Script> RustScript:: GODOT_CPP_FUNC (get_base_script)() const {
	return Ref<Script>();
}
StringName RustScript:: GODOT_CPP_FUNC (get_global_name)() const {
	return StringName();
}
bool RustScript:: GODOT_CPP_FUNC (inherits_script)(const Ref<Script> &p_script) const {
	return false;
}
StringName RustScript:: GODOT_CPP_FUNC (get_instance_base_type)() const {
	return StringName();
}
void *RustScript:: GODOT_CPP_FUNC (instance_create)(Object *p_for_object) const {
	return nullptr;
}
void *RustScript:: GODOT_CPP_FUNC (placeholder_instance_create)(Object *p_for_object) const {
	return nullptr;
}
bool RustScript:: GODOT_CPP_FUNC (instance_has)(Object *p_object) const {
	return false;
}
bool RustScript:: GODOT_CPP_FUNC (has_source_code)() const {
	return true;
}
String RustScript:: GODOT_CPP_FUNC (get_source_code)() const {
	return source_code;
}
void RustScript:: GODOT_CPP_FUNC (set_source_code)(const String &p_code) {
	source_code = p_code;
}
Error RustScript:: GODOT_CPP_FUNC (reload)(bool p_keep_state) {
	return Error::OK;
}
TypedArray<Dictionary> RustScript:: GODOT_CPP_FUNC (get_documentation)() const {
	return TypedArray<Dictionary>();
}
String RustScript:: GODOT_CPP_FUNC (get_class_icon_path)() const {
	return String("res://addons/godot_sandbox/RustScript.svg");
}
bool RustScript:: GODOT_CPP_FUNC (has_method)(const StringName &p_method) const {
	return false;
}
bool RustScript:: GODOT_CPP_FUNC (has_static_method)(const StringName &p_method) const {
	return false;
}
Dictionary RustScript:: GODOT_CPP_FUNC (get_method_info)(const StringName &p_method) const {
	return Dictionary();
}
bool RustScript:: GODOT_CPP_FUNC (is_tool)() const {
	return true;
}
bool RustScript:: GODOT_CPP_FUNC (is_valid)() const {
	return true;
}
bool RustScript:: GODOT_CPP_FUNC (is_abstract)() const {
	return true;
}
ScriptLanguage *RustScript:: GODOT_CPP_FUNC (get_language)() const {
	return RustScriptLanguage::get_singleton();
}
bool RustScript:: GODOT_CPP_FUNC (has_script_signal)(const StringName &p_signal) const {
	return false;
}
TypedArray<Dictionary> RustScript:: GODOT_CPP_FUNC (get_script_signal_list)() const {
	return TypedArray<Dictionary>();
}
bool RustScript:: GODOT_CPP_FUNC (has_property_default_value)(const StringName &p_property) const {
	return false;
}
Variant RustScript:: GODOT_CPP_FUNC (get_property_default_value)(const StringName &p_property) const {
	return Variant();
}
void RustScript:: GODOT_CPP_FUNC (update_exports)() {}
TypedArray<Dictionary> RustScript:: GODOT_CPP_FUNC (get_script_method_list)() const {
	return TypedArray<Dictionary>();
}
TypedArray<Dictionary> RustScript:: GODOT_CPP_FUNC (get_script_property_list)() const {
	return TypedArray<Dictionary>();
}
int32_t RustScript:: GODOT_CPP_FUNC (get_member_line)(const StringName &p_member) const {
	return 0;
}
Dictionary RustScript:: GODOT_CPP_FUNC (get_constants)() const {
	return Dictionary();
}
TypedArray<StringName> RustScript:: GODOT_CPP_FUNC (get_members)() const {
	return TypedArray<StringName>();
}
bool RustScript:: GODOT_CPP_FUNC (is_placeholder_fallback_enabled)() const {
	return false;
}
Variant RustScript:: GODOT_CPP_FUNC (get_rpc_config)() const {
	return Variant();
}

RustScript::RustScript() {
	source_code = R"C0D3(mod godot;
use variant::*;

pub fn main() {
}

#[no_mangle]
pub fn public_function() -> Variant {
	let v1 = Variant::new_integer(42);
	let v2 = Variant::new_float(3.14);
	let v3 = Variant::new_string("Hello from Rust!");
	print(&[v1, v2, v3]);

	return Variant::new_string("Rust in Godot");
}
)C0D3";
}
