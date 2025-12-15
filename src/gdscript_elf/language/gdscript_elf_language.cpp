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

String GDScriptELFLanguage::get_name() const {
	return "GDScriptELF";
}

String GDScriptELFLanguage::get_type() const {
	return "GDScriptELF";
}

String GDScriptELFLanguage::get_extension() const {
	return "gde";
}

void GDScriptELFLanguage::init() {
	// Initialize similar to GDScriptLanguage
	// Populate global constants
	int gcc = CoreConstants::get_global_constant_count();
	for (int i = 0; i < gcc; i++) {
		add_global_constant(StringName(CoreConstants::get_global_constant_name(i)), CoreConstants::get_global_constant_value(i));
	}

	add_global_constant(StringName("PI"), Math::PI);
	add_global_constant(StringName("TAU"), Math::TAU);
	add_global_constant(StringName("INF"), Math::INF);
	add_global_constant(StringName("NAN"), Math::NaN);

	// Register utility functions
	GDScriptUtilityFunctions::register_functions();
}

void GDScriptELFLanguage::finish() {
	// Cleanup
}

Script *GDScriptELFLanguage::create_script() const {
	return memnew(GDScriptELF);
}

bool GDScriptELFLanguage::validate(const String &p_script, const String &p_path, List<String> *r_functions, List<ScriptError> *r_errors, List<Warning> *r_warnings, HashSet<int> *r_safe_lines) const {
	// Use GDScript parser/analyzer for validation
	Ref<GDScriptParser> parser = memnew(GDScriptParser);
	Error err = parser->parse(p_script, p_path, false);
	
	if (err != OK) {
		if (r_errors) {
			for (const GDScriptParser::ParserError &e : parser->get_errors()) {
				ScriptError error;
				error.line = e.line;
				error.column = e.column;
				error.message = e.message;
				r_errors->push_back(error);
			}
		}
		return false;
	}

	// Run analyzer
	Ref<GDScriptAnalyzer> analyzer = memnew(GDScriptAnalyzer(parser.ptr()));
	err = analyzer->analyze();
	
	if (err != OK) {
		if (r_errors) {
			for (const GDScriptParser::ParserError &e : analyzer->get_errors()) {
				ScriptError error;
				error.line = e.line;
				error.column = e.column;
				error.message = e.message;
				r_errors->push_back(error);
			}
		}
		return false;
	}

	// Extract functions
	if (r_functions) {
		const GDScriptParser::ClassNode *class_node = parser->get_tree();
		if (class_node) {
			for (const GDScriptParser::ClassNode::Member &member : class_node->members) {
				if (member.type == GDScriptParser::ClassNode::Member::FUNCTION) {
					r_functions->push_back(member.function->identifier->name);
				}
			}
		}
	}

	return true;
}

String GDScriptELFLanguage::validate_path(const String &p_path) const {
	// Same validation as GDScript
	return GDScript::canonicalize_path(p_path);
}

Vector<String> GDScriptELFLanguage::get_reserved_words() const {
	// Use same reserved words as GDScript
	return Vector<String>(); // Will be populated from GDScript parser
}

bool GDScriptELFLanguage::is_control_flow_keyword(const String &p_string) const {
	// Use same keywords as GDScript
	return p_string == "if" || p_string == "elif" || p_string == "else" ||
		   p_string == "for" || p_string == "while" || p_string == "match" ||
		   p_string == "break" || p_string == "continue" || p_string == "return";
}

Vector<String> GDScriptELFLanguage::get_comment_delimiters() const {
	Vector<String> delimiters;
	delimiters.push_back("#");
	return delimiters;
}

Vector<String> GDScriptELFLanguage::get_doc_comment_delimiters() const {
	Vector<String> delimiters;
	delimiters.push_back("##");
	return delimiters;
}

Vector<String> GDScriptELFLanguage::get_string_delimiters() const {
	Vector<String> delimiters;
	delimiters.push_back("\"");
	delimiters.push_back("'");
	return delimiters;
}

Ref<Script> GDScriptELFLanguage::make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	Ref<GDScriptELF> script = memnew(GDScriptELF);
	// TODO: Implement template generation
	return script;
}

Vector<ScriptLanguage::ScriptTemplate> GDScriptELFLanguage::get_built_in_templates(const StringName &p_object) {
	return Vector<ScriptTemplate>();
}

bool GDScriptELFLanguage::is_using_templates() {
	return false;
}

bool GDScriptELFLanguage::supports_builtin_mode() const {
	return true;
}

bool GDScriptELFLanguage::supports_documentation() const {
	return true;
}

bool GDScriptELFLanguage::can_inherit_from_file() const {
	return true;
}

