#include "script_language_safegdscript.h"
#include "../script_language_common.h"
#include "script_safegdscript.h"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <string>
#include <unordered_set>
static constexpr const char *icon_path = "res://addons/godot_sandbox/SafeGDScript.svg";

static SafeGDScriptLanguage *safegdscript_language;

void SafeGDScriptLanguage::init() {
	safegdscript_language = memnew(SafeGDScriptLanguage);
	Engine::get_singleton()->register_script_language(safegdscript_language);
}
void SafeGDScriptLanguage::deinit() {
	if (safegdscript_language) {
		Engine::get_singleton()->unregister_script_language(safegdscript_language);
		memdelete(safegdscript_language);
		safegdscript_language = nullptr;
	}
}

SafeGDScriptLanguage *SafeGDScriptLanguage::get_singleton() {
	return safegdscript_language;
}

String SafeGDScriptLanguage::_get_name() const {
	return "SafeGD";
}
void SafeGDScriptLanguage::_init() {}
String SafeGDScriptLanguage::_get_type() const {
	return "SafeGDScript";
}
String SafeGDScriptLanguage::_get_extension() const {
	return "sgd";
}
void SafeGDScriptLanguage::_finish() {}
PackedStringArray SafeGDScriptLanguage::_get_reserved_words() const {
	static const PackedStringArray reserved_words{
		"if",
		"elif",
		"else",
		"for",
		"while",
		"match",
		"break",
		"continue",
		"return",
		"pass",
		"func",
		"class",
		"class_name",
		"extends",
		"is",
		"in",
		"as",
		"and",
		"or",
		"not",
		"var",
		"const",
		"static",
		"enum",
		"signal",
		"super",
		"self",
		"true",
		"false",
		"null",
		"void",
		"bool",
		"int",
		"float",
		"String",
		"Vector2",
		"Vector2i",
		"Vector3",
		"Vector3i",
		"Vector4",
		"Vector4i",
		"Color",
		"Array",
		"Dictionary",
		"PackedByteArray",
		"PackedInt32Array",
		"PackedInt64Array",
		"PackedFloat32Array",
		"PackedFloat64Array",
		"PackedStringArray",
		"PackedVector2Array",
		"PackedVector3Array",
		"PackedColorArray",
		"Node",
		"RefCounted",
		"Resource",
		"void",
		"assert",
		"await",
		"yield",
	};
	return reserved_words;
}
bool SafeGDScriptLanguage::_is_control_flow_keyword(const String &p_keyword) const {
	static const std::unordered_set<std::string> control_flow_keywords{
		"if", "elif", "else", "for", "while", "match", "break", "continue", "return", "pass", "assert"
	};
	return control_flow_keywords.find(p_keyword.utf8().get_data()) != control_flow_keywords.end();
}
PackedStringArray SafeGDScriptLanguage::_get_comment_delimiters() const {
	PackedStringArray comment_delimiters;
	comment_delimiters.push_back("#");
	return comment_delimiters;
}
PackedStringArray SafeGDScriptLanguage::_get_doc_comment_delimiters() const {
	PackedStringArray doc_comment_delimiters;
	doc_comment_delimiters.push_back("##");
	return doc_comment_delimiters;
}
PackedStringArray SafeGDScriptLanguage::_get_string_delimiters() const {
	PackedStringArray string_delimiters;
	string_delimiters.push_back("\" \"");
	string_delimiters.push_back("' '");
	string_delimiters.push_back("\"\"\" \"\"\"");
	return string_delimiters;
}
Ref<Script> SafeGDScriptLanguage::_make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	SafeGDScript *script = memnew(SafeGDScript);
	return Ref<Script>(script);
}
TypedArray<Dictionary> SafeGDScriptLanguage::_get_built_in_templates(const StringName &p_object) const {
	return TypedArray<Dictionary>();
}
bool SafeGDScriptLanguage::_is_using_templates() {
	return false;
}
Dictionary SafeGDScriptLanguage::_validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	return Dictionary();
}
String SafeGDScriptLanguage::_validate_path(const String &p_path) const {
	return String();
}
Object *SafeGDScriptLanguage::_create_script() const {
	SafeGDScript *script = memnew(SafeGDScript);
	return script;
}
bool SafeGDScriptLanguage::_has_named_classes() const {
	return false;
}
bool SafeGDScriptLanguage::_supports_builtin_mode() const {
	return true;
}
bool SafeGDScriptLanguage::_supports_documentation() const {
	return false;
}
bool SafeGDScriptLanguage::_can_inherit_from_file() const {
	return false;
}
bool SafeGDScriptLanguage::_can_make_function() const {
	return false;
}
int32_t SafeGDScriptLanguage::_find_function(const String &p_function, const String &p_code) const {
	return -1;
}
String SafeGDScriptLanguage::_make_function(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	return String();
}
Error SafeGDScriptLanguage::_open_in_external_editor(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) {
	return Error::OK;
}
bool SafeGDScriptLanguage::_overrides_external_editor() {
	return false;
}
Dictionary SafeGDScriptLanguage::_complete_code(const String &p_code, const String &p_path, Object *p_owner) const {
	return Dictionary();
}
Dictionary SafeGDScriptLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	return Dictionary();
}
String SafeGDScriptLanguage::_auto_indent_code(const String &p_code, int32_t p_from_line, int32_t p_to_line) const {
	return String();
}
void SafeGDScriptLanguage::_add_global_constant(const StringName &p_name, const Variant &p_value) {}
void SafeGDScriptLanguage::_add_named_global_constant(const StringName &p_name, const Variant &p_value) {}
void SafeGDScriptLanguage::_remove_named_global_constant(const StringName &p_name) {}
void SafeGDScriptLanguage::_thread_enter() {}
void SafeGDScriptLanguage::_thread_exit() {}
String SafeGDScriptLanguage::_debug_get_error() const {
	return String();
}
int32_t SafeGDScriptLanguage::_debug_get_stack_level_count() const {
	return 0;
}
int32_t SafeGDScriptLanguage::_debug_get_stack_level_line(int32_t p_level) const {
	return 0;
}
String SafeGDScriptLanguage::_debug_get_stack_level_function(int32_t p_level) const {
	return String();
}
Dictionary SafeGDScriptLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
Dictionary SafeGDScriptLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
void *SafeGDScriptLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	return nullptr;
}
Dictionary SafeGDScriptLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
String SafeGDScriptLanguage::_debug_parse_stack_level_expression(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	return String();
}
TypedArray<Dictionary> SafeGDScriptLanguage::_debug_get_current_stack_info() {
	return TypedArray<Dictionary>();
}
void SafeGDScriptLanguage::_reload_all_scripts() {}
void SafeGDScriptLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {}
PackedStringArray SafeGDScriptLanguage::_get_recognized_extensions() const {
	PackedStringArray array;
	array.push_back("sgd");
	return array;
}
TypedArray<Dictionary> SafeGDScriptLanguage::_get_public_functions() const {
	return TypedArray<Dictionary>();
}
Dictionary SafeGDScriptLanguage::_get_public_constants() const {
	return Dictionary();
}
TypedArray<Dictionary> SafeGDScriptLanguage::_get_public_annotations() const {
	return TypedArray<Dictionary>();
}
void SafeGDScriptLanguage::_profiling_start() {}
void SafeGDScriptLanguage::_profiling_stop() {}
int32_t SafeGDScriptLanguage::_profiling_get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
int32_t SafeGDScriptLanguage::_profiling_get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
void SafeGDScriptLanguage::_frame() {
	static bool icon_registered = register_language_icons;
	if (!icon_registered && Engine::get_singleton()->is_editor_hint()) {
		icon_registered = true;
		load_icon();
		EditorInterface::get_singleton()->get_base_control()->connect("theme_changed", callable_mp(this, &SafeGDScriptLanguage::load_icon));
	}
}
void SafeGDScriptLanguage::load_icon() {
	static bool reenter = false;
	if (reenter)
		return;
	reenter = true;
	if (Engine::get_singleton()->is_editor_hint() && FileAccess::file_exists(icon_path)) {
		Ref<Theme> editor_theme = EditorInterface::get_singleton()->get_editor_theme();
		if (editor_theme.is_valid() && !editor_theme->has_icon("SafeGDScript", "EditorIcons"))
		{
			ResourceLoader *resource_loader = ResourceLoader::get_singleton();
			Ref<Texture2D> tex = resource_loader->load(icon_path);
			editor_theme->set_icon("SafeGDScript", "EditorIcons", tex);
		}
	}
	reenter = false;
}
bool SafeGDScriptLanguage::_handles_global_class_type(const String &p_type) const {
	return p_type == "SafeGDScript" || p_type == "Sandbox";
}
Dictionary SafeGDScriptLanguage::_get_global_class_name(const String &p_path) const {
	Dictionary dict;
	if (!p_path.is_empty()) {
		dict["name"] = SafeGDScript::PathToGlobalName(p_path);
		dict["base_type"] = "Sandbox";
		dict["icon_path"] = String(icon_path);
	}
	return dict;
}
