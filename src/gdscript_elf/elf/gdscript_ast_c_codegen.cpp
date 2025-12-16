#if 0
// AST-to-C code generation disabled - preserved for future reconnection
// TODO: Re-enable when AST-to-C generation path is reconnected
/**************************************************************************/
/*  gdscript_ast_c_codegen.cpp                                            */
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

#include "gdscript_ast_c_codegen.h"

#include "../compilation/gdscript_parser.h"
#include "../compilation/gdscript_analyzer.h"

#include <godot_cpp/core/string_utils.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/char_string.hpp>

using namespace godot;

// Helper function for integer to string conversion
static inline String itos(int64_t p_val) {
	return String::num_int64(p_val);
}

GDScriptASTCCodeGenerator::GDScriptASTCCodeGenerator() {
	generated_code.clear();
	error_message.clear();
	temp_counter = 0;
}

GDScriptASTCCodeGenerator::~GDScriptASTCCodeGenerator() {
	generated_code.clear();
	error_message.clear();
}

String GDScriptASTCCodeGenerator::generate_c_code(const GDScriptParser::FunctionNode *p_function, const GDScriptParser::ClassNode *p_class, GDScriptAnalyzer *p_analyzer) {
	ERR_FAIL_NULL_V(p_function, String());
	ERR_FAIL_NULL_V(p_class, String());
	ERR_FAIL_NULL_V(p_analyzer, String());

	generated_code.clear();
	error_message.clear();
	temp_counter = 0;
	variable_names.clear();

	current_function = p_function;
	current_class = p_class;
	analyzer = p_analyzer;

	String code;

	// Generate includes and syscall definitions
	code += "#include <stdint.h>\n";
	code += "#include \"api.hpp\"\n\n";
	code += "#define GAME_API_BASE 500\n";
	code += "#define ECALL_VCALL (GAME_API_BASE + 1)\n";
	code += "#define ECALL_OBJ_PROP_GET (GAME_API_BASE + 45)\n";
	code += "#define ECALL_OBJ_PROP_SET (GAME_API_BASE + 46)\n\n";

	// Generate function signature
	generate_function_signature(p_function, code);
	code += " {\n";

	// Generate function body
	generate_function_body(p_function, code);

	code += "}\n";

	generated_code = code;
	return generated_code;
}

void GDScriptASTCCodeGenerator::generate_function_signature(const GDScriptParser::FunctionNode *p_function, String &r_code) {
	String func_name = p_function->identifier->name.operator String();
	func_name = sanitize_identifier(func_name);

	// Function signature: void gdscript_function_name(Variant** args, int argcount)
	r_code += "void gdscript_";
	r_code += func_name;
	r_code += "(Variant** args, int argcount)";
}

void GDScriptASTCCodeGenerator::generate_function_body(const GDScriptParser::FunctionNode *p_function, String &r_code) {
	// Extract result pointer
	r_code += "    Variant* result = args[0];\n";
	r_code += "    if (result == NULL) return;\n\n";

	// Extract parameters
	int param_count = p_function->parameters.size();
	if (param_count > 0) {
		r_code += "    // Extract parameters\n";
		for (int i = 0; i < param_count; i++) {
			const GDScriptParser::ParameterNode *param = p_function->parameters[i];
			String param_name = get_c_variable_name(param->identifier->name);
			r_code += "    Variant ";
			r_code += param_name;
			r_code += " = (argcount > ";
			r_code += itos(i + 1);
			r_code += ") ? *args[";
			r_code += itos(i + 1);
			r_code += "] : Variant();\n";
		}
		r_code += "\n";
	}

	// Generate statements
	if (p_function->body) {
		for (int i = 0; i < p_function->body->statements.size(); i++) {
			generate_statement(p_function->body->statements[i], r_code, 1);
		}
	}

	// Default return if no explicit return
	r_code += "    *result = Variant();\n";
}

