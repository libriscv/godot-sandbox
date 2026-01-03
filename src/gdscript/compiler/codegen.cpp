#include "codegen.h"
#include <stdexcept>
#include <sstream>
#include <cstring>

namespace gdscript {

// Helper function to convert type hint string to Variant::Type
static IRInstruction::TypeHint type_hint_from_string(const std::string& type_str) {
	if (type_str == "int") {
		return Variant::INT;
	} else if (type_str == "float") {
		return Variant::FLOAT;
	} else if (type_str == "bool") {
		return Variant::BOOL;
	} else if (type_str == "String") {
		return Variant::STRING;
	} else if (type_str == "Vector2") {
		return Variant::VECTOR2;
	} else if (type_str == "Vector2i") {
		return Variant::VECTOR2I;
	} else if (type_str == "Vector3") {
		return Variant::VECTOR3;
	} else if (type_str == "Vector3i") {
		return Variant::VECTOR3I;
	} else if (type_str == "Vector4") {
		return Variant::VECTOR4;
	} else if (type_str == "Vector4i") {
		return Variant::VECTOR4I;
	} else if (type_str == "Color") {
		return Variant::COLOR;
	}
	// Unknown type, return NONE
	return IRInstruction::TypeHint_NONE;
}

CodeGenerator::CodeGenerator() {}

IRProgram CodeGenerator::generate(const Program& program) {
	IRProgram ir_program;

	// Collect all locally defined function names
	m_local_functions.clear();
	for (const auto& func : program.functions) {
		m_local_functions.insert(func.name);
	}

	// Process global variables
	m_global_variables.clear();
	for (size_t i = 0; i < program.globals.size(); i++) {
		const auto& global = program.globals[i];
		m_global_variables[global.name] = i;

		// Convert AST global to IR global
		IRGlobalVar ir_global;
		ir_global.name = global.name;
		ir_global.is_const = global.is_const;

		// Convert type hint
		if (!global.type_hint.empty()) {
			ir_global.type_hint = type_hint_from_string(global.type_hint);
		}

		// Validate that global variables have either a type hint or an initializer
		// This is necessary for complex types (String, Array, Dictionary, etc.) which
		// require VASSIGN for proper reference counting. Without type information, we
		// cannot determine at compile time whether VASSIGN is needed.
		if (global.type_hint.empty() && !global.initializer) {
			throw std::runtime_error(
				"Global variable '" + global.name + "' requires either a type hint or an initializer. " +
				"Please add ': type' (e.g., ': Array') or an initializer (e.g., '= []'). " +
				"This is required to ensure proper memory management for complex types."
			);
		}

		// Extract initializer value if it's a literal
		if (global.initializer) {
			if (auto* lit = dynamic_cast<const LiteralExpr*>(global.initializer.get())) {
				switch (lit->lit_type) {
					case LiteralExpr::Type::INTEGER:
						ir_global.init_type = IRGlobalVar::InitType::INT;
						ir_global.init_value = std::get<int64_t>(lit->value);
						break;
					case LiteralExpr::Type::FLOAT:
						ir_global.init_type = IRGlobalVar::InitType::FLOAT;
						ir_global.init_value = std::get<double>(lit->value);
						break;
					case LiteralExpr::Type::STRING:
						ir_global.init_type = IRGlobalVar::InitType::STRING;
						ir_global.init_value = std::get<std::string>(lit->value);
						break;
					case LiteralExpr::Type::BOOL:
						ir_global.init_type = IRGlobalVar::InitType::BOOL;
						ir_global.init_value = std::get<bool>(lit->value);
						break;
					case LiteralExpr::Type::NULL_VAL:
						ir_global.init_type = IRGlobalVar::InitType::NULL_VAL;
						break;
				}
			} else if (auto* array_lit = dynamic_cast<const ArrayLiteralExpr*>(global.initializer.get())) {
				// Support empty array literals as global initializers
				if (array_lit->elements.empty()) {
					ir_global.init_type = IRGlobalVar::InitType::EMPTY_ARRAY;
				} else {
					// Non-empty arrays would require complex initialization
					// For now, leave as NONE (NIL) - can be improved later
				}
			}
			// For other non-literal initializers, we'll need to generate initialization code
			// This would be done in a special initialization function or at first access
		}

		ir_program.globals.push_back(ir_global);
	}

	for (const auto& func : program.functions) {
		ir_program.functions.push_back(generate_function(func));
	}

	ir_program.string_constants = m_string_constants;
	return ir_program;
}

IRFunction CodeGenerator::generate_function(const FunctionDecl& func) {
	IRFunction ir_func;
	ir_func.name = func.name;

	// Reset state for new function
	m_scope_stack.clear();
	m_next_register = 0;
	m_loop_stack.clear();

	// Create root scope for function
	push_scope();

	// Parameters are passed in registers a0-a7 (RISC-V convention)
	// For simplicity, we'll store them as variables immediately
	for (size_t i = 0; i < func.parameters.size(); i++) {
		const auto& param = func.parameters[i];
		ir_func.parameters.push_back(param.name);

		int reg = alloc_register();
		// In real implementation, would load from parameter registers
		// For now, assume parameters are already in variables
		declare_variable(param.name, reg);

		// Track parameter type if type hint is present
		if (!param.type_hint.empty()) {
			IRInstruction::TypeHint type = type_hint_from_string(param.type_hint);
			if (type != IRInstruction::TypeHint_NONE) {
				set_register_type(reg, type);
			}
		}
	}

	// Generate code for function body
	for (const auto& stmt : func.body) {
		gen_stmt(stmt.get(), ir_func);
	}

	// Ensure function returns (add implicit return if needed)
	if (ir_func.instructions.empty() ||
	    ir_func.instructions.back().opcode != IROpcode::RETURN) {
		ir_func.instructions.emplace_back(IROpcode::RETURN);
	}

	ir_func.max_registers = m_next_register;

	// Pop root scope
	pop_scope();

	return ir_func;
}

void CodeGenerator::gen_stmt(const Stmt* stmt, IRFunction& func) {
	if (auto* var_decl = dynamic_cast<const VarDeclStmt*>(stmt)) {
		gen_var_decl(var_decl, func);
	} else if (auto* assign = dynamic_cast<const AssignStmt*>(stmt)) {
		gen_assign(assign, func);
	} else if (auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
		gen_return(ret, func);
	} else if (auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
		gen_if(if_stmt, func);
	} else if (auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt)) {
		gen_while(while_stmt, func);
	} else if (auto* for_stmt = dynamic_cast<const ForStmt*>(stmt)) {
		gen_for(for_stmt, func);
	} else if (dynamic_cast<const BreakStmt*>(stmt)) {
		gen_break(nullptr, func);
	} else if (dynamic_cast<const ContinueStmt*>(stmt)) {
		gen_continue(nullptr, func);
	} else if (dynamic_cast<const PassStmt*>(stmt)) {
		// No-op
	} else if (auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
		gen_expr_stmt(expr_stmt, func);
	} else {
		throw std::runtime_error("Unknown statement type");
	}
}

