#pragma once

#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_language_extension.hpp>

GODOT_NAMESPACE

class ELFScriptLanguage : public ScriptLanguageExtension {
	GDCLASS(ELFScriptLanguage, ScriptLanguageExtension);

protected:
	static void GODOT_CPP_FUNC (bind_methods)() {}
	String file;

public:
	virtual String GODOT_CPP_FUNC (get_name)() const override;
	virtual void GODOT_CPP_FUNC (init)() override;
	virtual String GODOT_CPP_FUNC (get_type)() const override;
	virtual String GODOT_CPP_FUNC (get_extension)() const override;
	virtual void GODOT_CPP_FUNC (finish)() override;
	virtual PackedStringArray GODOT_CPP_FUNC (get_reserved_words)() const override;
	virtual bool GODOT_CPP_FUNC (is_control_flow_keyword)(const String &p_keyword) const override;
	virtual PackedStringArray GODOT_CPP_FUNC (get_comment_delimiters)() const override;
	virtual PackedStringArray GODOT_CPP_FUNC (get_doc_comment_delimiters)() const override;
	virtual PackedStringArray GODOT_CPP_FUNC (get_string_delimiters)() const override;
	virtual Ref<Script> GODOT_CPP_FUNC (make_template)(const String &p_template, const String &p_class_name, const String &p_base_class_name) const override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (get_built_in_templates)(const StringName &p_object) const override;
	virtual bool GODOT_CPP_FUNC (is_using_templates)() override;
	virtual Dictionary GODOT_CPP_FUNC (validate)(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const override;
	virtual String GODOT_CPP_FUNC (validate_path)(const String &p_path) const override;
	virtual Object *_create_script() const override;
	virtual bool GODOT_CPP_FUNC (has_named_classes)() const override;
	virtual bool GODOT_CPP_FUNC (supports_builtin_mode)() const override;
	virtual bool GODOT_CPP_FUNC (supports_documentation)() const override;
	virtual bool GODOT_CPP_FUNC (can_inherit_from_file)() const override;
	virtual int32_t GODOT_CPP_FUNC (find_function)(const String &p_function, const String &p_code) const override;
	virtual String GODOT_CPP_FUNC (make_function)(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const override;
	virtual Error GODOT_CPP_FUNC (open_in_external_editor)(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) override;
	virtual bool GODOT_CPP_FUNC (overrides_external_editor)() override;
	virtual Dictionary GODOT_CPP_FUNC (complete_code)(const String &p_code, const String &p_path, Object *p_owner) const override;
	virtual Dictionary GODOT_CPP_FUNC (lookup_code)(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const override;
	virtual String GODOT_CPP_FUNC (auto_indent_code)(const String &p_code, int32_t p_from_line, int32_t p_to_line) const override;
	virtual void GODOT_CPP_FUNC (add_global_constant)(const StringName &p_name, const Variant &p_value) override;
	virtual void GODOT_CPP_FUNC (add_named_global_constant)(const StringName &p_name, const Variant &p_value) override;
	virtual void GODOT_CPP_FUNC (remove_named_global_constant)(const StringName &p_name) override;
	virtual void GODOT_CPP_FUNC (thread_enter)() override;
	virtual void GODOT_CPP_FUNC (thread_exit)() override;
	virtual String GODOT_CPP_FUNC (debug_get_error)() const override;
	virtual int32_t GODOT_CPP_FUNC (debug_get_stack_level_count)() const override;
	virtual int32_t GODOT_CPP_FUNC (debug_get_stack_level_line)(int32_t p_level) const override;
	virtual String GODOT_CPP_FUNC (debug_get_stack_level_function)(int32_t p_level) const override;
	virtual Dictionary GODOT_CPP_FUNC (debug_get_stack_level_locals)(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) override;
	virtual Dictionary GODOT_CPP_FUNC (debug_get_stack_level_members)(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) override;
	virtual void *_debug_get_stack_level_instance(int32_t p_level) override;
	virtual Dictionary GODOT_CPP_FUNC (debug_get_globals)(int32_t p_max_subitems, int32_t p_max_depth) override;
	virtual String GODOT_CPP_FUNC (debug_parse_stack_level_expression)(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (debug_get_current_stack_info)() override;
	virtual void GODOT_CPP_FUNC (reload_all_scripts)() override;
	virtual void GODOT_CPP_FUNC (reload_tool_script)(const Ref<Script> &p_script, bool p_soft_reload) override;
	virtual PackedStringArray GODOT_CPP_FUNC (get_recognized_extensions)() const override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (get_public_functions)() const override;
	virtual Dictionary GODOT_CPP_FUNC (get_public_constants)() const override;
	virtual TypedArray<Dictionary> GODOT_CPP_FUNC (get_public_annotations)() const override;
	virtual void GODOT_CPP_FUNC (profiling_start)() override;
	virtual void GODOT_CPP_FUNC (profiling_stop)() override;
	virtual int32_t GODOT_CPP_FUNC (profiling_get_accumulated_data)(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) override;
	virtual int32_t GODOT_CPP_FUNC (profiling_get_frame_data)(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) override;
	virtual void GODOT_CPP_FUNC (frame)() override;
	virtual bool GODOT_CPP_FUNC (handles_global_class_type)(const String &p_type) const override;
	virtual Dictionary GODOT_CPP_FUNC (get_global_class_name)(const String &p_path) const override;
	void load_icon();
	
	ELFScriptLanguage() {}
	~ELFScriptLanguage() {}
};
