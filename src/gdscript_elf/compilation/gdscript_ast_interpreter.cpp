/**************************************************************************/
/*  gdscript_ast_interpreter.cpp                                          */
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

#include "gdscript_ast_interpreter.h"
#include "../language/gdscript_elf_instance.h"
#include "gdscript_analyzer.h"
#include <godot_cpp/core/error_macros.hpp>

using namespace godot;

// Fire escape handler - terminates trace execution
void* ast_fire_escape_handler = (void*)0x1; // Non-null pointer to identify fire escape

// Handler implementations using Nostradamus Distributor pattern
// Each handler executes the node and returns pointer to next handler

static void* handle_variable(void* interpreter_ptr, const GDScriptParser::Node* node, void* context) {
	GDScriptASTInterpreter* interpreter = static_cast<GDScriptASTInterpreter*>(interpreter_ptr);
	const GDScriptParser::VariableNode* var_node = static_cast<const GDScriptParser::VariableNode*>(node);
	interpreter->execute_variable_declaration(var_node);
	
	// Software pipelining: prefetch and return next handler
	// This is the key to Nostradamus Distributor - handlers return next handler
	const GDScriptParser::Node* next = interpreter->get_next_statement(node);
	if (!next) {
		return ast_fire_escape_handler; // End of trace
	}
	ASTNodeHandler next_handler = interpreter->get_handler(next);
	return (void*)next_handler;
}

static void* handle_assignment(void* interpreter_ptr, const GDScriptParser::Node* node, void* context) {
	GDScriptASTInterpreter* interpreter = static_cast<GDScriptASTInterpreter*>(interpreter_ptr);
	const GDScriptParser::AssignmentNode* assign_node = static_cast<const GDScriptParser::AssignmentNode*>(node);
	interpreter->execute_assignment(assign_node);
	
	const GDScriptParser::Node* next = interpreter->get_next_statement(node);
	if (!next) {
		return ast_fire_escape_handler;
	}
	ASTNodeHandler next_handler = interpreter->get_handler(next);
	return (void*)next_handler;
}

static void* handle_if(void* interpreter_ptr, const GDScriptParser::Node* node, void* context) {
	GDScriptASTInterpreter* interpreter = static_cast<GDScriptASTInterpreter*>(interpreter_ptr);
	const GDScriptParser::IfNode* if_node = static_cast<const GDScriptParser::IfNode*>(node);
	interpreter->execute_if_statement(if_node);
	
	const GDScriptParser::Node* next = interpreter->get_next_statement(node);
	if (!next) {
		return ast_fire_escape_handler;
	}
	ASTNodeHandler next_handler = interpreter->get_handler(next);
	return (void*)next_handler;
}

static void* handle_for(void* interpreter_ptr, const GDScriptParser::Node* node, void* context) {
	GDScriptASTInterpreter* interpreter = static_cast<GDScriptASTInterpreter*>(interpreter_ptr);
	const GDScriptParser::ForNode* for_node = static_cast<const GDScriptParser::ForNode*>(node);
	interpreter->execute_for_loop(for_node);
	
	const GDScriptParser::Node* next = interpreter->get_next_statement(node);
	if (!next) {
		return ast_fire_escape_handler;
	}
	ASTNodeHandler next_handler = interpreter->get_handler(next);
	return (void*)next_handler;
}

static void* handle_while(void* interpreter_ptr, const GDScriptParser::Node* node, void* context) {
	GDScriptASTInterpreter* interpreter = static_cast<GDScriptASTInterpreter*>(interpreter_ptr);
	const GDScriptParser::WhileNode* while_node = static_cast<const GDScriptParser::WhileNode*>(node);
	interpreter->execute_while_loop(while_node);
	
	const GDScriptParser::Node* next = interpreter->get_next_statement(node);
	if (!next) {
		return ast_fire_escape_handler;
	}
	ASTNodeHandler next_handler = interpreter->get_handler(next);
	return (void*)next_handler;
}

static void* handle_return(void* interpreter_ptr, const GDScriptParser::Node* node, void* context) {
	GDScriptASTInterpreter* interpreter = static_cast<GDScriptASTInterpreter*>(interpreter_ptr);
	const GDScriptParser::ReturnNode* return_node = static_cast<const GDScriptParser::ReturnNode*>(node);
	interpreter->execute_return_statement(return_node);
	// Return always terminates execution
	return ast_fire_escape_handler;
}