void CodeGenerator::gen_var_decl(const VarDeclStmt* stmt, IRFunction& func) {
	int reg = -1;

	if (stmt->initializer) {
		reg = gen_expr(stmt->initializer.get(), func);
	} else {
		reg = alloc_register();
		// Initialize to null/0
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
	}

	// Track type hint if provided
	if (!stmt->type_hint.empty()) {
		IRInstruction::TypeHint type = type_hint_from_string(stmt->type_hint);
		if (type != IRInstruction::TypeHint_NONE) {
			set_register_type(reg, type);
		}
	} else if (stmt->initializer) {
		// Infer type from initializer
		IRInstruction::TypeHint init_type = get_register_type(reg);
		if (init_type != IRInstruction::TypeHint_NONE) {
			set_register_type(reg, init_type);
		}
	}

	declare_variable(stmt->name, reg, stmt->is_const);
}

void CodeGenerator::gen_assign(const AssignStmt* stmt, IRFunction& func) {
	int value_reg = gen_expr(stmt->value.get(), func);

	// Check if this is an indexed assignment (arr[0] = value) or property assignment (obj.prop = value)
	if (stmt->target) {
		// Check for indexed assignment: arr[idx] = value
		if (auto* index_expr = dynamic_cast<const IndexExpr*>(stmt->target.get())) {
			int obj_reg = gen_expr(index_expr->object.get(), func);
			int idx_reg = gen_expr(index_expr->index.get(), func);

			// Use VCALL to call .set(index, value)
			// Format: VCALL result_reg, obj_reg, method_name, arg_count, arg1_reg, arg2_reg
			int result_reg = alloc_register();
			IRInstruction vcall_instr(IROpcode::VCALL);
			vcall_instr.operands.push_back(IRValue::reg(result_reg));
			vcall_instr.operands.push_back(IRValue::reg(obj_reg));
			vcall_instr.operands.push_back(IRValue::str("set"));
			vcall_instr.operands.push_back(IRValue::imm(2)); // 2 arguments
			vcall_instr.operands.push_back(IRValue::reg(idx_reg));
			vcall_instr.operands.push_back(IRValue::reg(value_reg));
			func.instructions.push_back(vcall_instr);

			free_register(obj_reg);
			free_register(idx_reg);
			free_register(value_reg);
			free_register(result_reg);
			return;
		}

		// Check for property assignment: obj.prop = value
		if (auto* member_expr = dynamic_cast<const MemberCallExpr*>(stmt->target.get())) {
			// Verify this is a property access (not a method call)
			if (member_expr->is_method_call) {
				throw std::runtime_error("Cannot assign to method call");
			}

			int obj_reg = gen_expr(member_expr->object.get(), func);

			// Property set: obj.prop = value
			// Use dedicated VSET instruction with ECALL_OBJ_PROP_SET syscall

			// Get string index for property name
			int str_idx = add_string_constant(member_expr->member_name);

			// Emit VSET instruction
			// Format: VSET obj_reg, string_idx, string_len, value_reg
			IRInstruction vset_instr(IROpcode::VSET);
			vset_instr.operands.push_back(IRValue::reg(obj_reg));
			vset_instr.operands.push_back(IRValue::imm(str_idx));
			vset_instr.operands.push_back(IRValue::imm(static_cast<int64_t>(member_expr->member_name.length())));
			vset_instr.operands.push_back(IRValue::reg(value_reg));
			func.instructions.push_back(vset_instr);

			free_register(obj_reg);
			free_register(value_reg);
			return;
		}

		throw std::runtime_error("Invalid assignment target type");
	}

	// Simple variable assignment
	// Check if this is a global variable
	if (is_global_variable(stmt->name)) {
		size_t global_idx = m_global_variables.at(stmt->name);
		func.instructions.emplace_back(IROpcode::STORE_GLOBAL, IRValue::imm(global_idx), IRValue::reg(value_reg));
		free_register(value_reg);
		return;
	}

	Variable* var = find_variable(stmt->name);
	if (!var) {
		throw std::runtime_error("Undefined variable: " + stmt->name);
	}

	// Check if variable is const
	if (var->is_const) {
		throw std::runtime_error("Cannot assign to const variable: " + stmt->name);
	}

	// Store value into variable's register
	if (var->register_num != value_reg) {
		func.instructions.emplace_back(IROpcode::MOVE,
		                               IRValue::reg(var->register_num),
		                               IRValue::reg(value_reg));
	}

	free_register(value_reg);
}

void CodeGenerator::gen_return(const ReturnStmt* stmt, IRFunction& func) {
	if (stmt->value) {
		int reg = gen_expr(stmt->value.get(), func);
		// Move return value to register 0 (return register)
		// Skip if already in register 0
		if (reg != 0) {
			func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(0), IRValue::reg(reg));
		}
		free_register(reg);
	}

	func.instructions.emplace_back(IROpcode::RETURN);
}

void CodeGenerator::gen_if(const IfStmt* stmt, IRFunction& func) {
	std::string else_label = make_label("else");
	std::string end_label = make_label("endif");

	// Evaluate condition
	int cond_reg = gen_expr(stmt->condition.get(), func);

	// Branch to else if condition is zero (false)
	if (!stmt->else_branch.empty()) {
		func.instructions.emplace_back(IROpcode::BRANCH_ZERO, IRValue::reg(cond_reg), IRValue::label(else_label));
	} else {
		func.instructions.emplace_back(IROpcode::BRANCH_ZERO, IRValue::reg(cond_reg), IRValue::label(end_label));
	}

	free_register(cond_reg);

	// Then branch (new scope)
	push_scope();
	for (const auto& s : stmt->then_branch) {
		gen_stmt(s.get(), func);
	}
	pop_scope();

	if (!stmt->else_branch.empty()) {
		func.instructions.emplace_back(IROpcode::JUMP, IRValue::label(end_label));

		// Else branch (new scope)
		func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(else_label));
		push_scope();
		for (const auto& s : stmt->else_branch) {
			gen_stmt(s.get(), func);
		}
		pop_scope();
	}

	func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));
}

void CodeGenerator::gen_while(const WhileStmt* stmt, IRFunction& func) {
	std::string loop_label = make_label("loop");
	std::string end_label = make_label("endloop");

	// Push loop context for break/continue
	m_loop_stack.push_back({end_label, loop_label});

	// Loop start
	func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(loop_label));

	// Evaluate condition
	int cond_reg = gen_expr(stmt->condition.get(), func);
	func.instructions.emplace_back(IROpcode::BRANCH_ZERO, IRValue::reg(cond_reg), IRValue::label(end_label));
	free_register(cond_reg);

	// Loop body (new scope)
	push_scope();
	for (const auto& s : stmt->body) {
		gen_stmt(s.get(), func);
	}
	pop_scope();

	// Jump back to loop start
	func.instructions.emplace_back(IROpcode::JUMP, IRValue::label(loop_label));

	// Loop end
	func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));

	m_loop_stack.pop_back();
}

