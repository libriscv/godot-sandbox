#include "script_language_cpp.h"
#include "script_cpp.h"
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/control.hpp>
#include <string>
#include <unordered_set>
static constexpr const char *icon_path = "res://addons/godot_sandbox/CPPScript.svg";

static CPPScriptLanguage *cpp_language;

void CPPScriptLanguage::init() {
	cpp_language = memnew(CPPScriptLanguage);
	Engine::get_singleton()->register_script_language(cpp_language);
}

CPPScriptLanguage *CPPScriptLanguage::get_singleton() {
	return cpp_language;
}

String CPPScriptLanguage::GODOT_CPP_FUNC (get_name)() const {
	return "CPPScript";
}
void CPPScriptLanguage::GODOT_CPP_FUNC (init)() {}
String CPPScriptLanguage::GODOT_CPP_FUNC (get_type)() const {
	return "CPPScript";
}
String CPPScriptLanguage::GODOT_CPP_FUNC (get_extension)() const {
	return "cpp";
}
void CPPScriptLanguage::GODOT_CPP_FUNC (finish)() {}
PackedStringArray CPPScriptLanguage::GODOT_CPP_FUNC (get_reserved_words)() const {
	static const PackedStringArray reserved_words{
		"alignas",
		"alignof",
		"and",
		"and_eq",
		"asm",
		"atomic_cancel",
		"atomic_commit",
		"atomic_noexcept",
		"auto",
		"bitand",
		"bitor",
		"break",
		"case",
		"catch",
		"char8_t",
		"char16_t",
		"char32_t",
		"class",
		"compl",
		"concept",
		"const",
		"consteval",
		"constexpr",
		"const_cast",
		"continue",
		"co_await",
		"co_return",
		"co_yield",
		"decltype",
		"default",
		"delete",
		"do",
		"dynamic_cast",
		"else",
		"enum",
		"explicit",
		"export",
		"extern",
		"false",
		"for",
		"friend",
		"goto",
		"if",
		"inline",
		"mutable",
		"namespace",
		"new",
		"noexcept",
		"not",
		"not_eq",
		"nullptr",
		"operator",
		"or",
		"or_eq",
		"private",
		"protected",
		"public",
		"reflexpr",
		"register",
		"reinterpret_cast",
		"requires",
		"return",
		"signed",
		"sizeof",
		"static",
		"static_assert",
		"static_cast",
		"struct",
		"switch",
		"synchronized",
		"template",
		"this",
		"thread_local",
		"throw",
		"true",
		"try",
		"typedef",
		"typeid",
		"typename",
		"union",
		"unsigned",
		"using",
		"virtual",
		"void",
		"volatile",
		"wchar_t",
		"while",
		"xor",
		"xor_eq",

		"bool",
		"char",
		"int",
		"long",
		"float",
		"double",
		"short",
		"int8_t",
		"int16_t",
		"int32_t",
		"int64_t",
		"uint8_t",
		"uint16_t",
		"uint32_t",
		"uint64_t",
		"size_t",
		"ssize_t",
		"intptr_t",
		"uintptr_t",
		"ptrdiff_t",
		"nullptr_t",
	};
	return reserved_words;
}
bool CPPScriptLanguage::GODOT_CPP_FUNC (is_control_flow_keyword)(const String &p_keyword) const {
	static const std::unordered_set<std::string> control_flow_keywords{
		"if", "else", "switch", "case", "default", "while", "do", "for", "break", "continue", "return", "goto", "try", "catch", "throw", "co_await", "co_return", "co_yield"
	};
	return control_flow_keywords.find(p_keyword.utf8().get_data()) != control_flow_keywords.end();
}
PackedStringArray CPPScriptLanguage::GODOT_CPP_FUNC (get_comment_delimiters)() const {
	PackedStringArray comment_delimiters;
	comment_delimiters.push_back("/* */");
	comment_delimiters.push_back("//");
	return comment_delimiters;
}
PackedStringArray CPPScriptLanguage::GODOT_CPP_FUNC (get_doc_comment_delimiters)() const {
	PackedStringArray doc_comment_delimiters;
	doc_comment_delimiters.push_back("///");
	doc_comment_delimiters.push_back("/** */");
	return doc_comment_delimiters;
}
PackedStringArray CPPScriptLanguage::GODOT_CPP_FUNC (get_string_delimiters)() const {
	PackedStringArray string_delimiters;
	string_delimiters.push_back("' '");
	string_delimiters.push_back("\" \"");
	return string_delimiters;
}
Ref<Script> CPPScriptLanguage::GODOT_CPP_FUNC (make_template)(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	CPPScript *script = memnew(CPPScript);
	return Ref<Script>(script);
}
TypedArray<Dictionary> CPPScriptLanguage::GODOT_CPP_FUNC (get_built_in_templates)(const StringName &p_object) const {
	return TypedArray<Dictionary>();
}
bool CPPScriptLanguage::GODOT_CPP_FUNC (is_using_templates)() {
	return false;
}
Dictionary CPPScriptLanguage::GODOT_CPP_FUNC (validate)(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	return Dictionary();
}
String CPPScriptLanguage::GODOT_CPP_FUNC (validate_path)(const String &p_path) const {
	return String();
}
Object *CPPScriptLanguage::GODOT_CPP_FUNC (create_script)() const {
	CPPScript *script = memnew(CPPScript);
	return script;
}
bool CPPScriptLanguage::GODOT_CPP_FUNC (has_named_classes)() const {
	return false;
}
bool CPPScriptLanguage::GODOT_CPP_FUNC (supports_builtin_mode)() const {
	return false;
}
bool CPPScriptLanguage::GODOT_CPP_FUNC (supports_documentation)() const {
	return false;
}
bool CPPScriptLanguage::GODOT_CPP_FUNC (can_inherit_from_file)() const {
	return false;
}
int32_t CPPScriptLanguage::GODOT_CPP_FUNC (find_function)(const String &p_function, const String &p_code) const {
	return -1;
}
String CPPScriptLanguage::GODOT_CPP_FUNC (make_function)(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	return String();
}
Error CPPScriptLanguage::GODOT_CPP_FUNC (open_in_external_editor)(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) {
	return Error::OK;
}
bool CPPScriptLanguage::GODOT_CPP_FUNC (overrides_external_editor)() {
	return false;
}
Dictionary CPPScriptLanguage::GODOT_CPP_FUNC (complete_code)(const String &p_code, const String &p_path, Object *p_owner) const {
	return Dictionary();
}
Dictionary CPPScriptLanguage::GODOT_CPP_FUNC (lookup_code)(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	return Dictionary();
}
String CPPScriptLanguage::GODOT_CPP_FUNC (auto_indent_code)(const String &p_code, int32_t p_from_line, int32_t p_to_line) const {
	return String();
}
void CPPScriptLanguage::GODOT_CPP_FUNC (add_global_constant)(const StringName &p_name, const Variant &p_value) {}
void CPPScriptLanguage::GODOT_CPP_FUNC (add_named_global_constant)(const StringName &p_name, const Variant &p_value) {}
void CPPScriptLanguage::GODOT_CPP_FUNC (remove_named_global_constant)(const StringName &p_name) {}
void CPPScriptLanguage::GODOT_CPP_FUNC (thread_enter)() {}
void CPPScriptLanguage::GODOT_CPP_FUNC (thread_exit)() {}
String CPPScriptLanguage::GODOT_CPP_FUNC (debug_get_error)() const {
	return String();
}
int32_t CPPScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_count)() const {
	return 0;
}
int32_t CPPScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_line)(int32_t p_level) const {
	return 0;
}
String CPPScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_function)(int32_t p_level) const {
	return String();
}
Dictionary CPPScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_locals)(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
Dictionary CPPScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_members)(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
void *CPPScriptLanguage::GODOT_CPP_FUNC (debug_get_stack_level_instance)(int32_t p_level) {
	return nullptr;
}
Dictionary CPPScriptLanguage::GODOT_CPP_FUNC (debug_get_globals)(int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
String CPPScriptLanguage::GODOT_CPP_FUNC (debug_parse_stack_level_expression)(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	return String();
}
TypedArray<Dictionary> CPPScriptLanguage::GODOT_CPP_FUNC (debug_get_current_stack_info)() {
	return TypedArray<Dictionary>();
}
void CPPScriptLanguage::GODOT_CPP_FUNC (reload_all_scripts)() {}
void CPPScriptLanguage::GODOT_CPP_FUNC (reload_tool_script)(const Ref<Script> &p_script, bool p_soft_reload) {}
PackedStringArray CPPScriptLanguage::GODOT_CPP_FUNC (get_recognized_extensions)() const {
	PackedStringArray array;
	array.push_back("cpp");
	array.push_back("cc");
	array.push_back("hh");
	array.push_back("h");
	array.push_back("hpp");
	return array;
}
TypedArray<Dictionary> CPPScriptLanguage::GODOT_CPP_FUNC (get_public_functions)() const {
	return TypedArray<Dictionary>();
}
Dictionary CPPScriptLanguage::GODOT_CPP_FUNC (get_public_constants)() const {
	return Dictionary();
}
TypedArray<Dictionary> CPPScriptLanguage::GODOT_CPP_FUNC (get_public_annotations)() const {
	return TypedArray<Dictionary>();
}
void CPPScriptLanguage::GODOT_CPP_FUNC (profiling_start)() {}
void CPPScriptLanguage::GODOT_CPP_FUNC (profiling_stop)() {}
int32_t CPPScriptLanguage::GODOT_CPP_FUNC (profiling_get_accumulated_data)(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
int32_t CPPScriptLanguage::GODOT_CPP_FUNC (profiling_get_frame_data)(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
void CPPScriptLanguage::GODOT_CPP_FUNC (frame)() {
	static bool icon_registered = false;
	if (!icon_registered && Engine::get_singleton()->is_editor_hint()) {
		icon_registered = true;
		// Manually register CPPScript icon
		load_icon();
		// Register theme callback
		EditorInterface::get_singleton()->get_base_control()->connect("theme_changed", callable_mp(this, &CPPScriptLanguage::load_icon));
	}
}
void CPPScriptLanguage::load_icon()
{
	static bool reenter = false;
	if (reenter)
		return;
	reenter = true;
	if (Engine::get_singleton()->is_editor_hint() && FileAccess::file_exists(icon_path)) {
		Ref<Theme> editor_theme = EditorInterface::get_singleton()->get_editor_theme();
		if (editor_theme.is_valid() && !editor_theme->has_icon("CPPScript", "EditorIcons"))
		{
			ResourceLoader *resource_loader = ResourceLoader::get_singleton();
			Ref<Texture2D> tex = resource_loader->load(icon_path);
			editor_theme->set_icon("CPPScript", "EditorIcons", tex);
		}
	}
	reenter = false;
}
bool CPPScriptLanguage::GODOT_CPP_FUNC (handles_global_class_type)(const String &p_type) const {
	return p_type == "CPPScript";
}
Dictionary CPPScriptLanguage::GODOT_CPP_FUNC (get_global_class_name)(const String &p_path) const {
	return Dictionary();
}
