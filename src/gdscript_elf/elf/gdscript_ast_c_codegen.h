/**************************************************************************/
/*  gdscript_ast_c_codegen.h                                             */
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

#pragma once

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include "../compilation/gdscript_parser.h"

using namespace godot;

// Forward declaration
class GDScriptAnalyzer;

// Generates C code directly from GDScript AST
// This bypasses bytecode generation entirely, avoiding VM-specific types
class GDScriptASTCCodeGenerator {
public:
	GDScriptASTCCodeGenerator();
	~GDScriptASTCCodeGenerator();

	// Generate C code from a function AST node
	// Returns the generated C source code as a String
	// Returns empty String on error
	String generate_c_code(const GDScriptParser::FunctionNode *p_function, const GDScriptParser::ClassNode *p_class, GDScriptAnalyzer *p_analyzer);

	// Get the generated C code
	String get_generated_code() const { return generated_code; }

	// Check if generation was successful
	bool is_valid() const { return !generated_code.is_empty(); }

	// Get any errors that occurred during generation
	String get_error() const { return error_message; }

private:
	String generated_code;
	String error_message;

	// Internal state
	const GDScriptParser::FunctionNode *current_function = nullptr;
	const GDScriptParser::ClassNode *current_class = nullptr;
	GDScriptAnalyzer *analyzer = nullptr;
	HashMap<StringName, String> variable_names; // Maps GDScript variable names to C variable names
	int temp_counter = 0;

	// Generate function signature in C
	void generate_function_signature(const GDScriptParser::FunctionNode *p_function, String &r_code);

	// Generate function body from AST
	void generate_function_body(const GDScriptParser::FunctionNode *p_function, String &r_code);

	// Generate code for a statement node
	void generate_statement(const GDScriptParser::Node *p_statement, String &r_code, int p_indent = 0);

	// Generate code for an expression node
	String generate_expression(const GDScriptParser::Node *p_expression);

	// Generate code for variable declaration
	void generate_variable_declaration(const GDScriptParser::VariableNode *p_variable, String &r_code, int p_indent = 0);

	// Generate code for assignment
	void generate_assignment(const GDScriptParser::AssignmentNode *p_assignment, String &r_code, int p_indent = 0);

	// Generate code for if statement
	void generate_if_statement(const GDScriptParser::IfNode *p_if, String &r_code, int p_indent = 0);

	// Generate code for for loop
	void generate_for_loop(const GDScriptParser::ForNode *p_for, String &r_code, int p_indent = 0);

	// Generate code for while loop
	void generate_while_loop(const GDScriptParser::WhileNode *p_while, String &r_code, int p_indent = 0);

	// Generate code for match statement
	void generate_match_statement(const GDScriptParser::MatchNode *p_match, String &r_code, int p_indent = 0);

	// Generate code for return statement
	void generate_return_statement(const GDScriptParser::ReturnNode *p_return, String &r_code, int p_indent = 0);

	// Generate code for function call
	String generate_function_call(const GDScriptParser::CallNode *p_call);

	// Generate code for method call
	String generate_method_call(const GDScriptParser::CallNode *p_call);

	// Generate code for operator (BinaryOpNode, UnaryOpNode, or TernaryOpNode)
	String generate_operator(const GDScriptParser::Node *p_operator);

	// Generate code for identifier
	String generate_identifier(const GDScriptParser::IdentifierNode *p_identifier);

	// Generate code for literal
	String generate_literal(const GDScriptParser::Node *p_literal);

	// Generate code for array access
	String generate_subscript(const GDScriptParser::SubscriptNode *p_subscript);

	// Generate code for member access
	String generate_member_access(const GDScriptParser::Node *p_node);

	// Generate type conversion
	String generate_type_conversion(const GDScriptParser::DataType &p_from, const GDScriptParser::DataType &p_to, const String &p_expression);

	// Generate syscall for Godot API access
	String generate_syscall(const String &p_function_name, const Vector<String> &p_args);

	// Helper: Get C type name from GDScript type
	String get_c_type_name(const GDScriptParser::DataType &p_type);

	// Helper: Get C variable name for GDScript identifier
	String get_c_variable_name(const StringName &p_name);

	// Helper: Generate temporary variable name
	String generate_temp_var();

	// Helper: Generate indentation
	String indent(int p_level);

	// Helper: Sanitize identifier for C
	String sanitize_identifier(const String &p_name);
};