static void* handle_break(void* interpreter_ptr, const GDScriptParser::Node* node, void* context) {
	// Break terminates current loop
	GDScriptASTInterpreter* interpreter = static_cast<GDScriptASTInterpreter*>(interpreter_ptr);
	interpreter->should_break = true;
	return ast_fire_escape_handler;
}

static void* handle_continue(void* interpreter_ptr, const GDScriptParser::Node* node, void* context) {
	// Continue jumps to loop condition
	GDScriptASTInterpreter* interpreter = static_cast<GDScriptASTInterpreter*>(interpreter_ptr);
	interpreter->should_continue = true;
	return ast_fire_escape_handler;
}

GDScriptASTInterpreter::GDScriptASTInterpreter() {
	instance = nullptr;
	current_function = nullptr;
	current_class = nullptr;
	analyzer = nullptr;
	has_return_value = false;
	should_break = false;
	should_continue = false;
}

GDScriptASTInterpreter::~GDScriptASTInterpreter() {
	reset();
}

void GDScriptASTInterpreter::reset() {
	locals.clear();
	stack.clear();
	return_value = Variant();
	has_return_value = false;
	should_break = false;
	should_continue = false;
	instance = nullptr;
	current_function = nullptr;
	current_class = nullptr;
	analyzer = nullptr;
	next_node = nullptr;
	current_stmt_index = 0;
}

Variant GDScriptASTInterpreter::execute_function(const GDScriptParser::FunctionNode *p_function,
                                                 const GDScriptParser::ClassNode *p_class,
                                                 GDScriptELFInstance *p_instance,
                                                 const Variant **p_args,
                                                 int p_argcount,
                                                 GDExtensionCallError &r_error) {
	reset();
	
	current_function = p_function;
	current_class = p_class;
	instance = p_instance;
	
	// Get analyzer from script if needed
	// analyzer will be set by caller if needed
	
	// Initialize parameters
	if (p_function->parameters.size() > 0) {
		int param_count = p_function->parameters.size();
		for (int i = 0; i < param_count && i < p_argcount; i++) {
			if (p_function->parameters[i] && p_function->parameters[i]->identifier) {
				locals[p_function->parameters[i]->identifier->name] = *p_args[i];
			}
		}
		// Fill in default arguments
		for (int i = p_argcount; i < param_count; i++) {
			int default_idx = i - (param_count - p_function->default_arg_values.size());
			if (default_idx >= 0 && default_idx < p_function->default_arg_values.size()) {
				if (p_function->parameters[i] && p_function->parameters[i]->identifier) {
					locals[p_function->parameters[i]->identifier->name] = p_function->default_arg_values[default_idx];
				}
			}
		}
	}
	
	// Execute function body using Nostradamus Distributor
	// Uses nested if statements (not loops) for better branch prediction
	// Based on: http://www.emulators.com/docs/nx25_nostradamus.htm
	if (p_function->body && p_function->body->statements.size() > 0) {
		// Start with first statement - handlers prefetch and return next handler
		current_stmt_index = 0;
		const GDScriptParser::Node* current_node = p_function->body->statements[0];
		ASTNodeHandler handler = get_handler(current_node);
		
		// Execute first handler - it returns next handler (software pipelining)
		// Handlers prefetch the next handler and return it
		handler = (ASTNodeHandler)(*handler)(this, current_node, nullptr);
		
		// Nostradamus Distributor: nested if statements instead of loops
		// Macro generates nested if statements with 16-byte spacing for branch prediction
		// Each handler returns the next handler (already prefetched with next node)
		#define NOSTRADAMUS_NEXT() \
			if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) { \
				if (current_stmt_index + 1 < p_function->body->statements.size()) { \
					current_node = p_function->body->statements[++current_stmt_index]; \
					handler = get_handler(current_node); \
					handler = (ASTNodeHandler)(*handler)(this, current_node, nullptr); \
				} else { \
					handler = (ASTNodeHandler)ast_fire_escape_handler; \
				} \
			}
		
		// Generate nested if statements using macro (up to 32 levels for max trace length)
		// Each level provides padding for better branch prediction (16-byte spacing)
		NOSTRADAMUS_NEXT()
		if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
			NOSTRADAMUS_NEXT()
			if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
				NOSTRADAMUS_NEXT()
				if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
					NOSTRADAMUS_NEXT()
					if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
						NOSTRADAMUS_NEXT()
						if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
							NOSTRADAMUS_NEXT()
							if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
								NOSTRADAMUS_NEXT()
								if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
									NOSTRADAMUS_NEXT()
									if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
										NOSTRADAMUS_NEXT()
										if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
											NOSTRADAMUS_NEXT()
											if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
												NOSTRADAMUS_NEXT()
												if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
													NOSTRADAMUS_NEXT()
													if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
														NOSTRADAMUS_NEXT()
														if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
															NOSTRADAMUS_NEXT()
															if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																NOSTRADAMUS_NEXT()
																if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																	NOSTRADAMUS_NEXT()
																	if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																		NOSTRADAMUS_NEXT()
																		if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																			NOSTRADAMUS_NEXT()
																			if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																				NOSTRADAMUS_NEXT()
																				if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																					NOSTRADAMUS_NEXT()
																					if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																						NOSTRADAMUS_NEXT()
																						if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																							NOSTRADAMUS_NEXT()
																							if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																								NOSTRADAMUS_NEXT()
																								if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																									NOSTRADAMUS_NEXT()
																									if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																										NOSTRADAMUS_NEXT()
																										if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																											NOSTRADAMUS_NEXT()
																											if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																												NOSTRADAMUS_NEXT()
																												if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																													NOSTRADAMUS_NEXT()
																													if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																														NOSTRADAMUS_NEXT()
																														if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																															NOSTRADAMUS_NEXT()
																															if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																																NOSTRADAMUS_NEXT()
																																if (handler != (ASTNodeHandler)ast_fire_escape_handler && !has_return_value) {
																																	NOSTRADAMUS_NEXT()
																																}
																															}
																														}
																													}
																												}
																											}
																										}
																									}
																								}
																							}
																						}
																					}
																				}
																			}
																		}
																	}
																}
															}
														}
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
		
		#undef NOSTRADAMUS_NEXT
	}
	
	if (has_return_value) {
		return return_value;
	}
	
	return Variant(); // Default return value
}