int GDScriptELFLanguage::find_function(const String &p_function, const String &p_code) const {
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

String GDScriptELFLanguage::make_function(const String &p_class, const String &p_name, const PackedStringArray &p_args) const {
	String code = "func " + p_name + "(";
	for (int i = 0; i < p_args.size(); i++) {
		if (i > 0) {
			code += ", ";
		}
		code += p_args[i];
	}
	code += "):\n\tpass\n";
	return code;
}

bool GDScriptELFLanguage::can_make_function() const {
	return true;
}

Error GDScriptELFLanguage::open_in_external_editor(const Ref<Script> &p_script, int p_line, int p_col) {
	return ERR_UNAVAILABLE;
}

bool GDScriptELFLanguage::overrides_external_editor() {
	return false;
}

ScriptLanguage::ScriptNameCasing GDScriptELFLanguage::preferred_file_name_casing() const {
	return SCRIPT_NAME_CASING_SNAKE_CASE;
}

Error GDScriptELFLanguage::complete_code(const String &p_code, const String &p_path, Object *p_owner, List<CodeCompletionOption> *r_options, bool &r_force, String &r_call_hint) {
	// TODO: Implement code completion
	return ERR_UNAVAILABLE;
}

#ifdef TOOLS_ENABLED
Error GDScriptELFLanguage::lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner, LookupResult &r_result) {
	// TODO: Implement code lookup
	return ERR_UNAVAILABLE;
}
#endif

void GDScriptELFLanguage::auto_indent_code(String &p_code, int p_from_line, int p_to_line) const {
	// TODO: Implement auto-indentation
}

void GDScriptELFLanguage::add_global_constant(const StringName &p_variable, const Variant &p_value) {
	// Store in a map similar to GDScriptLanguage
	// For now, just pass through to ScriptLanguage base
}

void GDScriptELFLanguage::add_named_global_constant(const StringName &p_name, const Variant &p_value) {
	// Store in a map similar to GDScriptLanguage
}

void GDScriptELFLanguage::remove_named_global_constant(const StringName &p_name) {
	// Remove from map
}

void GDScriptELFLanguage::thread_enter() {
	// Thread initialization
}

void GDScriptELFLanguage::thread_exit() {
	// Thread cleanup
}

String GDScriptELFLanguage::debug_get_error() const {
	return "";
}

int GDScriptELFLanguage::debug_get_stack_level_count() const {
	return 0;
}

int GDScriptELFLanguage::debug_get_stack_level_line(int p_level) const {
	return -1;
}

String GDScriptELFLanguage::debug_get_stack_level_function(int p_level) const {
	return "";
}

String GDScriptELFLanguage::debug_get_stack_level_source(int p_level) const {
	return "";
}

void GDScriptELFLanguage::debug_get_stack_level_locals(int p_level, List<String> *p_locals, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	// TODO: Implement debug locals
}

void GDScriptELFLanguage::debug_get_stack_level_members(int p_level, List<String> *p_members, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	// TODO: Implement debug members
}

ScriptInstance *GDScriptELFLanguage::debug_get_stack_level_instance(int p_level) {
	return nullptr;
}

void GDScriptELFLanguage::debug_get_globals(List<String> *p_globals, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	// TODO: Implement debug globals
}

void GDScriptELFLanguage::get_recognized_extensions(List<String> *p_extensions) const {
	if (p_extensions) {
		p_extensions->push_back("gde");
	}
}

void GDScriptELFLanguage::get_public_functions(List<MethodInfo> *p_functions) const {
	// Return public functions
}

void GDScriptELFLanguage::get_public_constants(List<Pair<String, Variant>> *p_constants) const {
	// Return public constants
}

void GDScriptELFLanguage::get_public_annotations(List<MethodInfo> *p_annotations) const {
	// Return public annotations
}

void GDScriptELFLanguage::profiling_set_save_native_calls(bool p_enable) {
	// TODO: Implement profiling
}

void GDScriptELFLanguage::frame() {
	// Per-frame updates
}

void GDScriptELFLanguage::reload_all_scripts() {
	// Reload all GDScriptELF scripts
}

void GDScriptELFLanguage::reload_scripts(const Array &p_scripts, bool p_soft_reload) {
	// Reload specific scripts
}

void GDScriptELFLanguage::reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {
	if (p_script.is_valid()) {
		p_script->reload(p_soft_reload);
	}
}

bool GDScriptELFLanguage::handles_global_class_type(const String &p_type) const {
	return p_type == "GDScriptELF";
}

String GDScriptELFLanguage::get_global_class_name(const String &p_path, String *r_base_type, String *r_icon_path, bool *r_is_abstract, bool *r_is_tool) const {
	// TODO: Extract global class name from script
	return "";
}

Vector<ScriptLanguage::StackInfo> GDScriptELFLanguage::debug_get_current_stack_info() {
	return Vector<StackInfo>();
}

String GDScriptELFLanguage::debug_parse_stack_level_expression(int p_level, const String &p_expression, int p_max_subitems, int p_max_depth) {
	return "";
}

void GDScriptELFLanguage::profiling_start() {
	// TODO: Start profiling
}

void GDScriptELFLanguage::profiling_stop() {
	// TODO: Stop profiling
}

int GDScriptELFLanguage::profiling_get_accumulated_data(ProfilingInfo *p_info_arr, int p_info_max) {
	return 0;
}

int GDScriptELFLanguage::profiling_get_frame_data(ProfilingInfo *p_info_arr, int p_info_max) {
	return 0;
}
