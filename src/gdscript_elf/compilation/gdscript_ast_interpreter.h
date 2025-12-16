/**************************************************************************/
/*  gdscript_ast_interpreter.h                                            */
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

#include "../compilation/gdscript_parser.h"
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/string_name.hpp>

using namespace godot;

class GDScriptELFInstance;
class GDScriptAnalyzer;

// AST Node Handler - returns pointer to next handler (Nostradamus Distributor pattern)
// Based on: http://www.emulators.com/docs/nx25_nostradamus.htm
// Each handler executes a node and returns the next handler to call
typedef void* (*ASTNodeHandler)(void* interpreter, const GDScriptParser::Node* node, void* context);

// Fire escape handler - special handler that terminates trace execution
extern void* ast_fire_escape_handler;

// GDScriptASTInterpreter - Direct AST interpretation using Nostradamus Distributor
// This bypasses bytecode/VM entirely and interprets AST nodes directly
class GDScriptASTInterpreter {
public:
	GDScriptASTInterpreter();
	~GDScriptASTInterpreter();

	// Execute a function from AST
	// Returns the function's return value, or Variant() on error
	Variant execute_function(const GDScriptParser::FunctionNode *p_function,
	                         const GDScriptParser::ClassNode *p_class,
	                         GDScriptELFInstance *p_instance,
	                         const Variant **p_args,
	                         int p_argcount,
	                         GDExtensionCallError &r_error);

	// Execute a statement node (using Nostradamus Distributor)
	// Returns pointer to next handler (or fire_escape_handler to terminate)
	ASTNodeHandler execute_statement(const GDScriptParser::Node *p_statement);

	// Execute an expression node
	// Returns the expression's value
	Variant execute_expression(const GDScriptParser::Node *p_expression);

	// Get current return value (set by return statement)
	Variant get_return_value() const { return return_value; }
	bool has_returned() const { return has_return_value; }

	// Reset interpreter state for new function execution
	void reset();

private:
	// Interpreter state
	GDScriptELFInstance *instance = nullptr;
	const GDScriptParser::FunctionNode *current_function = nullptr;
	const GDScriptParser::ClassNode *current_class = nullptr;
	GDScriptAnalyzer *analyzer = nullptr;

	// Execution stack and locals
	HashMap<StringName, Variant> locals; // Local variables
	Vector<Variant> stack; // Expression evaluation stack
	Variant return_value; // Function return value
	bool has_return_value = false;

	// Control flow state
	bool should_break = false;
	bool should_continue = false;
	
	// Nostradamus Distributor state - next node prefetched by handlers
	const GDScriptParser::Node* next_node = nullptr;
	int current_stmt_index = 0; // Current statement index in function body

	// Handler lookup - maps node type to handler function
	ASTNodeHandler get_handler(const GDScriptParser::Node *p_node);

	// Node execution methods (called by handlers)
	void execute_variable_declaration(const GDScriptParser::VariableNode *p_variable);
	void execute_assignment(const GDScriptParser::AssignmentNode *p_assignment);
	void execute_if_statement(const GDScriptParser::IfNode *p_if);
	void execute_for_loop(const GDScriptParser::ForNode *p_for);
	void execute_while_loop(const GDScriptParser::WhileNode *p_while);
	void execute_return_statement(const GDScriptParser::ReturnNode *p_return);
	Variant execute_call(const GDScriptParser::CallNode *p_call);
	Variant execute_binary_operator(const GDScriptParser::BinaryOpNode *p_op);
	Variant execute_unary_operator(const GDScriptParser::UnaryOpNode *p_op);
	Variant execute_identifier(const GDScriptParser::IdentifierNode *p_identifier);
	Variant execute_literal(const GDScriptParser::LiteralNode *p_literal);

	// Helper: Get variable value
	Variant get_variable(const StringName &p_name);
	
	// Helper: Set variable value
	void set_variable(const StringName &p_name, const Variant &p_value);

	// Helper: Get next statement in suite
	const GDScriptParser::Node* get_next_statement(const GDScriptParser::Node *p_current);
};
