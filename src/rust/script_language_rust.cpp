#include "script_language_rust.h"
#include "script_rust.h"
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/control.hpp>
#include <string>
#include <unordered_set>
static constexpr const char *icon_path = "res://addons/godot_sandbox/RustScript.svg";

static RustScriptLanguage *rust_language;

void RustScriptLanguage::init() {
	rust_language = memnew(RustScriptLanguage);
	Engine::get_singleton()->register_script_language(rust_language);
}

RustScriptLanguage *RustScriptLanguage::get_singleton() {
	return rust_language;
}

String RustScriptLanguage::GODOT_CPP_FUNC (get_name)() const {
	return "RustScript";
}
void RustScriptLanguage::GODOT_CPP_FUNC (init)() {}
String RustScriptLanguage::GODOT_CPP_FUNC (get_type)() const {
	return "RustScript";
}
String RustScriptLanguage::GODOT_CPP_FUNC (get_extension)() const {
	return "rs";
}
void RustScriptLanguage::GODOT_CPP_FUNC (finish)() {}
PackedStringArray RustScriptLanguage::GODOT_CPP_FUNC (get_reserved_words)() const {
	static const PackedStringArray reserved_words{
		"as",
		"use",
		"extern crate",
		"break",
		"const",
		"continue",
		"crate",
		"else",
		"if",
		"if let",
		"enum",
		"extern",
		"false",
		"fn",
		"for",
		"if",
		"impl",
		"in",
		"for",
		"let",
		"loop",
		"match",
		"mod",
		"move",
		"mut",
		"pub",
		"impl",
		"ref",
		"return",
		"Self",
		"self",
		"static",
		"struct",
		"super",
		"trait",
		"true",
		"type",
		"unsafe",
		"use",
		"where",
		"while",
		"abstract",
		"alignof",
		"become",
		"box",
		"do",
		"final",
		"macro",
		"offsetof",
		"override",
		"priv",
		"proc",
		"pure",
		"sizeof",
		"typeof",
		"unsized",
		"virtual",
		"yield"
	};
	return reserved_words;
}
bool RustScriptLanguage::GODOT_CPP_FUNC (is_control_flow_keyword)(const String &p_keyword) const {
	static const std::unordered_set<std::string> control_flow_keywords{
		"if", "else", "switch", "case", "default", "while", "loop", "for", "break", "continue", "return", "goto"
	};
	return control_flow_keywords.find(p_keyword.utf8().get_data()) != control_flow_keywords.end();
}
PackedStringArray RustScriptLanguage::GODOT_CPP_FUNC (get_comment_delimiters)() const {
	PackedStringArray comment_delimiters;
	comment_delimiters.push_back("/* */");
	comment_delimiters.push_back("//");
	return comment_delimiters;
}
PackedStringArray RustScriptLanguage::GODOT_CPP_FUNC (get_doc_comment_delimiters)() const {
	PackedStringArray doc_comment_delimiters;
	doc_comment_delimiters.push_back("///");
	doc_comment_delimiters.push_back("/** */");
	return doc_comment_delimiters;
}
PackedStringArray RustScriptLanguage::GODOT_CPP_FUNC (get_string_delimiters)() const {
	PackedStringArray string_delimiters;
	string_delimiters.push_back("' '");
	string_delimiters.push_back("\" \"");
	return string_delimiters;
}
Ref<Script> RustScriptLanguage::GODOT_CPP_FUNC (make_template)(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	RustScript *script = memnew(RustScript);
	return Ref<Script>(script);
}
TypedArray<Dictionary> RustScriptLanguage::GODOT_CPP_FUNC (get_built_in_templates)(const StringName &p_object) const {
	return TypedArray<Dictionary>();
}
bool RustScriptLanguage::GODOT_CPP_FUNC (is_using_templates)() {
	return false;
}
Dictionary RustScriptLanguage::GODOT_CPP_FUNC (validate)(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	return Dictionary();
}
String RustScriptLanguage::GODOT_CPP_FUNC (validate_path)(const String &p_path) const {
	return String();
}
Object *RustScriptLanguage::GODOT_CPP_FUNC (create_script)() const {
	RustScript *script = memnew(RustScript);
	return script;
}
bool RustScriptLanguage::GODOT_CPP_FUNC (has_named_classes)() const {
	return false;
}
bool RustScriptLanguage::GODOT_CPP_FUNC (supports_builtin_mode)() const {
	return false;
}
bool RustScriptLanguage::GODOT_CPP_FUNC (supports_documentation)() const {
	return false;
}
bool RustScriptLanguage::GODOT_CPP_FUNC (can_inherit_from_file)() const {
	return false;
}
int32_t RustScriptLanguage::GODOT_CPP_FUNC (find_function)(const String &p_function, const String &p_code) const {
	return -1;
}
String RustScriptLanguage::GODOT_CPP_FUNC (make_function)(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	return String();
}
Error RustScriptLanguage::GODOT_CPP_FUNC (open_in_external_editor)(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) {
	return Error::OK;
}
bool RustScriptLanguage::GODOT_CPP_FUNC (overrides_external_editor)() {
	return false;
}
Dictionary RustScriptLanguage::GODOT_CPP_FUNC (complete_code)(const String &p_code, const String &p_path, Object *p_owner) const {
	return Dictionary();
}
Dictionary RustScriptLanguage::GODOT_CPP_FUNC (lookup_code)(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	return Dictionary();
}
String RustScriptLanguage::GODOT_CPP_FUNC (auto_indent_code)(const String &p_code, int32_t p_from_line, int32_t p_to_line) const {
	return String();
}
void RustScriptLanguage::GODOT_CPP_FUNC (add_global_constant)(const StringName &p_name, const Variant &p_value) {}
void RustScriptLanguage::GODOT_CPP_FUNC (add_named_global_constant)(const StringName &p_name, const Variant &p_value) {}
void RustScriptLanguage::GODOT_CPP_FUNC (remove_named_global_constant)(const StringName &p_name) {}
void RustScriptLanguage::GODOT_CPP_FUNC (thread_enter)() {}
void RustScriptLanguage::GODOT_CPP_FUNC (thread_exit)() {}
String RustScriptLanguage::GODOT_CPP_FUNC (debug_get_error)() const {
	return String();
}
int32_t RustScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_count)() const {
	return 0;
}
int32_t RustScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_line)(int32_t p_level) const {
	return 0;
}
String RustScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_function)(int32_t p_level) const {
	return String();
}
Dictionary RustScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_locals)(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
Dictionary RustScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_members)(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
void *RustScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_instance)(int32_t p_level) {
	return nullptr;
}
Dictionary RustScriptLanguage::GODOT_CPP_FUNC (debug_get_globals)(int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
String RustScriptLanguage::GODOT_CPP_FUNC (debug_parse_stack_level_expression)(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	return String();
}
TypedArray<Dictionary> RustScriptLanguage::GODOT_CPP_FUNC (debug_get_current_stack_info)() {
	return TypedArray<Dictionary>();
}
void RustScriptLanguage::GODOT_CPP_FUNC (reload_all_scripts)() {}
void RustScriptLanguage::GODOT_CPP_FUNC (reload_tool_script)(const Ref<Script> &p_script, bool p_soft_reload) {}
PackedStringArray RustScriptLanguage::GODOT_CPP_FUNC (get_recognized_extensions)() const {
	PackedStringArray array;
	array.push_back("rs");
	return array;
}
TypedArray<Dictionary> RustScriptLanguage::GODOT_CPP_FUNC (get_public_functions)() const {
	return TypedArray<Dictionary>();
}
Dictionary RustScriptLanguage::GODOT_CPP_FUNC (get_public_constants)() const {
	return Dictionary();
}
TypedArray<Dictionary> RustScriptLanguage::GODOT_CPP_FUNC (get_public_annotations)() const {
	return TypedArray<Dictionary>();
}
void RustScriptLanguage::GODOT_CPP_FUNC (profiling_start)() {}
void RustScriptLanguage::GODOT_CPP_FUNC (profiling_stop)() {}
int32_t RustScriptLanguage::GODOT_CPP_FUNC (profiling_get_accumulated_data)(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
int32_t RustScriptLanguage::GODOT_CPP_FUNC (profiling_get_frame_data)(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
void RustScriptLanguage::GODOT_CPP_FUNC (frame)() {
	static bool icon_registered = false;
	if (!icon_registered && Engine::get_singleton()->is_editor_hint()) {
		icon_registered = true;
		// Manually register language icon
		load_icon();
		// Register theme callback
		EditorInterface::get_singleton()->get_base_control()->connect("theme_changed", callable_mp(this, &RustScriptLanguage::load_icon));
	}
}
void RustScriptLanguage::load_icon()
{
	static bool reenter = false;
	if (reenter)
		return;
	reenter = true;
	if (Engine::get_singleton()->is_editor_hint() && FileAccess::file_exists(icon_path)) {
		Ref<Theme> editor_theme = EditorInterface::get_singleton()->get_editor_theme();
		if (editor_theme.is_valid() && !editor_theme->has_icon("RustScript", "EditorIcons"))
		{
			ResourceLoader *resource_loader = ResourceLoader::get_singleton();
			Ref<Texture2D> tex = resource_loader->load(icon_path);
			editor_theme->set_icon("RustScript", "EditorIcons", tex);
		}
	}
	reenter = false;
}
bool RustScriptLanguage::GODOT_CPP_FUNC (handles_global_class_type)(const String &p_type) const {
	return p_type == "RustScript";
}
Dictionary RustScriptLanguage::GODOT_CPP_FUNC (get_global_class_name)(const String &p_path) const {
	return Dictionary();
}