void CodeGenerator::gen_for(const ForStmt* stmt, IRFunction& func) {
	// Desugar: for variable in iterable: body
	// Support range() calls and array iteration
	// For range(): Convert to:
	//   var _iter = iterable  (evaluate range())
	//   var variable = 0
	//   while variable < _iter:
	//     body
	//     variable = variable + 1
	// For arrays: Convert to:
	//   var _array = iterable
	//   var _idx = 0
	//   while _idx < _array.size():
	//     var variable = _array[_idx]
	//     body
	//     _idx = _idx + 1

	// Check if iterable is a range() call
	auto* call_expr = dynamic_cast<const CallExpr*>(stmt->iterable.get());
	bool is_range = call_expr && call_expr->function_name == "range";

	// Check for obviously non-iterable types and give a proper error
	auto* literal = dynamic_cast<const LiteralExpr*>(stmt->iterable.get());
	if (literal) {
		if (literal->lit_type == LiteralExpr::Type::INTEGER ||
		    literal->lit_type == LiteralExpr::Type::FLOAT ||
		    literal->lit_type == LiteralExpr::Type::BOOL ||
		    literal->lit_type == LiteralExpr::Type::NULL_VAL) {
			throw std::runtime_error("Cannot iterate over non-iterable type in 'for' loop. Did you mean 'for " +
			                         stmt->variable + " in range(" +
			                         (literal->lit_type == LiteralExpr::Type::INTEGER ?
			                          std::to_string(std::get<int64_t>(literal->value)) : "N") +
			                         "):'?");
		}
	}

	if (!is_range) {
		// Array iteration
		std::string loop_label = make_label("for_loop");
		std::string continue_label = make_label("for_continue");
		std::string end_label = make_label("for_end");

		// Push loop context for break/continue
		m_loop_stack.push_back({end_label, continue_label});

		// Create new scope for loop (includes loop variable)
		push_scope();

		int array_reg = gen_expr(stmt->iterable.get(), func);

		// Initialize index counter with 0
		int index_reg = alloc_register();
		auto& index_load = func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(index_reg), IRValue::imm(0));

		// Loop start
		func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(loop_label));

		// Call ECALL_ARRAY_SIZE to get the array size
		// ECALL_ARRAY_SIZE = GAME_API_BASE + 23 = 523
		int size_reg = alloc_register();
		IRInstruction size_syscall(IROpcode::CALL_SYSCALL);
		size_syscall.operands.push_back(IRValue::reg(size_reg));  // result register
		size_syscall.operands.push_back(IRValue::imm(523));         // ECALL_ARRAY_SIZE
		size_syscall.operands.push_back(IRValue::reg(array_reg));   // array register
		func.instructions.push_back(size_syscall);

		// Condition: index < size
		int cond_reg = alloc_register();
		auto& cmp_instr = func.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
		                               IRValue::reg(index_reg), IRValue::reg(size_reg));

		func.instructions.emplace_back(IROpcode::BRANCH_ZERO, IRValue::reg(cond_reg), IRValue::label(end_label));
		free_register(cond_reg);

		// Get element from array using ECALL_ARRAY_AT
		// ECALL_ARRAY_AT = GAME_API_BASE + 22 = 522
		int elem_reg = alloc_register();
		IRInstruction at_syscall(IROpcode::CALL_SYSCALL);
		at_syscall.operands.push_back(IRValue::reg(elem_reg));    // result register (element)
		at_syscall.operands.push_back(IRValue::imm(522));         // ECALL_ARRAY_AT
		at_syscall.operands.push_back(IRValue::reg(array_reg));   // array register
		at_syscall.operands.push_back(IRValue::reg(index_reg));   // index register
		func.instructions.push_back(at_syscall);

		// Assign the element to the loop variable
		declare_variable(stmt->variable, elem_reg);

		// Loop body (new scope for body, separate from loop variable scope)
		push_scope();
		for (const auto& s : stmt->body) {
			gen_stmt(s.get(), func);
		}
		pop_scope();

		// Continue label - where continue jumps to
		func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(continue_label));

		// Increment: index = index + 1
		int new_idx_reg = alloc_register();
		auto& add_instr = func.instructions.emplace_back(IROpcode::ADD, IRValue::reg(new_idx_reg),
		                               IRValue::reg(index_reg), IRValue::imm(1));
		auto& move_instr = func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(index_reg), IRValue::reg(new_idx_reg));
		free_register(new_idx_reg);

		// Jump back to loop start
		func.instructions.emplace_back(IROpcode::JUMP, IRValue::label(loop_label));

		// Loop end
		func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));

		// Clean up
		pop_scope();
		m_loop_stack.pop_back();
		// Note: array_reg and size_reg are allocated inside the loop, so they're freed each iteration
		free_register(index_reg);
		free_register(elem_reg);
		return;
	}

	// Generate range() arguments
	// range(n) -> 0 to n-1
	// range(start, end) -> start to end-1
	// range(start, end, step) -> start to end-1 by step

	int start_reg = -1, end_reg = -1, step_reg = -1;

	if (call_expr->arguments.size() == 1) {
		// range(n): start=0, end=n, step=1
		start_reg = alloc_register();
		auto& start_instr = func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(start_reg), IRValue::imm(0));
		end_reg = gen_expr(call_expr->arguments[0].get(), func);
		step_reg = alloc_register();
		auto& step_instr = func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(step_reg), IRValue::imm(1));
	} else if (call_expr->arguments.size() == 2) {
		// range(start, end): step=1
		start_reg = gen_expr(call_expr->arguments[0].get(), func);
		end_reg = gen_expr(call_expr->arguments[1].get(), func);
		step_reg = alloc_register();
		auto& step_instr = func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(step_reg), IRValue::imm(1));
	} else if (call_expr->arguments.size() == 3) {
		// range(start, end, step)
		start_reg = gen_expr(call_expr->arguments[0].get(), func);
		end_reg = gen_expr(call_expr->arguments[1].get(), func);
		step_reg = gen_expr(call_expr->arguments[2].get(), func);
	} else {
		throw std::runtime_error("range() takes 1, 2, or 3 arguments");
	}

	std::string loop_label = make_label("for_loop");
	std::string continue_label = make_label("for_continue");
	std::string end_label = make_label("for_end");

	// Push loop context for break/continue
	// Continue should jump to the increment step, not the condition check
	m_loop_stack.push_back({end_label, continue_label});

	// Create new scope for loop (includes loop variable)
	push_scope();

	// Initialize loop variable with start value
	int loop_var_reg = alloc_register();
	auto& move_instr = func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(loop_var_reg), IRValue::reg(start_reg));
	declare_variable(stmt->variable, loop_var_reg);

	// Loop start
	func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(loop_label));

	// Condition depends on step direction:
	// - If step > 0: loop_var < end
	// - If step < 0: loop_var > end
	// - If step == 0: infinite loop (but that's a user error)
	//
	// For runtime step values, we need to check the sign dynamically.
	// For now, we'll check if step is a compile-time constant to optimize.

	int cond_reg = alloc_register();

	// Check if step is a constant value
	bool step_is_constant = false;
	int64_t step_value = 0;

	// Look back one instruction to see if step_reg was loaded with LOAD_IMM
	if (func.instructions.size() >= 3) {
		auto& prev_instr = func.instructions[func.instructions.size() - 3];
		if (prev_instr.opcode == IROpcode::LOAD_IMM &&
		    std::get<int>(prev_instr.operands[0].value) == step_reg) {
			step_is_constant = true;
			step_value = std::get<int64_t>(prev_instr.operands[1].value);
		}
	}

	if (step_is_constant) {
		// Optimize: use appropriate comparison based on constant step
		if (step_value >= 0) {
			// Forward iteration: loop_var < end
			auto& cmp_instr = func.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
			                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
			cmp_instr.type_hint = Variant::INT; // range() always produces integers
		} else {
			// Backward iteration: loop_var > end
			auto& cmp_instr = func.instructions.emplace_back(IROpcode::CMP_GT, IRValue::reg(cond_reg),
			                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
			cmp_instr.type_hint = Variant::INT; // range() always produces integers
		}
	} else {
		// Runtime step: check sign dynamically
		// if step >= 0: check loop_var < end
		// else: check loop_var > end
		std::string pos_step_label = make_label("for_pos_step");
		std::string check_cond_label = make_label("for_check_cond");

		int zero_reg = alloc_register();
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(zero_reg), IRValue::imm(0));

		int step_sign_reg = alloc_register();
		auto& step_cmp = func.instructions.emplace_back(IROpcode::CMP_GTE, IRValue::reg(step_sign_reg),
		                               IRValue::reg(step_reg), IRValue::reg(zero_reg));
		step_cmp.type_hint = Variant::INT; // range() always produces integers
		free_register(zero_reg);

		// If step >= 0, use loop_var < end
		func.instructions.emplace_back(IROpcode::BRANCH_NOT_ZERO, IRValue::reg(step_sign_reg),
		                               IRValue::label(pos_step_label));

		// Negative step: loop_var > end
		auto& neg_cmp = func.instructions.emplace_back(IROpcode::CMP_GT, IRValue::reg(cond_reg),
		                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
		neg_cmp.type_hint = Variant::INT; // range() always produces integers
		func.instructions.emplace_back(IROpcode::JUMP, IRValue::label(check_cond_label));

		// Positive step: loop_var < end
		func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(pos_step_label));
		auto& pos_cmp = func.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
		                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
		pos_cmp.type_hint = Variant::INT; // range() always produces integers

		func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(check_cond_label));
		free_register(step_sign_reg);
	}

	func.instructions.emplace_back(IROpcode::BRANCH_ZERO, IRValue::reg(cond_reg), IRValue::label(end_label));
	free_register(cond_reg);

	// Loop body (new scope for body, separate from loop variable scope)
	push_scope();
	for (const auto& s : stmt->body) {
		gen_stmt(s.get(), func);
	}
	pop_scope();

	// Continue label - where continue jumps to
	func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(continue_label));

	// Increment: loop_var = loop_var + step
	int new_val_reg = alloc_register();
	auto& add_instr = func.instructions.emplace_back(IROpcode::ADD, IRValue::reg(new_val_reg),
	                               IRValue::reg(loop_var_reg), IRValue::reg(step_reg));
	add_instr.type_hint = Variant::INT; // range() always produces integers
	auto& move_instr2 = func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(loop_var_reg), IRValue::reg(new_val_reg));
	free_register(new_val_reg);

	// Jump back to loop start
	func.instructions.emplace_back(IROpcode::JUMP, IRValue::label(loop_label));

	// Loop end
	func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));

	// Clean up
	pop_scope();
	m_loop_stack.pop_back();
	free_register(start_reg);
	free_register(end_reg);
	free_register(step_reg);
}