ASTNodeHandler GDScriptASTInterpreter::execute_statement(const GDScriptParser::Node *p_statement) {
	ASTNodeHandler handler = get_handler(p_statement);
	if (handler) {
		return (ASTNodeHandler)(*handler)(this, p_statement, nullptr);
	}
	return (ASTNodeHandler)ast_fire_escape_handler;
}

Variant GDScriptASTInterpreter::execute_expression(const GDScriptParser::Node *p_expression) {
	if (!p_expression) {
		return Variant();
	}
	
	switch (p_expression->type) {
		case GDScriptParser::Node::IDENTIFIER: {
			const GDScriptParser::IdentifierNode* id_node = static_cast<const GDScriptParser::IdentifierNode*>(p_expression);
			return execute_identifier(id_node);
		}
		case GDScriptParser::Node::LITERAL: {
			const GDScriptParser::LiteralNode* lit_node = static_cast<const GDScriptParser::LiteralNode*>(p_expression);
			return execute_literal(lit_node);
		}
		case GDScriptParser::Node::CALL: {
			const GDScriptParser::CallNode* call_node = static_cast<const GDScriptParser::CallNode*>(p_expression);
			return execute_call(call_node);
		}
		case GDScriptParser::Node::BINARY_OPERATOR: {
			const GDScriptParser::BinaryOpNode* bin_op = static_cast<const GDScriptParser::BinaryOpNode*>(p_expression);
			return execute_binary_operator(bin_op);
		}
		case GDScriptParser::Node::UNARY_OPERATOR: {
			const GDScriptParser::UnaryOpNode* unary_op = static_cast<const GDScriptParser::UnaryOpNode*>(p_expression);
			return execute_unary_operator(unary_op);
		}
		default:
			return Variant();
	}
}

