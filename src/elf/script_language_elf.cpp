#include "script_language_elf.h"
#include "script_elf.h"
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/control.hpp>
static constexpr const char *icon_path = "res://addons/godot_sandbox/Sandbox.svg";

String ELFScriptLanguage::GODOT_CPP_FUNC (get_name)() const {
	return "ELF";
}
void ELFScriptLanguage::GODOT_CPP_FUNC (init)() {}
String ELFScriptLanguage::GODOT_CPP_FUNC (get_type)() const {
	return "ELFScript";
}
String ELFScriptLanguage::GODOT_CPP_FUNC (get_extension)() const {
	return "elf";
}
void ELFScriptLanguage::GODOT_CPP_FUNC (finish)() {}
PackedStringArray ELFScriptLanguage::GODOT_CPP_FUNC (get_reserved_words)() const {
	return PackedStringArray();
}
bool ELFScriptLanguage::GODOT_CPP_FUNC (is_control_flow_keyword)(const String &p_keyword) const {
	return false;
}
PackedStringArray ELFScriptLanguage::GODOT_CPP_FUNC (get_comment_delimiters)() const {
	PackedStringArray comment_delimiters;
	comment_delimiters.push_back("/* */");
	comment_delimiters.push_back("//");
	return comment_delimiters;
}
PackedStringArray ELFScriptLanguage::GODOT_CPP_FUNC (get_doc_comment_delimiters)() const {
	PackedStringArray doc_comment_delimiters;
	doc_comment_delimiters.push_back("///");
	doc_comment_delimiters.push_back("/** */");
	return doc_comment_delimiters;
}
PackedStringArray ELFScriptLanguage::GODOT_CPP_FUNC (get_string_delimiters)() const {
	PackedStringArray string_delimiters;
	string_delimiters.push_back("' '");
	string_delimiters.push_back("\" \"");
	return string_delimiters;
}
Ref<Script> ELFScriptLanguage::GODOT_CPP_FUNC (make_template)(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	ELFScript *elf_script = memnew(ELFScript);
	return Ref<Script>(elf_script);
}
TypedArray<Dictionary> ELFScriptLanguage::GODOT_CPP_FUNC (get_built_in_templates)(const StringName &p_object) const {
	return TypedArray<Dictionary>();
}
bool ELFScriptLanguage::GODOT_CPP_FUNC (is_using_templates)() {
	return false;
}
Dictionary ELFScriptLanguage::GODOT_CPP_FUNC (validate)(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	return Dictionary();
}
String ELFScriptLanguage::GODOT_CPP_FUNC (validate_path)(const String &p_path) const {
	return String();
}
Object *ELFScriptLanguage::GODOT_CPP_FUNC (create_script)() const {
	ELFScript *script = memnew(ELFScript);
	return script;
}
bool ELFScriptLanguage::GODOT_CPP_FUNC (has_named_classes)() const {
	return true;
}
bool ELFScriptLanguage::GODOT_CPP_FUNC (supports_builtin_mode)() const {
	return true;
}
bool ELFScriptLanguage::GODOT_CPP_FUNC (supports_documentation)() const {
	return false;
}
bool ELFScriptLanguage::GODOT_CPP_FUNC (can_inherit_from_file)() const {
	return false;
}
int32_t ELFScriptLanguage::GODOT_CPP_FUNC (find_function)(const String &p_function, const String &p_code) const {
	return -1;
}
String ELFScriptLanguage::GODOT_CPP_FUNC (make_function)(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	return String();
}
Error ELFScriptLanguage::GODOT_CPP_FUNC (open_in_external_editor)(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) {
	return Error::OK;
}
bool ELFScriptLanguage::GODOT_CPP_FUNC (overrides_external_editor)() {
	return false;
}
Dictionary ELFScriptLanguage::GODOT_CPP_FUNC (complete_code)(const String &p_code, const String &p_path, Object *p_owner) const {
	return Dictionary();
}
Dictionary ELFScriptLanguage::GODOT_CPP_FUNC (lookup_code)(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	return Dictionary();
}
String ELFScriptLanguage::GODOT_CPP_FUNC (auto_indent_code)(const String &p_code, int32_t p_from_line, int32_t p_to_line) const {
	return String();
}
void ELFScriptLanguage::GODOT_CPP_FUNC (add_global_constant)(const StringName &p_name, const Variant &p_value) {}
void ELFScriptLanguage::GODOT_CPP_FUNC (add_named_global_constant)(const StringName &p_name, const Variant &p_value) {}
void ELFScriptLanguage::GODOT_CPP_FUNC (remove_named_global_constant)(const StringName &p_name) {}
void ELFScriptLanguage::GODOT_CPP_FUNC (thread_enter)() {}
void ELFScriptLanguage::GODOT_CPP_FUNC (thread_exit)() {}
String ELFScriptLanguage::GODOT_CPP_FUNC (debug_get_error)() const {
	return String();
}
int32_t ELFScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_count)() const {
	return 0;
}
int32_t ELFScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_line)(int32_t p_level) const {
	return 0;
}
String ELFScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_function)(int32_t p_level) const {
	return String();
}
Dictionary ELFScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_locals)(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
Dictionary ELFScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_members)(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
void *ELFScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_instance)(int32_t p_level) {
	return nullptr;
}
Dictionary ELFScriptLanguage::GODOT_CPP_FUNC (debug_get_globals)(int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
String ELFScriptLanguage::GODOT_CPP_FUNC (debug_parse_stack_level_expression)(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	return String();
}
TypedArray<Dictionary> ELFScriptLanguage::GODOT_CPP_FUNC (debug_get_current_stack_info)() {
	return TypedArray<Dictionary>();
}
void ELFScriptLanguage::GODOT_CPP_FUNC (reload_all_scripts)() {}
void ELFScriptLanguage::GODOT_CPP_FUNC (reload_tool_script)(const Ref<Script> &p_script, bool p_soft_reload) {}
PackedStringArray ELFScriptLanguage::GODOT_CPP_FUNC (get_recognized_extensions)() const {
	PackedStringArray array;
	array.push_back("elf");
	return array;
}
TypedArray<Dictionary> ELFScriptLanguage::GODOT_CPP_FUNC (get_public_functions)() const {
	return TypedArray<Dictionary>();
}
Dictionary ELFScriptLanguage::GODOT_CPP_FUNC (get_public_constants)() const {
	return Dictionary();
}
TypedArray<Dictionary> ELFScriptLanguage::GODOT_CPP_FUNC (get_public_annotations)() const {
	return TypedArray<Dictionary>();
}
void ELFScriptLanguage::GODOT_CPP_FUNC (profiling_start)() {}
void ELFScriptLanguage::GODOT_CPP_FUNC (profiling_stop)() {}
int32_t ELFScriptLanguage::GODOT_CPP_FUNC (profiling_get_accumulated_data)(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
int32_t ELFScriptLanguage::GODOT_CPP_FUNC (profiling_get_frame_data)(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
void ELFScriptLanguage::GODOT_CPP_FUNC (frame)() {
	static bool icon_registered = false;
	if (!icon_registered && Engine::get_singleton()->is_editor_hint()) {
		icon_registered = true;
		// Manually register ELFScript icon
		load_icon();
		// Register theme callback
		EditorInterface::get_singleton()->get_base_control()->connect("theme_changed", callable_mp(this, &ELFScriptLanguage::load_icon));
	}
}
void ELFScriptLanguage::load_icon()
{
	static bool reenter = false;
	if (reenter)
		return;
	reenter = true;
	if (Engine::get_singleton()->is_editor_hint() && FileAccess::file_exists(icon_path)) {
		Ref<Theme> editor_theme = EditorInterface::get_singleton()->get_editor_theme();
		if (editor_theme.is_valid() && !editor_theme->has_icon("ELFScript", "EditorIcons"))
		{
			ResourceLoader *resource_loader = ResourceLoader::get_singleton();
			Ref<Texture2D> tex = resource_loader->load(icon_path);
			editor_theme->set_icon("ELFScript", "EditorIcons", tex);
		}
	}
	reenter = false;
}
bool ELFScriptLanguage::GODOT_CPP_FUNC (handles_global_class_type)(const String &p_type) const {
	return p_type == "ELFScript" || p_type == "Sandbox";
}
Dictionary ELFScriptLanguage::GODOT_CPP_FUNC (get_global_class_name)(const String &p_path) const {
	Ref<Resource> resource = ResourceLoader::get_singleton()->load(p_path);
	Ref<ELFScript> elf_model = Object::cast_to<ELFScript>(resource.ptr());
	Dictionary dict;
	if (elf_model.is_valid()) {
		dict["name"] = elf_model->_get_global_name();
		dict["base_type"] = "Sandbox";
		dict["icon_path"] = String("res://addons/godot_sandbox/Sandbox.svg");
	}
	return dict;
}
