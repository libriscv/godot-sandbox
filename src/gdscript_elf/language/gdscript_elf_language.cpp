/**************************************************************************/
/*  gdscript_elf_language.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gdscript_elf_language.h"
#include "gdscript_elf.h"
#include "../compilation/gdscript_parser.h"
#include "../compilation/gdscript_analyzer.h"
#include "../compilation/gdscript_compiler.h"
#include "../compilation/gdscript_utility_functions.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/defs.hpp>
#include <limits>

using namespace godot;

GDScriptELFLanguage *GDScriptELFLanguage::singleton = nullptr;

GDScriptELFLanguage::GDScriptELFLanguage() {
	singleton = this;
}

GDScriptELFLanguage::~GDScriptELFLanguage() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

String GDScriptELFLanguage::_get_name() const {
	return "GDScriptELF";
}

String GDScriptELFLanguage::_get_type() const {
	return "GDScriptELF";
}

String GDScriptELFLanguage::_get_extension() const {
	return "gde";
}

void GDScriptELFLanguage::_init() {
	// Initialize similar to GDScriptLanguage
	// Populate global constants from ProjectSettings
	Ref<ProjectSettings> project_settings = ProjectSettings::get_singleton();
	if (project_settings.is_valid()) {
		// Add common math constants
		_add_global_constant(StringName("PI"), 3.14159265358979323846);
		_add_global_constant(StringName("TAU"), 6.28318530717958647692);
		_add_global_constant(StringName("INF"), std::numeric_limits<double>::infinity());
		_add_global_constant(StringName("NAN"), std::numeric_limits<double>::quiet_NaN());
	}

	// Register utility functions
	GDScriptUtilityFunctions::register_functions();
}

void GDScriptELFLanguage::_finish() {
	// Cleanup
}

Object *GDScriptELFLanguage::_create_script() const {
	return memnew(GDScriptELF);
}

Dictionary GDScriptELFLanguage::_validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	Dictionary result;
	PackedStringArray functions;
	TypedArray<Dictionary> errors;
	TypedArray<Dictionary> warnings;
	TypedArray<int> safe_lines;
	bool valid = true;

	// Use GDScript parser/analyzer for validation
	Ref<GDScriptParser> parser = memnew(GDScriptParser);
	Error err = parser->parse(p_script, p_path, false);
	
	if (err != OK) {
		valid = false;
		if (p_validate_errors) {
			for (const GDScriptParser::ParserError &e : parser->get_errors()) {
				Dictionary error_dict;
				error_dict["line"] = e.line;
				error_dict["column"] = e.column;
				error_dict["message"] = e.message;
				errors.append(error_dict);
			}
		}
		result["valid"] = false;
		result["functions"] = functions;
		result["errors"] = errors;
		result["warnings"] = warnings;
		result["safe_lines"] = safe_lines;
		return result;
	}

	// Run analyzer
	Ref<GDScriptAnalyzer> analyzer = memnew(GDScriptAnalyzer(parser.ptr()));
	err = analyzer->analyze();
	
	if (err != OK) {
		valid = false;
		if (p_validate_errors) {
			for (const GDScriptParser::ParserError &e : analyzer->get_errors()) {
				Dictionary error_dict;
				error_dict["line"] = e.line;
				error_dict["column"] = e.column;
				error_dict["message"] = e.message;
				errors.append(error_dict);
			}
		}
		result["valid"] = false;
		result["functions"] = functions;
		result["errors"] = errors;
		result["warnings"] = warnings;
		result["safe_lines"] = safe_lines;
		return result;
	}

	// Extract functions
	if (p_validate_functions) {
		const GDScriptParser::ClassNode *class_node = parser->get_tree();
		if (class_node) {
			for (const GDScriptParser::ClassNode::Member &member : class_node->members) {
				if (member.type == GDScriptParser::ClassNode::Member::FUNCTION) {
					functions.append(member.function->identifier->name);
				}
			}
		}
	}

	result["valid"] = valid;
	result["functions"] = functions;
	result["errors"] = errors;
	result["warnings"] = warnings;
	result["safe_lines"] = safe_lines;
	return result;
}

String GDScriptELFLanguage::_validate_path(const String &p_path) const {
	// Same validation as GDScript
	// TODO: Implement path canonicalization
	return p_path;
}

PackedStringArray GDScriptELFLanguage::_get_reserved_words() const {
	// Use same reserved words as GDScript
	PackedStringArray words;
	// TODO: Populate from GDScript parser
	return words;
}

bool GDScriptELFLanguage::_is_control_flow_keyword(const String &p_keyword) const {
	// Use same keywords as GDScript
	return p_keyword == "if" || p_keyword == "elif" || p_keyword == "else" ||
		   p_keyword == "for" || p_keyword == "while" || p_keyword == "match" ||
		   p_keyword == "break" || p_keyword == "continue" || p_keyword == "return";
}

PackedStringArray GDScriptELFLanguage::_get_comment_delimiters() const {
	PackedStringArray delimiters;
	delimiters.append("#");
	return delimiters;
}

PackedStringArray GDScriptELFLanguage::_get_doc_comment_delimiters() const {
	PackedStringArray delimiters;
	delimiters.append("##");
	return delimiters;
}

PackedStringArray GDScriptELFLanguage::_get_string_delimiters() const {
	PackedStringArray delimiters;
	delimiters.append("\"");
	delimiters.append("'");
	return delimiters;
}

Ref<Script> GDScriptELFLanguage::_make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	Ref<GDScriptELF> script = memnew(GDScriptELF);
	// TODO: Implement template generation
	return script;
}

TypedArray<Dictionary> GDScriptELFLanguage::_get_built_in_templates(const StringName &p_object) const {
	return TypedArray<Dictionary>();
}

bool GDScriptELFLanguage::_is_using_templates() {
	return false;
}

bool GDScriptELFLanguage::_supports_builtin_mode() const {
	return true;
}

bool GDScriptELFLanguage::_supports_documentation() const {
	return true;
}

bool GDScriptELFLanguage::_can_inherit_from_file() const {
	return true;
}

bool GDScriptELFLanguage::_has_named_classes() const {
	return true;
}

int32_t GDScriptELFLanguage::_find_function(const String &p_function, const String &p_code) const {
	// Use GDScript parser to find function
	Ref<GDScriptParser> parser = memnew(GDScriptParser);
	parser->parse(p_code, "", false);
	const GDScriptParser::ClassNode *class_node = parser->get_tree();
	if (class_node) {
		for (const GDScriptParser::ClassNode::Member &member : class_node->members) {
			if (member.type == GDScriptParser::ClassNode::Member::FUNCTION &&
				member.function->identifier->name == p_function) {
				return member.function->start_line;
			}
		}
	}
	return -1;
}

String GDScriptELFLanguage::_make_function(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	String code = "func " + p_function_name + "(";
	for (int i = 0; i < p_function_args.size(); i++) {
		if (i > 0) {
			code += ", ";
		}
		code += p_function_args[i];
	}
	code += "):\n\tpass\n";
	return code;
}

bool GDScriptELFLanguage::_can_make_function() const {
	return true;
}

Error GDScriptELFLanguage::_open_in_external_editor(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) {
	return ERR_UNAVAILABLE;
}

bool GDScriptELFLanguage::_overrides_external_editor() {
	return false;
}

Dictionary GDScriptELFLanguage::_complete_code(const String &p_code, const String &p_path, Object *p_owner) const {
	// TODO: Implement code completion
	Dictionary result;
	result["options"] = TypedArray<Dictionary>();
	result["forced"] = false;
	result["call_hint"] = "";
	return result;
}

Dictionary GDScriptELFLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	// TODO: Implement code lookup
	Dictionary result;
	return result;
}

String GDScriptELFLanguage::_auto_indent_code(const String &p_code, int32_t p_from_line, int32_t p_to_line) const {
	// TODO: Implement auto-indentation
	return p_code;
}

void GDScriptELFLanguage::_add_global_constant(const StringName &p_name, const Variant &p_value) {
	// Store in a map similar to GDScriptLanguage
	// TODO: Implement global constant storage
}

void GDScriptELFLanguage::_add_named_global_constant(const StringName &p_name, const Variant &p_value) {
	// Store in a map similar to GDScriptLanguage
	// TODO: Implement named global constant storage
}

void GDScriptELFLanguage::_remove_named_global_constant(const StringName &p_name) {
	// Remove from map
	// TODO: Implement named global constant removal
}

void GDScriptELFLanguage::_thread_enter() {
	// Thread initialization
}

void GDScriptELFLanguage::_thread_exit() {
	// Thread cleanup
}

String GDScriptELFLanguage::_debug_get_error() const {
	return "";
}

int32_t GDScriptELFLanguage::_debug_get_stack_level_count() const {
	return 0;
}

int32_t GDScriptELFLanguage::_debug_get_stack_level_line(int32_t p_level) const {
	return -1;
}

String GDScriptELFLanguage::_debug_get_stack_level_function(int32_t p_level) const {
	return "";
}

Dictionary GDScriptELFLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	// TODO: Implement debug locals
	Dictionary result;
	result["locals"] = PackedStringArray();
	result["values"] = Array();
	return result;
}

Dictionary GDScriptELFLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	// TODO: Implement debug members
	Dictionary result;
	result["members"] = PackedStringArray();
	result["values"] = Array();
	return result;
}

void *GDScriptELFLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	return nullptr;
}

Dictionary GDScriptELFLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	// TODO: Implement debug globals
	Dictionary result;
	result["globals"] = PackedStringArray();
	result["values"] = Array();
	return result;
}

String GDScriptELFLanguage::_debug_parse_stack_level_expression(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	return "";
}

TypedArray<Dictionary> GDScriptELFLanguage::_debug_get_current_stack_info() {
	return TypedArray<Dictionary>();
}

void GDScriptELFLanguage::_reload_all_scripts() {
	// Reload all GDScriptELF scripts
	// TODO: Implement script reloading
}

void GDScriptELFLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {
	if (p_script.is_valid()) {
		p_script->reload(p_soft_reload);
	}
}

PackedStringArray GDScriptELFLanguage::_get_recognized_extensions() const {
	PackedStringArray extensions;
	extensions.append("gde");
	return extensions;
}

TypedArray<Dictionary> GDScriptELFLanguage::_get_public_functions() const {
	// Return public functions
	// TODO: Implement public functions retrieval
	return TypedArray<Dictionary>();
}

Dictionary GDScriptELFLanguage::_get_public_constants() const {
	// Return public constants
	// TODO: Implement public constants retrieval
	return Dictionary();
}

TypedArray<Dictionary> GDScriptELFLanguage::_get_public_annotations() const {
	// Return public annotations
	// TODO: Implement public annotations retrieval
	return TypedArray<Dictionary>();
}

void GDScriptELFLanguage::_profiling_start() {
	// TODO: Start profiling
}

void GDScriptELFLanguage::_profiling_stop() {
	// TODO: Stop profiling
}

int32_t GDScriptELFLanguage::_profiling_get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	// TODO: Implement profiling data retrieval
	return 0;
}

int32_t GDScriptELFLanguage::_profiling_get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	// TODO: Implement profiling frame data retrieval
	return 0;
}

void GDScriptELFLanguage::_frame() {
	// Per-frame updates
}

bool GDScriptELFLanguage::_handles_global_class_type(const String &p_type) const {
	return p_type == "GDScriptELF";
}

Dictionary GDScriptELFLanguage::_get_global_class_name(const String &p_path) const {
	// TODO: Extract global class name from script
	Dictionary result;
	result["name"] = "";
	result["base_type"] = "";
	result["icon_path"] = "";
	result["is_abstract"] = false;
	result["is_tool"] = false;
	return result;
}