ASTNodeHandler GDScriptASTInterpreter::get_handler(const GDScriptParser::Node *p_node) {
	if (!p_node) {
		return (ASTNodeHandler)ast_fire_escape_handler;
	}
	
	switch (p_node->type) {
		case GDScriptParser::Node::VARIABLE:
			return handle_variable;
		case GDScriptParser::Node::ASSIGNMENT:
			return handle_assignment;
		case GDScriptParser::Node::IF:
			return handle_if;
		case GDScriptParser::Node::FOR:
			return handle_for;
		case GDScriptParser::Node::WHILE:
			return handle_while;
		case GDScriptParser::Node::RETURN:
			return handle_return;
		case GDScriptParser::Node::BREAK:
			return handle_break;
		case GDScriptParser::Node::CONTINUE:
			return handle_continue;
		default:
			// Unknown node type - terminate trace
			return (ASTNodeHandler)ast_fire_escape_handler;
	}
}

const GDScriptParser::Node* GDScriptASTInterpreter::get_next_statement(const GDScriptParser::Node *p_current) {
	if (!p_current || !current_function || !current_function->body) {
		return nullptr;
	}
	
	// Find current statement in body's statements list
	const GDScriptParser::SuiteNode* suite = current_function->body;
	for (int i = 0; i < suite->statements.size(); i++) {
		if (suite->statements[i] == p_current) {
			// Return next statement
			if (i + 1 < suite->statements.size()) {
				return suite->statements[i + 1];
			}
			break;
		}
	}
	
	return nullptr;
}

void GDScriptASTInterpreter::execute_variable_declaration(const GDScriptParser::VariableNode *p_variable) {
	if (!p_variable || !p_variable->identifier) {
		return;
	}
	
	StringName var_name = p_variable->identifier->name;
	Variant initial_value = Variant();
	
	if (p_variable->initializer) {
		initial_value = execute_expression(p_variable->initializer);
	}
	
	locals[var_name] = initial_value;
}

void GDScriptASTInterpreter::execute_assignment(const GDScriptParser::AssignmentNode *p_assignment) {
	if (!p_assignment || !p_assignment->assignee) {
		return;
	}
	
	Variant value = execute_expression(p_assignment->assigned_value);
	
	// Handle assignment target
	if (p_assignment->assignee->type == GDScriptParser::Node::IDENTIFIER) {
		const GDScriptParser::IdentifierNode* id_node = static_cast<const GDScriptParser::IdentifierNode*>(p_assignment->assignee);
		set_variable(id_node->name, value);
	}
	// TODO: Handle other assignment targets (subscript, member access, etc.)
}

void GDScriptASTInterpreter::execute_if_statement(const GDScriptParser::IfNode *p_if) {
	if (!p_if || !p_if->condition) {
		return;
	}
	
	Variant condition_value = execute_expression(p_if->condition);
	bool condition = condition_value.operator bool();
	
	if (condition && p_if->true_block) {
		// Execute true block
		for (int i = 0; i < p_if->true_block->statements.size(); i++) {
			execute_statement(p_if->true_block->statements[i]);
			if (has_return_value || should_break || should_continue) {
				break;
			}
		}
	} else if (!condition && p_if->false_block) {
		// Execute false block
		for (int i = 0; i < p_if->false_block->statements.size(); i++) {
			execute_statement(p_if->false_block->statements[i]);
			if (has_return_value || should_break || should_continue) {
				break;
			}
		}
	}
}

void GDScriptASTInterpreter::execute_for_loop(const GDScriptParser::ForNode *p_for) {
	if (!p_for || !p_for->list || !p_for->variable) {
		return;
	}
	
	Variant iterable = execute_expression(p_for->list);
	should_break = false;
	should_continue = false;
	
	// TODO: Handle different iterable types (Array, Dictionary, Range, etc.)
	// For now, assume Array
	if (iterable.get_type() == Variant::ARRAY) {
		Array arr = iterable;
		for (int i = 0; i < arr.size(); i++) {
			set_variable(p_for->variable->name, arr[i]);
			
			if (p_for->loop) {
				for (int j = 0; j < p_for->loop->statements.size(); j++) {
					execute_statement(p_for->loop->statements[j]);
					if (has_return_value || should_break) {
						break;
					}
					if (should_continue) {
						should_continue = false;
						continue;
					}
				}
			}
			
			if (has_return_value || should_break) {
				break;
			}
		}
	}
	
	should_break = false;
}

