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
#include "../compilation/gdscript.h"
#include "../elf/gdscript_bytecode_elf_compiler.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/classes/scene_tree.hpp>

using namespace godot;

void GDScriptELF::_bind_methods() {
	// Bind methods if needed
}

GDScriptELF::GDScriptELF() : script_list(this) {
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

// is_valid() is already defined inline in header

bool GDScriptELF::inherits_script(const Ref<Script> &p_script) const {
	if (p_script.is_null()) {
		return false;
	}
	
	Ref<GDScriptELF> gdelf = p_script;
	if (gdelf.is_null()) {
		return false;
	}
	
	const GDScriptELF *s = this;
	while (s) {
		if (s == gdelf.ptr()) {
			return true;
		}
		s = s->_base;
	}
	
	return false;
}

// is_tool() is already defined inline in header

// is_abstract() is already defined inline in header

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
	// Use parser directly instead of analyzer->get_parser()
	if (parser.is_valid()) {
		const GDScriptParser::ClassNode *class_node = parser->get_tree();
		if (class_node && class_node->base_type.kind == GDScriptParser::DataType::CLASS) {
			return class_node->base_type.class_type->name;
		}
	}
	return StringName("RefCounted"); // Default base type
}

ScriptInstanceExtension *GDScriptELF::instance_create(Object *p_this) {
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

// PlaceHolderScriptInstance doesn't exist in GDExtension
// PlaceHolderScriptInstance *GDScriptELF::placeholder_instance_create(Object *p_this) {
// 	// TODO: Create placeholder instance
// 	return nullptr;
// }

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
	// Extract default value using analyzer's evaluation
	if (analyzer.is_valid() && analyzer->get_parser()) {
		const GDScriptParser::ClassNode *class_node = analyzer->get_parser()->get_tree();
		if (class_node) {
			for (const GDScriptParser::ClassNode::Member &member : class_node->members) {
				if (member.type == GDScriptParser::ClassNode::Member::VARIABLE &&
					member.variable->identifier->name == p_property) {
					// Use analyzer's make_variable_default_value to evaluate the default
					// This handles both initializer expressions and type defaults
					// Note: We need to cast away const since make_variable_default_value takes non-const
					// This is safe because we're only reading the variable node
					GDScriptParser::VariableNode *var_node = const_cast<GDScriptParser::VariableNode *>(member.variable);
					return analyzer->make_variable_default_value(var_node);
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
	// Create a temporary GDScript instance to use with the compiler
	Ref<GDScript> temp_script = memnew(GDScript);
	temp_script->set_source_code(source);
	temp_script->set_path(path);

	// Use GDScriptCompiler to generate bytecode
	GDScriptCompiler compiler;
	err = compiler.compile(parser.ptr(), temp_script.ptr(), false);
	if (err != OK) {
		compilation_error = err;
		ERR_PRINT("GDScriptELF: Compilation failed: " + compiler.get_error());
		return err;
	}

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

	// Extract constants from compiled script and parser
	// First get constants from compiled script (these are evaluated)
	for (const KeyValue<StringName, Variant> &E : temp_script->get_constants()) {
		constants[E.key] = E.value;
	}
	
	// Also extract constants from parser/analyzer for variables with initializers
	// These will be evaluated when needed via get_property_default_value()

	// Extract member variables from compiled script
	int member_index = 0;
	for (const KeyValue<StringName, GDScript::MemberInfo> &E : temp_script->debug_get_member_indices()) {
		MemberInfo info;
		info.index = member_index++;
		info.data_type = E.value.data_type;
		info.property_info = E.value.property_info;
		info.setter = E.value.setter;
		info.getter = E.value.getter;
		member_indices[E.key] = info;
		members.insert(E.key);
	}

	// Extract and compile functions to ELF
	// Get all member functions from the compiled script
	const HashMap<StringName, GDScriptFunction *> &compiled_functions = temp_script->get_member_functions();
	
	for (const KeyValue<StringName, GDScriptFunction *> &E : compiled_functions) {
		StringName func_name = E.key;
		GDScriptFunction *gd_function = E.value;

		// Create GDScriptELFFunction
		GDScriptELFFunction *elf_func = memnew(GDScriptELFFunction);
		elf_func->name = func_name;
		elf_func->script = this;
		elf_func->argument_count = gd_function->argument_count;
		elf_func->default_argument_count = gd_function->default_arguments.size();
		elf_func->default_arguments = gd_function->default_arguments;
		elf_func->argument_types = gd_function->argument_types;
		elf_func->return_type = gd_function->return_type;
		elf_func->is_static = gd_function->is_static;
		elf_func->is_vararg = gd_function->is_vararg();
		elf_func->has_yield = gd_function->has_yield;
		elf_func->line = gd_function->get_line();

		// Copy bytecode for VM fallback
		elf_func->code = gd_function->code;
		elf_func->constants = gd_function->constants;
		elf_func->stack = gd_function->stack;

		// Try to compile to ELF
		if (GDScriptBytecodeELFCompiler::can_compile_function(gd_function)) {
			PackedByteArray elf_binary = GDScriptBytecodeELFCompiler::compile_function_to_elf(gd_function);
			if (!elf_binary.is_empty()) {
				elf_func->set_elf_binary(elf_binary);
				function_elf_binaries[func_name] = elf_binary;
			} else {
				// ELF compilation failed, but we have bytecode fallback
				String error = GDScriptBytecodeELFCompiler::get_last_error();
				WARN_PRINT("GDScriptELF: Failed to compile function '" + String(func_name) + "' to ELF: " + error + ". Using VM fallback.");
			}
		} else {
			// Function cannot be compiled to ELF (e.g., unsupported opcodes)
			WARN_PRINT("GDScriptELF: Function '" + String(func_name) + "' cannot be compiled to ELF. Using VM fallback.");
		}

		member_functions[func_name] = elf_func;

		// Store special functions
		if (func_name == StringName("_init")) {
			initializer = elf_func;
		} else if (func_name == StringName("_ready")) {
			implicit_ready = elf_func;
		} else if (func_name == StringName("_static_init")) {
			static_initializer = elf_func;
		}
	}

	// Handle implicit initializer and ready functions
	if (temp_script->get_implicit_initializer()) {
		GDScriptFunction *gd_func = temp_script->get_implicit_initializer();
		GDScriptELFFunction *elf_func = memnew(GDScriptELFFunction);
		elf_func->name = StringName("@implicit_new");
		elf_func->script = this;
		elf_func->code = gd_func->code;
		elf_func->constants = gd_func->constants;
		elf_func->stack = gd_func->stack;
		implicit_initializer = elf_func;
	}

	if (temp_script->get_implicit_ready()) {
		GDScriptFunction *gd_func = temp_script->get_implicit_ready();
		GDScriptELFFunction *elf_func = memnew(GDScriptELFFunction);
		elf_func->name = StringName("@implicit_ready");
		elf_func->script = this;
		elf_func->code = gd_func->code;
		elf_func->constants = gd_func->constants;
		elf_func->stack = gd_func->stack;
		if (!implicit_ready) {
			implicit_ready = elf_func;
		}
	}

	if (temp_script->get_static_initializer()) {
		GDScriptFunction *gd_func = temp_script->get_static_initializer();
		GDScriptELFFunction *elf_func = memnew(GDScriptELFFunction);
		elf_func->name = StringName("@static_init");
		elf_func->script = this;
		elf_func->code = gd_func->code;
		elf_func->constants = gd_func->constants;
		elf_func->stack = gd_func->stack;
		if (!static_initializer) {
			static_initializer = elf_func;
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
