/**************************************************************************/
/*  gdscript_elf.cpp                                                      */
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

#include "gdscript_elf.h"
#include "gdscript_elf_language.h"
#include "gdscript_elf_instance.h"
#include "gdscript_elf_function.h"
#include "../compilation/gdscript_parser.h"
#include "../compilation/gdscript_analyzer.h"
#include "../compilation/gdscript_compiler.h"
#include "../compilation/gdscript_function.h"
#include "../elf/gdscript_bytecode_elf_compiler.h"
#include "core/io/file_access.h"
#include "core/object/script_language.h"
#include "scene/scene_string_names.h"

void GDScriptELF::_bind_methods() {
	// Bind methods if needed
}

GDScriptELF::GDScriptELF() {
	valid = false;
	tool = false;
	reloading = false;
	_is_abstract = false;
	subclass_count = 0;
	destructing = false;
	clearing = false;
	path_valid = false;
	compilation_error = OK;
}

GDScriptELF::~GDScriptELF() {
	_clear();
}

bool GDScriptELF::is_valid() const {
	return valid;
}

bool GDScriptELF::inherits_script(const Ref<Script> &p_script) const {
	if (p_script.is_null()) {
		return false;
	}
	
	Ref<GDScriptELF> gdelf = p_script;
	if (gdelf.is_null()) {
		return false;
	}
	
	GDScriptELF *s = this;
	while (s) {
		if (s == gdelf.ptr()) {
			return true;
		}
		s = s->_base;
	}
	
	return false;
}

bool GDScriptELF::is_tool() const {
	return tool;
}

bool GDScriptELF::is_abstract() const {
	return _is_abstract;
}

Ref<Script> GDScriptELF::get_base_script() const {
	if (base.is_valid()) {
		return base;
	}
	return Ref<Script>();
}

StringName GDScriptELF::get_global_name() const {
	return global_name;
}

StringName GDScriptELF::get_instance_base_type() const {
	if (analyzer.is_valid() && analyzer->get_parser()) {
		const GDScriptParser::ClassNode *class_node = analyzer->get_parser()->get_tree();
		if (class_node && class_node->base_type.is_valid()) {
			return class_node->base_type->name;
		}
	}
	return StringName("RefCounted"); // Default base type
}

ScriptInstance *GDScriptELF::instance_create(Object *p_this) {
	if (!valid) {
		return nullptr;
	}

	GDScriptELFInstance *instance = memnew(GDScriptELFInstance);
	instance->owner = p_this;
	instance->owner_id = p_this->get_instance_id();
	instance->script = Ref<GDScriptELF>(this);
	instance->reload_members();

	// Initialize sandbox for ELF execution
	instance->_initialize_sandbox();

	// Call implicit initializer if available
	if (implicit_initializer) {
		Callable::CallError ce;
		implicit_initializer->call(instance, nullptr, 0, ce);
	}

	return instance;
}

PlaceHolderScriptInstance *GDScriptELF::placeholder_instance_create(Object *p_this) {
	// TODO: Create placeholder instance
	return nullptr;
}

bool GDScriptELF::has_script_signal(const StringName &p_signal) const {
	return _signals.has(p_signal);
}

void GDScriptELF::get_script_signal_list(List<MethodInfo> *r_signals) const {
	for (const KeyValue<StringName, MethodInfo> &E : _signals) {
		r_signals->push_back(E.value);
	}
}

bool GDScriptELF::can_instantiate() const {
	return valid && !_is_abstract;
}

bool GDScriptELF::has_source_code() const {
	return !source.is_empty();
}

String GDScriptELF::get_source_code() const {
	return source;
}

void GDScriptELF::set_source_code(const String &p_code) {
	source = p_code;
}

Error GDScriptELF::reload(bool p_keep_state) {
	return _compile_to_elf();
}

bool GDScriptELF::has_method(const StringName &p_method) const {
	return member_functions.has(p_method);
}

bool GDScriptELF::has_static_method(const StringName &p_method) const {
	if (member_functions.has(p_method)) {
		return member_functions[p_method]->get_is_static();
	}
	return false;
}