void GDScriptASTCCodeGenerator::generate_statement(const GDScriptParser::Node *p_statement, String &r_code, int p_indent) {
	if (!p_statement) {
		return;
	}

	String indent_str = indent(p_indent);

	switch (p_statement->type) {
		case GDScriptParser::Node::VARIABLE: {
			generate_variable_declaration(static_cast<const GDScriptParser::VariableNode *>(p_statement), r_code, p_indent);
		} break;
		case GDScriptParser::Node::ASSIGNMENT: {
			generate_assignment(static_cast<const GDScriptParser::AssignmentNode *>(p_statement), r_code, p_indent);
		} break;
		case GDScriptParser::Node::IF: {
			generate_if_statement(static_cast<const GDScriptParser::IfNode *>(p_statement), r_code, p_indent);
		} break;
		case GDScriptParser::Node::FOR: {
			generate_for_loop(static_cast<const GDScriptParser::ForNode *>(p_statement), r_code, p_indent);
		} break;
		case GDScriptParser::Node::WHILE: {
			generate_while_loop(static_cast<const GDScriptParser::WhileNode *>(p_statement), r_code, p_indent);
		} break;
		case GDScriptParser::Node::RETURN: {
			generate_return_statement(static_cast<const GDScriptParser::ReturnNode *>(p_statement), r_code, p_indent);
		} break;
		case GDScriptParser::Node::EXPRESSION: {
			// Expression statement (function call, etc.)
			String expr = generate_expression(p_statement);
			r_code += indent_str;
			r_code += expr;
			r_code += ";\n";
		} break;
		default: {
			// Unsupported statement type - generate fallback
			r_code += indent_str;
			r_code += "// TODO: Unsupported statement type ";
			r_code += itos(p_statement->type);
			r_code += "\n";
		} break;
	}
}

String GDScriptASTCCodeGenerator::generate_expression(const GDScriptParser::Node *p_expression) {
	if (!p_expression) {
		return "Variant()";
	}

	switch (p_expression->type) {
		case GDScriptParser::Node::IDENTIFIER: {
			return generate_identifier(static_cast<const GDScriptParser::IdentifierNode *>(p_expression));
		}
		case GDScriptParser::Node::LITERAL: {
			return generate_literal(p_expression);
		}
		case GDScriptParser::Node::CALL: {
			return generate_function_call(static_cast<const GDScriptParser::CallNode *>(p_expression));
		}
		case GDScriptParser::Node::BINARY_OPERATOR: {
			return generate_operator(p_expression);
		}
		case GDScriptParser::Node::UNARY_OPERATOR: {
			return generate_operator(p_expression);
		}
		case GDScriptParser::Node::TERNARY_OPERATOR: {
			return generate_operator(p_expression);
		}
		case GDScriptParser::Node::SUBSCRIPT: {
			return generate_subscript(static_cast<const GDScriptParser::SubscriptNode *>(p_expression));
		}
		default: {
			return "Variant()"; // Fallback
		}
	}
}

void GDScriptASTCCodeGenerator::generate_variable_declaration(const GDScriptParser::VariableNode *p_variable, String &r_code, int p_indent) {
	String indent_str = indent(p_indent);
	String var_name = get_c_variable_name(p_variable->identifier->name);

	r_code += indent_str;
	r_code += "Variant ";
	r_code += var_name;

	if (p_variable->initializer) {
		r_code += " = ";
		r_code += generate_expression(p_variable->initializer);
	} else {
		r_code += " = Variant()";
	}

	r_code += ";\n";
}

void GDScriptASTCCodeGenerator::generate_assignment(const GDScriptParser::AssignmentNode *p_assignment, String &r_code, int p_indent) {
	String indent_str = indent(p_indent);
	String lhs = generate_expression(p_assignment->assignee);
	String rhs = generate_expression(p_assignment->assigned_value);

	r_code += indent_str;
	r_code += lhs;
	r_code += " = ";
	r_code += rhs;
	r_code += ";\n";
}

void GDScriptASTCCodeGenerator::generate_if_statement(const GDScriptParser::IfNode *p_if, String &r_code, int p_indent) {
	String indent_str = indent(p_indent);
	String condition = generate_expression(p_if->condition);

	r_code += indent_str;
	r_code += "if (";
	r_code += condition;
	r_code += ".operator bool()) {\n";

	// True block
	if (p_if->true_block) {
		for (int i = 0; i < p_if->true_block->statements.size(); i++) {
			generate_statement(p_if->true_block->statements[i], r_code, p_indent + 1);
		}
	}

	// False block
	if (p_if->false_block) {
		r_code += indent_str;
		r_code += "} else {\n";
		for (int i = 0; i < p_if->false_block->statements.size(); i++) {
			generate_statement(p_if->false_block->statements[i], r_code, p_indent + 1);
		}
	}

	r_code += indent_str;
	r_code += "}\n";
}