void CodeGenerator::gen_break(const BreakStmt* stmt, IRFunction& func) {
	if (m_loop_stack.empty()) {
		throw std::runtime_error("'break' outside of loop");
	}

	func.instructions.emplace_back(IROpcode::JUMP, IRValue::label(m_loop_stack.back().break_label));
}

void CodeGenerator::gen_continue(const ContinueStmt* stmt, IRFunction& func) {
	if (m_loop_stack.empty()) {
		throw std::runtime_error("'continue' outside of loop");
	}

	func.instructions.emplace_back(IROpcode::JUMP, IRValue::label(m_loop_stack.back().continue_label));
}

void CodeGenerator::gen_expr_stmt(const ExprStmt* stmt, IRFunction& func) {
	int reg = gen_expr(stmt->expression.get(), func);
	free_register(reg);
}

int CodeGenerator::gen_expr(const Expr* expr, IRFunction& func) {
	if (auto* lit = dynamic_cast<const LiteralExpr*>(expr)) {
		return gen_literal(lit, func);
	} else if (auto* var = dynamic_cast<const VariableExpr*>(expr)) {
		return gen_variable(var, func);
	} else if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
		return gen_binary(bin, func);
	} else if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
		return gen_unary(un, func);
	} else if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
		return gen_call(call, func);
	} else if (auto* member = dynamic_cast<const MemberCallExpr*>(expr)) {
		return gen_member_call(member, func);
	} else if (auto* index = dynamic_cast<const IndexExpr*>(expr)) {
		return gen_index(index, func);
	} else if (auto* array_lit = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
		return gen_array_literal(array_lit, func);
	} else {
		throw std::runtime_error("Unknown expression type");
	}
}

int CodeGenerator::gen_literal(const LiteralExpr* expr, IRFunction& func) {
	int reg = alloc_register();

	switch (expr->lit_type) {
		case LiteralExpr::Type::INTEGER: {
			IRInstruction instr(IROpcode::LOAD_IMM, IRValue::reg(reg),
			                    IRValue::imm(std::get<int64_t>(expr->value)));
			instr.type_hint = Variant::INT;
			func.instructions.push_back(instr);
			set_register_type(reg, Variant::INT);
			break;
		}

		case LiteralExpr::Type::FLOAT: {
			// Float literals are always 64-bit doubles in GDScript
			double d = std::get<double>(expr->value);
			IRInstruction instr(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(reg), IRValue::fimm(d));
			instr.type_hint = Variant::FLOAT;
			func.instructions.push_back(instr);
			set_register_type(reg, Variant::FLOAT);
			break;
		}

		case LiteralExpr::Type::BOOL: {
			IRInstruction instr(IROpcode::LOAD_BOOL, IRValue::reg(reg),
			                    IRValue::imm(std::get<bool>(expr->value) ? 1 : 0));
			instr.type_hint = Variant::BOOL;
			func.instructions.push_back(instr);
			set_register_type(reg, Variant::BOOL);
			break;
		}

		case LiteralExpr::Type::STRING: {
			int str_idx = add_string_constant(std::get<std::string>(expr->value));
			IRInstruction instr(IROpcode::LOAD_STRING, IRValue::reg(reg), IRValue::imm(str_idx));
			instr.type_hint = Variant::STRING;
			func.instructions.push_back(instr);
			set_register_type(reg, Variant::STRING);
			break;
		}

		case LiteralExpr::Type::NULL_VAL:
			func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
			break;
	}

	return reg;
}

