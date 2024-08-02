#include "script_language_cpp.h"
#include "script_cpp.h"
#include <string>
#include <unordered_set>

String CPPScriptLanguage::_get_name() const {
	return "CPPScript";
}
void CPPScriptLanguage::_init() {}
String CPPScriptLanguage::_get_type() const {
	return "CPPScript";
}
String CPPScriptLanguage::_get_extension() const {
	return "cpp";
}
void CPPScriptLanguage::_finish() {}
PackedStringArray CPPScriptLanguage::_get_reserved_words() const {
	const std::string reserved_words[] = {
		"alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit", "atomic_noexcept",
		"auto", "bitand", "bitor", "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
		"class", "compl", "concept", "const", "consteval", "constexpr", "const_cast", "continue", "co_await",
		"co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast", "else",
		"enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline",
		"int", "int32_t", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
		"or_eq", "private", "protected", "public", "reflexpr", "register", "reinterpret_cast", "requires",
		"return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
		"synchronized", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
		"typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
	};
	PackedStringArray reserved_words_packed;
	for (auto &word : reserved_words) {
		reserved_words_packed.push_back(String(word.c_str()));
	}
	return reserved_words_packed;
}
bool CPPScriptLanguage::_is_control_flow_keyword(const String &p_keyword) const {
	const std::unordered_set<std::string> control_flow_keywords{
		"if", "else", "switch", "case", "default", "while", "do", "for", "break", "continue", "return", "goto", "try", "catch", "throw", "co_await", "co_return", "co_yield"
	};
	return control_flow_keywords.find(p_keyword.utf8().get_data()) != control_flow_keywords.end();
}
PackedStringArray CPPScriptLanguage::_get_comment_delimiters() const {
	PackedStringArray comment_delimiters;
	comment_delimiters.push_back("/* */");
	comment_delimiters.push_back("//");
	return comment_delimiters;
}
PackedStringArray CPPScriptLanguage::_get_doc_comment_delimiters() const {
	PackedStringArray doc_comment_delimiters;
	doc_comment_delimiters.push_back("///");
	doc_comment_delimiters.push_back("/** */");
	return doc_comment_delimiters;
}
PackedStringArray CPPScriptLanguage::_get_string_delimiters() const {
	PackedStringArray string_delimiters;
	string_delimiters.push_back("' '");
	string_delimiters.push_back("\" \"");
	return string_delimiters;
}
Ref<Script> CPPScriptLanguage::_make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	return Ref<Script>();
}
TypedArray<Dictionary> CPPScriptLanguage::_get_built_in_templates(const StringName &p_object) const {
	return TypedArray<Dictionary>();
}
bool CPPScriptLanguage::_is_using_templates() {
	return false;
}
Dictionary CPPScriptLanguage::_validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	return Dictionary();
}
String CPPScriptLanguage::_validate_path(const String &p_path) const {
	return String();
}
Object *CPPScriptLanguage::_create_script() const {
	CPPScript *script = memnew(CPPScript);
	return script;
}
bool CPPScriptLanguage::_has_named_classes() const {
	return false;
}
bool CPPScriptLanguage::_supports_builtin_mode() const {
	return false;
}
bool CPPScriptLanguage::_supports_documentation() const {
	return false;
}
bool CPPScriptLanguage::_can_inherit_from_file() const {
	return false;
}
int32_t CPPScriptLanguage::_find_function(const String &p_function, const String &p_code) const {
	return -1;
}
String CPPScriptLanguage::_make_function(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	return String();
}
bool CPPScriptLanguage::_can_make_function() const {
	return false;
}
Error CPPScriptLanguage::_open_in_external_editor(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) {
	return Error::OK;
}
bool CPPScriptLanguage::_overrides_external_editor() {
	return false;
}
ScriptLanguage::ScriptNameCasing CPPScriptLanguage::_preferred_file_name_casing() const {
	return ScriptNameCasing::SCRIPT_NAME_CASING_AUTO;
}
Dictionary CPPScriptLanguage::_complete_code(const String &p_code, const String &p_path, Object *p_owner) const {
	return Dictionary();
}
Dictionary CPPScriptLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	return Dictionary();
}
String CPPScriptLanguage::_auto_indent_code(const String &p_code, int32_t p_from_line, int32_t p_to_line) const {
	return String();
}
void CPPScriptLanguage::_add_global_constant(const StringName &p_name, const Variant &p_value) {}
void CPPScriptLanguage::_add_named_global_constant(const StringName &p_name, const Variant &p_value) {}
void CPPScriptLanguage::_remove_named_global_constant(const StringName &p_name) {}
void CPPScriptLanguage::_thread_enter() {}
void CPPScriptLanguage::_thread_exit() {}
String CPPScriptLanguage::_debug_get_error() const {
	return String();
}
int32_t CPPScriptLanguage::_debug_get_stack_level_count() const {
	return 0;
}
int32_t CPPScriptLanguage::_debug_get_stack_level_line(int32_t p_level) const {
	return 0;
}
String CPPScriptLanguage::_debug_get_stack_level_function(int32_t p_level) const {
	return String();
}
String CPPScriptLanguage::_debug_get_stack_level_source(int32_t p_level) const {
	return String();
}
Dictionary CPPScriptLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
Dictionary CPPScriptLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
void *CPPScriptLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	return nullptr;
}
Dictionary CPPScriptLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
String CPPScriptLanguage::_debug_parse_stack_level_expression(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	return String();
}
TypedArray<Dictionary> CPPScriptLanguage::_debug_get_current_stack_info() {
	return TypedArray<Dictionary>();
}
void CPPScriptLanguage::_reload_all_scripts() {}
void CPPScriptLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {}
PackedStringArray CPPScriptLanguage::_get_recognized_extensions() const {
	PackedStringArray array;
	array.push_back("cpp");
	array.push_back("cc");
	array.push_back("hh");
	array.push_back("h");
	array.push_back("hpp");
	return array;
}
TypedArray<Dictionary> CPPScriptLanguage::_get_public_functions() const {
	return TypedArray<Dictionary>();
}
Dictionary CPPScriptLanguage::_get_public_constants() const {
	return Dictionary();
}
TypedArray<Dictionary> CPPScriptLanguage::_get_public_annotations() const {
	return TypedArray<Dictionary>();
}
void CPPScriptLanguage::_profiling_start() {}
void CPPScriptLanguage::_profiling_stop() {}
void CPPScriptLanguage::_profiling_set_save_native_calls(bool p_enable) {}
int32_t CPPScriptLanguage::_profiling_get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
int32_t CPPScriptLanguage::_profiling_get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
void CPPScriptLanguage::_frame() {}
bool CPPScriptLanguage::_handles_global_class_type(const String &p_type) const {
	return false;
}
Dictionary CPPScriptLanguage::_get_global_class_name(const String &p_path) const {
	return Dictionary();
}