void GDScriptASTCCodeGenerator::generate_for_loop(const GDScriptParser::ForNode *p_for, String &r_code, int p_indent) {
	String indent_str = indent(p_indent);
	String iterator_name = get_c_variable_name(p_for->iterator->identifier->name);
	String container = generate_expression(p_for->list);

	r_code += indent_str;
	r_code += "// For loop: TODO - implement full for loop support\n";
	r_code += indent_str;
	r_code += "Variant ";
	r_code += iterator_name;
	r_code += " = ";
	r_code += container;
	r_code += ";\n";
	// TODO: Implement full for loop generation
}

void GDScriptASTCCodeGenerator::generate_while_loop(const GDScriptParser::WhileNode *p_while, String &r_code, int p_indent) {
	String indent_str = indent(p_indent);
	String condition = generate_expression(p_while->condition);

	r_code += indent_str;
	r_code += "while (";
	r_code += condition;
	r_code += ".operator bool()) {\n";

	if (p_while->loop_block) {
		for (int i = 0; i < p_while->loop_block->statements.size(); i++) {
			generate_statement(p_while->loop_block->statements[i], r_code, p_indent + 1);
		}
	}

	r_code += indent_str;
	r_code += "}\n";
}

void GDScriptASTCCodeGenerator::generate_match_statement(const GDScriptParser::MatchNode *p_match, String &r_code, int p_indent) {
	// TODO: Implement match statement
	r_code += indent(p_indent);
	r_code += "// TODO: Match statement not yet implemented\n";
}

void GDScriptASTCCodeGenerator::generate_return_statement(const GDScriptParser::ReturnNode *p_return, String &r_code, int p_indent) {
	String indent_str = indent(p_indent);
	r_code += indent_str;
	if (p_return->return_value) {
		String return_expr = generate_expression(p_return->return_value);
		r_code += "*result = ";
		r_code += return_expr;
		r_code += ";\n";
		r_code += indent_str;
		r_code += "return;\n";
	} else {
		r_code += "*result = Variant();\n";
		r_code += indent_str;
		r_code += "return;\n";
	}
}

String GDScriptASTCCodeGenerator::generate_function_call(const GDScriptParser::CallNode *p_call) {
	// Generate syscall for function calls
	String func_name = p_call->function_name.operator String();
	Vector<String> args;

	if (p_call->arguments.size() > 0) {
		for (int i = 0; i < p_call->arguments.size(); i++) {
			args.push_back(generate_expression(p_call->arguments[i]));
		}
	}

	return generate_syscall(func_name, args);
}

String GDScriptASTCCodeGenerator::generate_method_call(const GDScriptParser::CallNode *p_call) {
	// Similar to function call but for method calls
	return generate_function_call(p_call);
}