Dictionary GDScriptELF::get_method_info(const StringName &p_method) const {
	if (!member_functions.has(p_method)) {
		return Dictionary();
	}

	GDScriptELFFunction *func = member_functions[p_method];
	MethodInfo info;
	info.name = p_method;
	info.return_val.type = func->get_return_type().has_type ? Variant::NIL : Variant::NIL; // TODO: Convert return type
	info.argument_count = func->get_argument_count();

	for (int i = 0; i < func->get_argument_types().size(); i++) {
		PropertyInfo arg_info;
		arg_info.type = Variant::NIL; // TODO: Convert from GDScriptDataType
		info.arguments.push_back(arg_info);
	}

	return info;
}

void GDScriptELF::get_script_method_list(List<MethodInfo> *p_list) const {
	for (const KeyValue<StringName, GDScriptELFFunction *> &E : member_functions) {
		MethodInfo info;
		info.name = E.key;
		info.return_val.type = E.value->get_return_type().has_type ? Variant::NIL : Variant::NIL; // TODO: Convert return type
		info.argument_count = E.value->get_argument_count();
		p_list->push_back(info);
	}
}

void GDScriptELF::get_script_property_list(List<PropertyInfo> *p_list) const {
	for (const KeyValue<StringName, MemberInfo> &E : member_indices) {
		p_list->push_back(E.value.property_info);
	}
}

bool GDScriptELF::has_property_default_value(const StringName &p_property) const {
	// Check if property has a default value from the parser
	if (analyzer.is_valid() && analyzer->get_parser()) {
		const GDScriptParser::ClassNode *class_node = analyzer->get_parser()->get_tree();
		if (class_node) {
			for (const GDScriptParser::ClassNode::Member &member : class_node->members) {
				if (member.type == GDScriptParser::ClassNode::Member::VARIABLE &&
					member.variable->identifier->name == p_property) {
					return member.variable->initializer != nullptr;
				}
			}
		}
	}
	return false;
}

Variant GDScriptELF::get_property_default_value(const StringName &p_property) const {
	// Extract default value from parser
	if (analyzer.is_valid() && analyzer->get_parser()) {
		const GDScriptParser::ClassNode *class_node = analyzer->get_parser()->get_tree();
		if (class_node) {
			for (const GDScriptParser::ClassNode::Member &member : class_node->members) {
				if (member.type == GDScriptParser::ClassNode::Member::VARIABLE &&
					member.variable->identifier->name == p_property &&
					member.variable->initializer) {
					// TODO: Evaluate initializer expression to get default value
					// This requires running the analyzer's constant evaluation
					return Variant();
				}
			}
		}
	}
	return Variant();
}

void GDScriptELF::update_exports() {
	// TODO: Update exports
}

Variant GDScriptELF::get_rpc_config() const {
	return rpc_config;
}

Ref<GDScriptELF> GDScriptELF::get_base() const {
	return base;
}

GDScriptELF *GDScriptELF::find_class(const String &p_qualified_name) {
	// TODO: Find class by qualified name
	return nullptr;
}

bool GDScriptELF::has_class(const GDScriptELF *p_script) const {
	// TODO: Check if script has class
	return false;
}

PackedByteArray GDScriptELF::get_function_elf(const StringName &p_function_name) const {
	if (function_elf_binaries.has(p_function_name)) {
		return function_elf_binaries[p_function_name];
	}
	return PackedByteArray();
}

bool GDScriptELF::has_function_elf(const StringName &p_function_name) const {
	return function_elf_binaries.has(p_function_name);
}