int CodeGenerator::gen_variable(const VariableExpr* expr, IRFunction& func) {
	// Check if this is a global class reference
	if (is_global_class(expr->name)) {
		return gen_global_class_get(expr->name, func);
	}

	// Handle 'self' as an alias for get_node()
	if (expr->name == "self") {
		// Generate get_node() call
		int result_reg = alloc_register();

		// CALL_SYSCALL result_reg, ECALL_GET_NODE, 0
		IRInstruction instr(IROpcode::CALL_SYSCALL);
		instr.operands.push_back(IRValue::reg(result_reg));    // result register
		instr.operands.push_back(IRValue::imm(507));            // ECALL_GET_NODE
		instr.operands.push_back(IRValue::imm(0));              // addr = 0 (owner node)
		func.instructions.push_back(instr);

		return result_reg;
	}

	// Check if this is a global variable
	if (is_global_variable(expr->name)) {
		int result_reg = alloc_register();
		size_t global_idx = m_global_variables.at(expr->name);
		func.instructions.emplace_back(IROpcode::LOAD_GLOBAL, IRValue::reg(result_reg), IRValue::imm(global_idx));
		return result_reg;
	}

	Variable* var = find_variable(expr->name);
	if (!var) {
		throw std::runtime_error("Undefined variable: " + expr->name);
	}

	// Return a copy in a new register
	int new_reg = alloc_register();
	func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(new_reg), IRValue::reg(var->register_num));

	// Propagate type information from the variable to the new register
	IRInstruction::TypeHint var_type = get_register_type(var->register_num);
	if (var_type != IRInstruction::TypeHint_NONE) {
		set_register_type(new_reg, var_type);
	}

	return new_reg;
}

int CodeGenerator::gen_binary(const BinaryExpr* expr, IRFunction& func) {
	int left_reg = gen_expr(expr->left.get(), func);
	int right_reg = gen_expr(expr->right.get(), func);
	int result_reg = alloc_register();

	// Check type hints for operands to determine if result should be float
	IRInstruction::TypeHint left_type = get_register_type(left_reg);
	IRInstruction::TypeHint right_type = get_register_type(right_reg);

	// Determine if this is an arithmetic operation (vs comparison or logical)
	bool is_arithmetic = (expr->op == BinaryExpr::Op::ADD ||
	                      expr->op == BinaryExpr::Op::SUB ||
	                      expr->op == BinaryExpr::Op::MUL ||
	                      expr->op == BinaryExpr::Op::DIV ||
	                      expr->op == BinaryExpr::Op::MOD);
	bool is_comparison = (expr->op == BinaryExpr::Op::EQ ||
	                      expr->op == BinaryExpr::Op::NEQ ||
	                      expr->op == BinaryExpr::Op::LT ||
	                      expr->op == BinaryExpr::Op::LTE ||
	                      expr->op == BinaryExpr::Op::GT ||
	                      expr->op == BinaryExpr::Op::GTE);

	// For arithmetic operations and comparisons:
	// ONLY set type hint when BOTH operands have the SAME type
	// This enables native RISC-V codegen optimizations
	//
	// When types don't match (e.g. INT + FLOAT), we leave result_type as NONE
	// and fall back to VEVAL syscall which handles type coercion correctly
	IRInstruction::TypeHint result_type = IRInstruction::TypeHint_NONE;
	if (is_arithmetic || is_comparison) {
		if (left_type == Variant::INT && right_type == Variant::INT) {
			result_type = Variant::INT;
		} else if (left_type == Variant::FLOAT && right_type == Variant::FLOAT) {
			result_type = Variant::FLOAT;
		} else if (left_type != IRInstruction::TypeHint_NONE &&
		           right_type != IRInstruction::TypeHint_NONE &&
		           left_type == right_type &&
		           TypeHintUtils::is_vector(left_type)) {
			// Both operands are the same vector type
			result_type = left_type;
		}
		// Mixed types or unsupported types: leave as NONE for VEVAL fallback
	}

	IROpcode op;
	switch (expr->op) {
		case BinaryExpr::Op::ADD: op = IROpcode::ADD; break;
		case BinaryExpr::Op::SUB: op = IROpcode::SUB; break;
		case BinaryExpr::Op::MUL: op = IROpcode::MUL; break;
		case BinaryExpr::Op::DIV: op = IROpcode::DIV; break;
		case BinaryExpr::Op::MOD: op = IROpcode::MOD; break;
		case BinaryExpr::Op::EQ: op = IROpcode::CMP_EQ; break;
		case BinaryExpr::Op::NEQ: op = IROpcode::CMP_NEQ; break;
		case BinaryExpr::Op::LT: op = IROpcode::CMP_LT; break;
		case BinaryExpr::Op::LTE: op = IROpcode::CMP_LTE; break;
		case BinaryExpr::Op::GT: op = IROpcode::CMP_GT; break;
		case BinaryExpr::Op::GTE: op = IROpcode::CMP_GTE; break;
		case BinaryExpr::Op::AND: op = IROpcode::AND; break;
		case BinaryExpr::Op::OR: op = IROpcode::OR; break;
		default:
			throw std::runtime_error("Unknown binary operator");
	}

	IRInstruction instr(op, IRValue::reg(result_reg), IRValue::reg(left_reg), IRValue::reg(right_reg));
	instr.type_hint = result_type;
	func.instructions.push_back(instr);

	if (result_type != IRInstruction::TypeHint_NONE) {
		set_register_type(result_reg, result_type);
	}

	free_register(left_reg);
	free_register(right_reg);

	return result_reg;
}

int CodeGenerator::gen_unary(const UnaryExpr* expr, IRFunction& func) {
	int operand_reg = gen_expr(expr->operand.get(), func);
	int result_reg = alloc_register();

	IROpcode op = (expr->op == UnaryExpr::Op::NEG) ? IROpcode::NEG : IROpcode::NOT;
	func.instructions.emplace_back(op, IRValue::reg(result_reg), IRValue::reg(operand_reg));

	free_register(operand_reg);
	return result_reg;
}