void GDScriptASTInterpreter::execute_while_loop(const GDScriptParser::WhileNode *p_while) {
	if (!p_while || !p_while->condition) {
		return;
	}
	
	should_break = false;
	should_continue = false;
	
	while (true) {
		Variant condition_value = execute_expression(p_while->condition);
		if (!condition_value.operator bool()) {
			break;
		}
		
		if (p_while->loop_block) {
			for (int i = 0; i < p_while->loop_block->statements.size(); i++) {
				execute_statement(p_while->loop_block->statements[i]);
				if (has_return_value || should_break) {
					break;
				}
				if (should_continue) {
					should_continue = false;
					break; // Jump to condition check
				}
			}
		}
		
		if (has_return_value || should_break) {
			break;
		}
	}
	
	should_break = false;
}

void GDScriptASTInterpreter::execute_return_statement(const GDScriptParser::ReturnNode *p_return) {
	if (p_return && p_return->return_value) {
		return_value = execute_expression(p_return->return_value);
	} else {
		return_value = Variant();
	}
	has_return_value = true;
}

Variant GDScriptASTInterpreter::execute_call(const GDScriptParser::CallNode *p_call) {
	if (!p_call || !p_call->callee) {
		return Variant();
	}
	
	// TODO: Implement function calls
	// For now, return empty variant
	return Variant();
}

Variant GDScriptASTInterpreter::execute_binary_operator(const GDScriptParser::BinaryOpNode *p_op) {
	if (!p_op || !p_op->left_operand || !p_op->right_operand) {
		return Variant();
	}
	
	Variant left = execute_expression(p_op->left_operand);
	Variant right = execute_expression(p_op->right_operand);
	
	// TODO: Implement all binary operators
	// For now, handle basic arithmetic
	switch (p_op->operation) {
		case GDScriptParser::BinaryOpNode::OP_ADDITION:
			return left.operator Variant() + right.operator Variant();
		case GDScriptParser::BinaryOpNode::OP_SUBTRACTION:
			return left.operator Variant() - right.operator Variant();
		case GDScriptParser::BinaryOpNode::OP_MULTIPLICATION:
			return left.operator Variant() * right.operator Variant();
		case GDScriptParser::BinaryOpNode::OP_DIVISION:
			return left.operator Variant() / right.operator Variant();
		case GDScriptParser::BinaryOpNode::OP_COMP_EQUAL:
			return Variant(left == right);
		case GDScriptParser::BinaryOpNode::OP_COMP_NOT_EQUAL:
			return Variant(left != right);
		default:
			return Variant();
	}
}

Variant GDScriptASTInterpreter::execute_unary_operator(const GDScriptParser::UnaryOpNode *p_op) {
	if (!p_op || !p_op->operand) {
		return Variant();
	}
	
	Variant operand = execute_expression(p_op->operand);
	
	// TODO: Implement unary operators
	switch (p_op->operation) {
		case GDScriptParser::UnaryOpNode::OP_NEGATIVE:
			return -operand.operator Variant();
		case GDScriptParser::UnaryOpNode::OP_LOGIC_NOT:
			return Variant(!operand.operator bool());
		default:
			return Variant();
	}
}

Variant GDScriptASTInterpreter::execute_identifier(const GDScriptParser::IdentifierNode *p_identifier) {
	if (!p_identifier) {
		return Variant();
	}
	
	return get_variable(p_identifier->name);
}

Variant GDScriptASTInterpreter::execute_literal(const GDScriptParser::LiteralNode *p_literal) {
	if (!p_literal) {
		return Variant();
	}
	
	// Return the literal's value (reduced_value is set during analysis)
	if (p_literal->reduced_value.get_type() != Variant::NIL) {
		return p_literal->reduced_value;
	}
	
	// Fallback to value if reduced_value not available
	return p_literal->value;
}

Variant GDScriptASTInterpreter::get_variable(const StringName &p_name) {
	// Check locals first
	if (locals.has(p_name)) {
		return locals[p_name];
	}
	
	// TODO: Check instance members, class members, globals, etc.
	
	return Variant();
}

void GDScriptASTInterpreter::set_variable(const StringName &p_name, const Variant &p_value) {
	locals[p_name] = p_value;
	// TODO: Handle instance members, class members, etc.
}