Error GDScriptELF::_compile_to_elf() {
	valid = false;
	compilation_error = OK;

	if (source.is_empty()) {
		// Try to load from file
		if (path_valid && !path.is_empty()) {
			Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
			if (file.is_valid()) {
				source = file->get_as_text();
			}
		}
	}

	if (source.is_empty()) {
		compilation_error = ERR_FILE_NOT_FOUND;
		return compilation_error;
	}

	// Parse
	parser = memnew(GDScriptParser);
	Error err = parser->parse(source, path, false);
	if (err != OK) {
		compilation_error = err;
		memdelete(parser.ptr());
		parser.unref();
		return err;
	}

	// Analyze
	analyzer = memnew(GDScriptAnalyzer(parser.ptr()));
	err = analyzer->analyze();
	if (err != OK) {
		compilation_error = err;
		memdelete(analyzer.ptr());
		analyzer.unref();
		return err;
	}

	// Compile to bytecode first (using GDScriptCompiler)
	// Note: We need to create a temporary GDScript to use the compiler
	// For now, we'll extract function information from the parser/analyzer
	// and compile functions individually to ELF

	const GDScriptParser::ClassNode *class_node = parser->get_tree();
	if (!class_node) {
		compilation_error = ERR_INVALID_DATA;
		return compilation_error;
	}

	// Extract class information
	if (class_node->identifier) {
		local_name = class_node->identifier->name;
	}

	// Extract base class
	if (class_node->base_type.is_valid()) {
		// Base class handling would go here
	}

	// Extract constants
	for (const GDScriptParser::ClassNode::Member &member : class_node->members) {
		if (member.type == GDScriptParser::ClassNode::Member::VARIABLE) {
			if (member.variable->is_constant) {
				constants[member.variable->identifier->name] = Variant(); // TODO: Evaluate constant value
			}
		}
	}

	// Extract member variables
	int member_index = 0;
	for (const GDScriptParser::ClassNode::Member &member : class_node->members) {
		if (member.type == GDScriptParser::ClassNode::Member::VARIABLE && !member.variable->is_constant) {
			MemberInfo info;
			info.index = member_index++;
			info.data_type = GDScriptDataType(); // TODO: Convert from parser data type
			info.property_info.name = member.variable->identifier->name;
			info.property_info.type = Variant::NIL; // TODO: Convert from data type
			member_indices[member.variable->identifier->name] = info;
			members.insert(member.variable->identifier->name);
		}
	}

	// Extract and compile functions to ELF
	for (const GDScriptParser::ClassNode::Member &member : class_node->members) {
		if (member.type == GDScriptParser::ClassNode::Member::FUNCTION) {
			StringName func_name = member.function->identifier->name;

			// Create GDScriptELFFunction
			GDScriptELFFunction *func = memnew(GDScriptELFFunction);
			func->name = func_name;
			func->script = this;
			func->argument_count = member.function->parameters.size();
			func->line = member.function->start_line;

			// Extract argument types
			for (const GDScriptParser::ParameterNode *param : member.function->parameters) {
				GDScriptDataType arg_type;
				if (param->data_type.is_set()) {
					// TODO: Convert parser data type to GDScriptDataType
				}
				func->argument_types.push_back(arg_type);
			}

			// TODO: Compile function bytecode to ELF
			// For now, we'll need to use GDScriptCompiler to generate bytecode first
			// Then use GDScriptBytecodeELFCompiler to compile to ELF
			// This is a complex integration that requires:
			// 1. Creating a temporary GDScriptFunction with bytecode
			// 2. Compiling that to ELF using GDScriptBytecodeELFCompiler
			// 3. Storing the ELF binary

			// For now, mark function as created but not yet compiled to ELF
			member_functions[func_name] = func;

			// Store special functions
			if (func_name == StringName("_init")) {
				initializer = func;
			} else if (func_name == StringName("_ready")) {
				implicit_ready = func;
			} else if (func_name == StringName("_static_init")) {
				static_initializer = func;
			}
		}
	}

	valid = true;
	return OK;
}

void GDScriptELF::_clear() {
	clearing = true;
	
	member_functions.clear();
	member_indices.clear();
	members.clear();
	constants.clear();
	subclasses.clear();
	_signals.clear();
	function_elf_binaries.clear();
	
	base.unref();
	_base = nullptr;
	_owner = nullptr;
	
	parser.unref();
	analyzer.unref();
	
	clearing = false;
}

bool GDScriptELF::_get(const StringName &p_name, Variant &r_ret) const {
	if (constants.has(p_name)) {
		r_ret = constants[p_name];
		return true;
	}
	return false;
}

bool GDScriptELF::_set(const StringName &p_name, const Variant &p_value) {
	// Script-level properties are read-only
	return false;
}

void GDScriptELF::_get_property_list(List<PropertyInfo> *p_properties) const {
	// Add constants as properties
	for (const KeyValue<StringName, Variant> &E : constants) {
		PropertyInfo pi;
		pi.name = E.key;
		pi.type = E.value.get_type();
		pi.usage = PROPERTY_USAGE_SCRIPT_VARIABLE | PROPERTY_USAGE_DEFAULT;
		p_properties->push_back(pi);
	}
}

Variant GDScriptELF::callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	if (member_functions.has(p_method)) {
		// Static method call - would need an instance
		// For now, return error as static calls need proper handling
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}
	r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
	return Variant();
}