int CodeGenerator::gen_call(const CallExpr* expr, IRFunction& func) {
	// Generate code for arguments
	std::vector<int> arg_regs;
	for (const auto& arg : expr->arguments) {
		arg_regs.push_back(gen_expr(arg.get(), func));
	}

	// Check if this is an inline primitive constructor
	if (is_inline_primitive_constructor(expr->function_name)) {
		int result = gen_inline_constructor(expr->function_name, arg_regs, func);
		for (int reg : arg_regs) {
			free_register(reg);
		}
		return result;
	}

	// Handle get_node() as a special syscall
	if (expr->function_name == "get_node") {
		// get_node() takes 0 or 1 argument (node path)
		if (arg_regs.size() > 1) {
			throw std::runtime_error("get_node() takes at most 1 argument");
		}

		int result_reg = alloc_register();

		if (arg_regs.empty()) {
			// get_node() with no args - get the owner node
			// CALL_SYSCALL result_reg, ECALL_GET_NODE, 0
			IRInstruction instr(IROpcode::CALL_SYSCALL);
			instr.operands.push_back(IRValue::reg(result_reg));    // result register
			instr.operands.push_back(IRValue::imm(507));            // ECALL_GET_NODE
			instr.operands.push_back(IRValue::imm(0));              // addr = 0 (owner node)
			func.instructions.push_back(instr);
		} else {
			// get_node(path) - will be handled in RISC-V codegen
			// For now, convert to CALL_SYSCALL with the path argument
			int path_reg = arg_regs[0];

			IRInstruction instr(IROpcode::CALL_SYSCALL);
			instr.operands.push_back(IRValue::reg(result_reg));    // result register
			instr.operands.push_back(IRValue::imm(507));            // ECALL_GET_NODE
			instr.operands.push_back(IRValue::imm(0));              // addr = 0 (owner node)
			instr.operands.push_back(IRValue::reg(path_reg));       // path register
			func.instructions.push_back(instr);
		}

		for (int reg : arg_regs) {
			free_register(reg);
		}

		return result_reg;
	}

	// Check if this is a call to a locally defined function
	if (is_local_function(expr->function_name)) {
		// Local function call - use regular CALL instruction
		int result_reg = alloc_register();

		// Generate CALL instruction with function name, result register, and argument registers
		// Format: CALL function_name, result_reg, arg_count, arg1_reg, arg2_reg, ...
		IRInstruction call_instr(IROpcode::CALL);
		call_instr.operands.push_back(IRValue::str(expr->function_name));
		call_instr.operands.push_back(IRValue::reg(result_reg));
		call_instr.operands.push_back(IRValue::imm(arg_regs.size()));
		for (int arg_reg : arg_regs) {
			call_instr.operands.push_back(IRValue::reg(arg_reg));
		}
		func.instructions.push_back(call_instr);

		for (int reg : arg_regs) {
			free_register(reg);
		}

		return result_reg;
	}

	// Treat all other freestanding function calls as self-calls
	// Convert foo(arg1, arg2) to self.foo(arg1, arg2)
	int self_reg = alloc_register();

	// Generate get_node() for self
	// CALL_SYSCALL self_reg, ECALL_GET_NODE, 0
	IRInstruction get_self_instr(IROpcode::CALL_SYSCALL);
	get_self_instr.operands.push_back(IRValue::reg(self_reg));     // result register
	get_self_instr.operands.push_back(IRValue::imm(507));            // ECALL_GET_NODE
	get_self_instr.operands.push_back(IRValue::imm(0));              // addr = 0 (owner node)
	func.instructions.push_back(get_self_instr);

	int result_reg = alloc_register();

	// Generate VCALL instruction for self.method call
	// Format: VCALL result_reg, self_reg, method_name, arg_count, arg1_reg, arg2_reg, ...
	IRInstruction vcall_instr(IROpcode::VCALL);
	vcall_instr.operands.push_back(IRValue::reg(result_reg));
	vcall_instr.operands.push_back(IRValue::reg(self_reg));
	vcall_instr.operands.push_back(IRValue::str(expr->function_name));
	vcall_instr.operands.push_back(IRValue::imm(arg_regs.size()));
	for (int arg_reg : arg_regs) {
		vcall_instr.operands.push_back(IRValue::reg(arg_reg));
	}
	func.instructions.push_back(vcall_instr);

	free_register(self_reg);
	for (int reg : arg_regs) {
		free_register(reg);
	}

	return result_reg;
}

int CodeGenerator::gen_member_call(const MemberCallExpr* expr, IRFunction& func) {
	int obj_reg = gen_expr(expr->object.get(), func);

	// Generate code for arguments
	std::vector<int> arg_regs;
	for (const auto& arg : expr->arguments) {
		arg_regs.push_back(gen_expr(arg.get(), func));
	}

	// Check if this is inline member access (x, y, z, r, g, b, a on vectors)
	if (!expr->is_method_call && arg_regs.empty()) {
		IRInstruction::TypeHint obj_type = get_register_type(obj_reg);
		if (is_inline_member_access(obj_type, expr->member_name)) {
			int result = gen_inline_member_get(obj_reg, obj_type, expr->member_name, func);
			free_register(obj_reg);
			return result;
		}

		// Property access: obj.property (no parentheses)
		// Use dedicated VGET instruction with ECALL_OBJ_PROP_GET syscall
		int result_reg = alloc_register();

		// Get string index for property name
		int str_idx = add_string_constant(expr->member_name);

		// Emit VGET instruction
		// Format: VGET result_reg, obj_reg, string_idx, string_len
		IRInstruction vget_instr(IROpcode::VGET);
		vget_instr.operands.push_back(IRValue::reg(result_reg));
		vget_instr.operands.push_back(IRValue::reg(obj_reg));
		vget_instr.operands.push_back(IRValue::imm(str_idx));
		vget_instr.operands.push_back(IRValue::imm(static_cast<int64_t>(expr->member_name.length())));
		func.instructions.push_back(vget_instr);

		free_register(obj_reg);
		return result_reg;
	}

	int result_reg = alloc_register();

	// Use VCALL for Variant method calls
	// Format: VCALL result_reg, obj_reg, method_name, arg_count, arg1_reg, arg2_reg, ...
	IRInstruction vcall_instr(IROpcode::VCALL);
	vcall_instr.operands.push_back(IRValue::reg(result_reg));
	vcall_instr.operands.push_back(IRValue::reg(obj_reg));
	vcall_instr.operands.push_back(IRValue::str(expr->member_name));
	vcall_instr.operands.push_back(IRValue::imm(arg_regs.size()));
	for (int arg_reg : arg_regs) {
		vcall_instr.operands.push_back(IRValue::reg(arg_reg));
	}
	func.instructions.push_back(vcall_instr);

	free_register(obj_reg);
	for (int reg : arg_regs) {
		free_register(reg);
	}

	return result_reg;
}

int CodeGenerator::gen_index(const IndexExpr* expr, IRFunction& func) {
	int obj_reg = gen_expr(expr->object.get(), func);
	int idx_reg = gen_expr(expr->index.get(), func);

	int result_reg = alloc_register();

	// Transform arr[x] to arr.get(x) using VCALL
	// Format: VCALL result_reg, obj_reg, method_name, arg_count, arg1_reg
	IRInstruction vcall_instr(IROpcode::VCALL);
	vcall_instr.operands.push_back(IRValue::reg(result_reg));
	vcall_instr.operands.push_back(IRValue::reg(obj_reg));
	vcall_instr.operands.push_back(IRValue::str("get"));
	vcall_instr.operands.push_back(IRValue::imm(1)); // 1 argument
	vcall_instr.operands.push_back(IRValue::reg(idx_reg));
	func.instructions.push_back(vcall_instr);

	free_register(obj_reg);
	free_register(idx_reg);

	return result_reg;
}

int CodeGenerator::gen_array_literal(const ArrayLiteralExpr* expr, IRFunction& func) {
	std::vector<int> elem_regs;

	// Generate code for each element
	for (const auto& elem : expr->elements) {
		int reg = gen_expr(elem.get(), func);
		elem_regs.push_back(reg);
	}

	int result_reg = alloc_register();

	// Create MAKE_ARRAY instruction
	// Format: MAKE_ARRAY result_reg, element_count, elem1_reg, elem2_reg, ...
	IRInstruction instr(IROpcode::MAKE_ARRAY);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(static_cast<int>(elem_regs.size()))); // element count
	for (int reg : elem_regs) {
		instr.operands.push_back(IRValue::reg(reg));
	}

	func.instructions.push_back(instr);
	set_register_type(result_reg, Variant::ARRAY);

	// Free element registers
	for (int reg : elem_regs) {
		free_register(reg);
	}

	return result_reg;
}

int CodeGenerator::alloc_register() {
	return m_next_register++;
}