String GDScriptASTCCodeGenerator::generate_operator(const GDScriptParser::Node *p_operator) {
	// Handle different operator node types
	if (p_operator->type == GDScriptParser::Node::BINARY_OPERATOR) {
		const GDScriptParser::BinaryOpNode *bin_op = static_cast<const GDScriptParser::BinaryOpNode *>(p_operator);
		if (!bin_op->left_operand || !bin_op->right_operand) {
			return "Variant()";
		}

		String left = generate_expression(bin_op->left_operand);
		String right = generate_expression(bin_op->right_operand);

		// Map GDScript operators to C++ operators
		String op;
		switch (bin_op->operation) {
			case GDScriptParser::BinaryOpNode::OP_ADDITION:
				op = "+";
				break;
			case GDScriptParser::BinaryOpNode::OP_SUBTRACTION:
				op = "-";
				break;
			case GDScriptParser::BinaryOpNode::OP_MULTIPLICATION:
				op = "*";
				break;
			case GDScriptParser::BinaryOpNode::OP_DIVISION:
				op = "/";
				break;
			case GDScriptParser::BinaryOpNode::OP_COMP_EQUAL:
				return "(" + left + " == " + right + ") ? Variant(true) : Variant(false)";
			case GDScriptParser::BinaryOpNode::OP_COMP_NOT_EQUAL:
				return "(" + left + " != " + right + ") ? Variant(false) : Variant(true)";
			default:
				// Use syscall for complex operators
				return generate_syscall("operator_" + itos(bin_op->operation), Vector<String>({ left, right }));
	}

		return "(" + left + " " + op + " " + right + ")";
	} else if (p_operator->type == GDScriptParser::Node::UNARY_OPERATOR) {
		const GDScriptParser::UnaryOpNode *unary_op = static_cast<const GDScriptParser::UnaryOpNode *>(p_operator);
		if (!unary_op->operand) {
			return "Variant()";
		}
		String operand = generate_expression(unary_op->operand);
		// TODO: Handle unary operators properly
		return "Variant()"; // Placeholder
	} else if (p_operator->type == GDScriptParser::Node::TERNARY_OPERATOR) {
		const GDScriptParser::TernaryOpNode *ternary_op = static_cast<const GDScriptParser::TernaryOpNode *>(p_operator);
		if (!ternary_op->condition || !ternary_op->true_expr || !ternary_op->false_expr) {
			return "Variant()";
		}
		String condition = generate_expression(ternary_op->condition);
		String true_expr = generate_expression(ternary_op->true_expr);
		String false_expr = generate_expression(ternary_op->false_expr);
		return "(" + condition + ".operator bool() ? " + true_expr + " : " + false_expr + ")";
	}

	return "Variant()"; // Fallback
}

String GDScriptASTCCodeGenerator::generate_identifier(const GDScriptParser::IdentifierNode *p_identifier) {
	return get_c_variable_name(p_identifier->name);
}

String GDScriptASTCCodeGenerator::generate_literal(const GDScriptParser::Node *p_literal) {
	// TODO: Handle different literal types
	return "Variant()"; // Placeholder
}

String GDScriptASTCCodeGenerator::generate_subscript(const GDScriptParser::SubscriptNode *p_subscript) {
	String base = generate_expression(p_subscript->base);
	String index = generate_expression(p_subscript->index);
	return base + "[" + index + "]";
}

String GDScriptASTCCodeGenerator::generate_member_access(const GDScriptParser::Node *p_node) {
	// TODO: Implement member access
	return "Variant()";
}

String GDScriptASTCCodeGenerator::generate_syscall(const String &p_function_name, const Vector<String> &p_args) {
	String call = "syscall_vcall(\"" + p_function_name + "\"";
	for (int i = 0; i < p_args.size(); i++) {
		call += ", " + p_args[i];
	}
	call += ")";
	return call;
}

String GDScriptASTCCodeGenerator::get_c_type_name(const GDScriptParser::DataType &p_type) {
	// Map GDScript types to C types
	switch (p_type.builtin_type) {
		case Variant::BOOL:
			return "bool";
		case Variant::INT:
			return "int64_t";
		case Variant::FLOAT:
			return "double";
		case Variant::STRING:
			return "String";
		default:
			return "Variant";
	}
}

String GDScriptASTCCodeGenerator::get_c_variable_name(const StringName &p_name) {
	if (variable_names.has(p_name)) {
		return variable_names[p_name];
	}
	String sanitized = sanitize_identifier(p_name.operator String());
	variable_names[p_name] = sanitized;
	return sanitized;
}

#endif // #if 0 - AST-to-C code generation disabled

String GDScriptASTCCodeGenerator::generate_temp_var() {
	return "temp_" + itos(temp_counter++);
}

String GDScriptASTCCodeGenerator::indent(int p_level) {
	String result;
	for (int i = 0; i < p_level; i++) {
		result += "    ";
	}
	return result;
}

String GDScriptASTCCodeGenerator::sanitize_identifier(const String &p_name) {
	String result = p_name;
	result = result.replace(".", "_");
	result = result.replace(" ", "_");
	result = result.replace("-", "_");
	// Ensure it starts with a letter or underscore
	if (result.length() > 0 && !((result[0] >= 'a' && result[0] <= 'z') || (result[0] >= 'A' && result[0] <= 'Z') || result[0] == '_')) {
		result = "_" + result;
	}
	return result;
}