void CodeGenerator::free_register(int reg) {
	// In a more sophisticated version, would track free registers
	// For now, registers are never reused within a function
}

std::string CodeGenerator::make_label(const std::string& prefix) {
	return prefix + "_" + std::to_string(m_next_label++);
}

int CodeGenerator::add_string_constant(const std::string& str) {
	// Check if string already exists
	for (size_t i = 0; i < m_string_constants.size(); i++) {
		if (m_string_constants[i] == str) {
			return static_cast<int>(i);
		}
	}

	m_string_constants.push_back(str);
	return static_cast<int>(m_string_constants.size() - 1);
}

void CodeGenerator::push_scope() {
	Scope new_scope;
	if (m_scope_stack.empty()) {
		new_scope.parent_scope_idx = SIZE_MAX; // Root scope
	} else {
		new_scope.parent_scope_idx = m_scope_stack.size() - 1;
	}
	m_scope_stack.push_back(new_scope);
}

void CodeGenerator::pop_scope() {
	if (m_scope_stack.empty()) {
		throw std::runtime_error("Cannot pop scope: scope stack is empty");
	}
	m_scope_stack.pop_back();
}

CodeGenerator::Variable* CodeGenerator::find_variable(const std::string& name) {
	// Search from innermost to outermost scope
	for (int i = static_cast<int>(m_scope_stack.size()) - 1; i >= 0; i--) {
		auto it = m_scope_stack[i].variables.find(name);
		if (it != m_scope_stack[i].variables.end()) {
			return &it->second;
		}
	}
	return nullptr; // Not found
}

void CodeGenerator::declare_variable(const std::string& name, int register_num, bool is_const) {
	if (m_scope_stack.empty()) {
		throw std::runtime_error("Cannot declare variable: no scope active");
	}

	// Check if variable already exists in current scope (shadowing is allowed, but redeclaration in same scope is not)
	auto& current_scope = m_scope_stack.back();
	if (current_scope.variables.find(name) != current_scope.variables.end()) {
		throw std::runtime_error("Variable '" + name + "' already declared in current scope");
	}

	current_scope.variables[name] = {name, register_num, IRInstruction::TypeHint_NONE, is_const};
}

// Type tracking helpers
void CodeGenerator::set_register_type(int reg, IRInstruction::TypeHint type) {
	m_register_types[reg] = type;
}

IRInstruction::TypeHint CodeGenerator::get_register_type(int reg) const {
	auto it = m_register_types.find(reg);
	if (it != m_register_types.end()) {
		return it->second;
	}
	return IRInstruction::TypeHint_NONE;
}

bool CodeGenerator::is_inline_primitive_constructor(const std::string& name) const {
	return name == "Vector2" || name == "Vector3" || name == "Vector4" ||
	       name == "Vector2i" || name == "Vector3i" || name == "Vector4i" ||
	       name == "Color" || name == "Rect2" || name == "Rect2i" || name == "Plane" ||
	       name == "Array" || name == "Dictionary" ||
	       name == "PackedByteArray" || name == "PackedInt32Array" ||
	       name == "PackedInt64Array" || name == "PackedFloat32Array" ||
	       name == "PackedFloat64Array" || name == "PackedStringArray" ||
	       name == "PackedVector2Array" || name == "PackedVector3Array" ||
	       name == "PackedColorArray" || name == "PackedVector4Array";
}

bool CodeGenerator::is_inline_member_access(IRInstruction::TypeHint type, const std::string& member) const {
	switch (type) {
		case Variant::VECTOR2:
		case Variant::VECTOR2I:
			return member == "x" || member == "y";

		case Variant::VECTOR3:
		case Variant::VECTOR3I:
			return member == "x" || member == "y" || member == "z";

		case Variant::VECTOR4:
		case Variant::VECTOR4I:
			return member == "x" || member == "y" || member == "z" || member == "w";

		case Variant::COLOR:
			return member == "r" || member == "g" || member == "b" || member == "a";

		case Variant::RECT2:
		case Variant::RECT2I:
			// Rect2 has position and size, which are Vector2/Vector2i
			// For now, don't optimize these - they're more complex
			return false;

		case Variant::PLANE:
			// Plane has normal (Vector3) and d (float)
			// For now, don't optimize these
			return false;

		default:
			return false;
	}
}

int CodeGenerator::gen_inline_constructor(const std::string& name, const std::vector<int>& arg_regs, IRFunction& func) {
	int result_reg = alloc_register();
	IRInstruction instr(IROpcode::CALL); // Default fallback
	IRInstruction::TypeHint result_type = IRInstruction::TypeHint_NONE;

	if (name == "Vector2" && arg_regs.size() == 2) {
		instr = IRInstruction(IROpcode::MAKE_VECTOR2);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::reg(arg_regs[0])); // x
		instr.operands.push_back(IRValue::reg(arg_regs[1])); // y
		result_type = Variant::VECTOR2;
	} else if (name == "Vector3" && arg_regs.size() == 3) {
		instr = IRInstruction(IROpcode::MAKE_VECTOR3);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::reg(arg_regs[0])); // x
		instr.operands.push_back(IRValue::reg(arg_regs[1])); // y
		instr.operands.push_back(IRValue::reg(arg_regs[2])); // z
		result_type = Variant::VECTOR3;
	} else if (name == "Vector4" && arg_regs.size() == 4) {
		instr = IRInstruction(IROpcode::MAKE_VECTOR4);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::reg(arg_regs[0])); // x
		instr.operands.push_back(IRValue::reg(arg_regs[1])); // y
		instr.operands.push_back(IRValue::reg(arg_regs[2])); // z
		instr.operands.push_back(IRValue::reg(arg_regs[3])); // w
		result_type = Variant::VECTOR4;
	} else if (name == "Vector2i" && arg_regs.size() == 2) {
		instr = IRInstruction(IROpcode::MAKE_VECTOR2I);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::reg(arg_regs[0])); // x
		instr.operands.push_back(IRValue::reg(arg_regs[1])); // y
		result_type = Variant::VECTOR2I;
	} else if (name == "Vector3i" && arg_regs.size() == 3) {
		instr = IRInstruction(IROpcode::MAKE_VECTOR3I);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::reg(arg_regs[0])); // x
		instr.operands.push_back(IRValue::reg(arg_regs[1])); // y
		instr.operands.push_back(IRValue::reg(arg_regs[2])); // z
		result_type = Variant::VECTOR3I;
	} else if (name == "Vector4i" && arg_regs.size() == 4) {
		instr = IRInstruction(IROpcode::MAKE_VECTOR4I);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::reg(arg_regs[0])); // x
		instr.operands.push_back(IRValue::reg(arg_regs[1])); // y
		instr.operands.push_back(IRValue::reg(arg_regs[2])); // z
		instr.operands.push_back(IRValue::reg(arg_regs[3])); // w
		result_type = Variant::VECTOR4I;
	} else if (name == "Color") {
		// Color() with 0 args: white (1, 1, 1, 1)
		// Color(r, g, b) with 3 args: default alpha to 1.0
		// Color(r, g, b, a) with 4 args: full specification
		if (arg_regs.size() == 0) {
			// Color() - white with alpha 1.0
			int r_reg = alloc_register();
			int g_reg = alloc_register();
			int b_reg = alloc_register();
			int a_reg = alloc_register();

			func.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(r_reg), IRValue::fimm(1.0));
			func.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(g_reg), IRValue::fimm(1.0));
			func.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(b_reg), IRValue::fimm(1.0));
			func.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(a_reg), IRValue::fimm(1.0));

			instr = IRInstruction(IROpcode::MAKE_COLOR);
			instr.operands.push_back(IRValue::reg(result_reg));
			instr.operands.push_back(IRValue::reg(r_reg));
			instr.operands.push_back(IRValue::reg(g_reg));
			instr.operands.push_back(IRValue::reg(b_reg));
			instr.operands.push_back(IRValue::reg(a_reg));
			result_type = Variant::COLOR;
		} else if (arg_regs.size() == 3) {
			// Color(r, g, b) - default alpha to 1.0
			int a_reg = alloc_register();
			func.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(a_reg), IRValue::fimm(1.0));

			instr = IRInstruction(IROpcode::MAKE_COLOR);
			instr.operands.push_back(IRValue::reg(result_reg));
			instr.operands.push_back(IRValue::reg(arg_regs[0])); // r
			instr.operands.push_back(IRValue::reg(arg_regs[1])); // g
			instr.operands.push_back(IRValue::reg(arg_regs[2])); // b
			instr.operands.push_back(IRValue::reg(a_reg)); // a = 1.0
			result_type = Variant::COLOR;
		} else if (arg_regs.size() == 4) {
			instr = IRInstruction(IROpcode::MAKE_COLOR);
			instr.operands.push_back(IRValue::reg(result_reg));
			instr.operands.push_back(IRValue::reg(arg_regs[0])); // r
			instr.operands.push_back(IRValue::reg(arg_regs[1])); // g
			instr.operands.push_back(IRValue::reg(arg_regs[2])); // b
			instr.operands.push_back(IRValue::reg(arg_regs[3])); // a
			result_type = Variant::COLOR;
		} else {
			throw std::runtime_error("Color constructor requires 0, 3, or 4 arguments");
		}
	} else if (name == "Array") {
		// Array() - empty array or with initial elements
		// For now, only support empty Array()
		instr = IRInstruction(IROpcode::MAKE_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size()))); // element count
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::ARRAY;
	} else if (name == "PackedByteArray") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_BYTE_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size()))); // element count
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_BYTE_ARRAY;
	} else if (name == "PackedInt32Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_INT32_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size()))); // element count
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_INT32_ARRAY;
	} else if (name == "PackedInt64Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_INT64_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size()))); // element count
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_INT64_ARRAY;
	} else if (name == "PackedFloat32Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_FLOAT32_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size()))); // element count
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_FLOAT32_ARRAY;
	} else if (name == "PackedFloat64Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_FLOAT64_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size()))); // element count
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_FLOAT64_ARRAY;
	} else if (name == "PackedStringArray") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_STRING_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size()))); // element count
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_STRING_ARRAY;
	} else if (name == "PackedVector2Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_VECTOR2_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size()))); // element count
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_VECTOR2_ARRAY;
	} else if (name == "PackedVector3Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_VECTOR3_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size()))); // element count
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_VECTOR3_ARRAY;
	} else if (name == "PackedColorArray") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_COLOR_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size()))); // element count
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_COLOR_ARRAY;
	} else if (name == "PackedVector4Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_VECTOR4_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size()))); // element count
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_VECTOR4_ARRAY;
	} else if (name == "Dictionary") {
		// Dictionary() - empty dictionary
		instr = IRInstruction(IROpcode::MAKE_DICTIONARY);
		instr.operands.push_back(IRValue::reg(result_reg));
		result_type = Variant::DICTIONARY;
	} else {
		// Fallback to regular CALL for unsupported constructors or wrong arg counts
		instr.operands.push_back(IRValue::str(name));
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(arg_regs.size()));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
	}

	// Set the instruction's type hint
	if (result_type != IRInstruction::TypeHint_NONE) {
		instr.type_hint = result_type;
		set_register_type(result_reg, result_type);
	}

	func.instructions.push_back(instr);

	return result_reg;
}

int CodeGenerator::gen_inline_member_get(int obj_reg, IRInstruction::TypeHint obj_type, const std::string& member, IRFunction& func) {
	int result_reg = alloc_register();

	IRInstruction instr(IROpcode::VGET_INLINE);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(IRValue::str(member));
	instr.operands.push_back(IRValue::imm(static_cast<int>(obj_type)));

	func.instructions.push_back(instr);

	// Result is always a float or int Variant
	bool is_int_vector = (obj_type == Variant::VECTOR2I ||
	                      obj_type == Variant::VECTOR3I ||
	                      obj_type == Variant::VECTOR4I);

	set_register_type(result_reg, is_int_vector ? Variant::INT : Variant::FLOAT);

	return result_reg;
}

std::unordered_set<std::string> CodeGenerator::get_global_classes() {
	// Common Godot global classes
	return {
		"AudioServer",
		"CameraServer",
		"DisplayServer",
		"NavigationServer2D",
		"NavigationServer3D",
		"PhysicsServer2D",
		"PhysicsServer3D",
		"TextServerManager",
		"ClassDB",
		"EditorInterface",
		"Engine",
		"EngineDebugger",
		"Geometry2D",
		"Geometry3D",
		"Input",
		"InputMap",
		"IP",
		"OS",
		"Performance",
		"ProjectSettings",
		"ResourceLoader",
		"ResourceSaver",
		"ThemeDB",
		"Time",
		"WorkerThreadPool",
	};
}

bool CodeGenerator::is_global_class(const std::string& name) const {
	static const auto global_classes = get_global_classes();
	return global_classes.find(name) != global_classes.end();
}

bool CodeGenerator::is_local_function(const std::string& name) const {
	return m_local_functions.find(name) != m_local_functions.end();
}

bool CodeGenerator::is_global_variable(const std::string& name) const {
	return m_global_variables.find(name) != m_global_variables.end();
}

int CodeGenerator::gen_global_class_get(const std::string& class_name, IRFunction& func) {
	// Generate a CALL_SYSCALL instruction to get the global class object
	// ECALL_GET_OBJ (504) takes: a0 = result pointer, a1 = class name pointer, a2 = class name length
	// Returns: a0 contains the object data

	int result_reg = alloc_register();

	// Add the class name as a string constant
	int str_idx = add_string_constant(class_name);

	// Generate CALL_SYSCALL instruction
	// Format: CALL_SYSCALL result_reg, syscall_number, string_index, string_length
	IRInstruction instr(IROpcode::CALL_SYSCALL);
	instr.operands.push_back(IRValue::reg(result_reg));              // result register
	instr.operands.push_back(IRValue::imm(504));                     // ECALL_GET_OBJ
	instr.operands.push_back(IRValue::imm(str_idx));                 // string constant index
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(class_name.length()))); // string length

	func.instructions.push_back(instr);

	// The result is an OBJECT Variant
	set_register_type(result_reg, IRInstruction::TypeHint_NONE); // Objects don't have a specific primitive type

	return result_reg;
}

} // namespace gdscript
