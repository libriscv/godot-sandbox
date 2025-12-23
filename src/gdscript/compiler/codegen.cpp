#include "codegen.h"
#include <stdexcept>
#include <sstream>
#include <cstring>

namespace gdscript {

CodeGenerator::CodeGenerator() {}

IRProgram CodeGenerator::generate(const Program& program) {
	IRProgram ir_program;

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

	declare_variable(stmt->name, reg);
}

void CodeGenerator::gen_assign(const AssignStmt* stmt, IRFunction& func) {
	int value_reg = gen_expr(stmt->value.get(), func);

	// Check if this is an indexed assignment (arr[0] = value)
	if (stmt->target) {
		// Transform arr[idx] = value to arr.set(idx, value)
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
		} else {
			throw std::runtime_error("Invalid assignment target type");
		}
	}

	// Simple variable assignment
	Variable* var = find_variable(stmt->name);
	if (!var) {
		throw std::runtime_error("Undefined variable: " + stmt->name);
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
	// For now, we only support range() calls
	// Convert to:
	//   var _iter = iterable  (evaluate range())
	//   var variable = 0
	//   while variable < _iter:
	//     body
	//     variable = variable + 1

	// Check if iterable is a range() call
	auto* call_expr = dynamic_cast<const CallExpr*>(stmt->iterable.get());
	if (!call_expr || call_expr->function_name != "range") {
		throw std::runtime_error("For loops currently only support range() - general iterables not yet implemented");
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
		start_instr.type_hint = IRInstruction::TypeHint::RAW_INT;
		end_reg = gen_expr(call_expr->arguments[0].get(), func);
		// Mark the last instruction (which produces end_reg) as RAW_INT
		if (!func.instructions.empty()) {
			func.instructions.back().type_hint = IRInstruction::TypeHint::RAW_INT;
		}
		step_reg = alloc_register();
		auto& step_instr = func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(step_reg), IRValue::imm(1));
		step_instr.type_hint = IRInstruction::TypeHint::RAW_INT;
	} else if (call_expr->arguments.size() == 2) {
		// range(start, end): step=1
		start_reg = gen_expr(call_expr->arguments[0].get(), func);
		// Mark start as RAW_INT
		if (!func.instructions.empty()) {
			func.instructions.back().type_hint = IRInstruction::TypeHint::RAW_INT;
		}
		end_reg = gen_expr(call_expr->arguments[1].get(), func);
		// Mark end as RAW_INT
		if (!func.instructions.empty()) {
			func.instructions.back().type_hint = IRInstruction::TypeHint::RAW_INT;
		}
		step_reg = alloc_register();
		auto& step_instr = func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(step_reg), IRValue::imm(1));
		step_instr.type_hint = IRInstruction::TypeHint::RAW_INT;
	} else if (call_expr->arguments.size() == 3) {
		// range(start, end, step)
		start_reg = gen_expr(call_expr->arguments[0].get(), func);
		// Mark start as RAW_INT
		if (!func.instructions.empty()) {
			func.instructions.back().type_hint = IRInstruction::TypeHint::RAW_INT;
		}
		end_reg = gen_expr(call_expr->arguments[1].get(), func);
		// Mark end as RAW_INT
		if (!func.instructions.empty()) {
			func.instructions.back().type_hint = IRInstruction::TypeHint::RAW_INT;
		}
		step_reg = gen_expr(call_expr->arguments[2].get(), func);
		// Mark step as RAW_INT
		if (!func.instructions.empty()) {
			func.instructions.back().type_hint = IRInstruction::TypeHint::RAW_INT;
		}
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
	move_instr.type_hint = IRInstruction::TypeHint::RAW_INT; // Loop counter is always integer from range()
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
			cmp_instr.type_hint = IRInstruction::TypeHint::RAW_INT; // Integer comparison
		} else {
			// Backward iteration: loop_var > end
			auto& cmp_instr = func.instructions.emplace_back(IROpcode::CMP_GT, IRValue::reg(cond_reg),
			                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
			cmp_instr.type_hint = IRInstruction::TypeHint::RAW_INT; // Integer comparison
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
		step_cmp.type_hint = IRInstruction::TypeHint::RAW_INT; // Integer comparison
		free_register(zero_reg);

		// If step >= 0, use loop_var < end
		func.instructions.emplace_back(IROpcode::BRANCH_NOT_ZERO, IRValue::reg(step_sign_reg),
		                               IRValue::label(pos_step_label));

		// Negative step: loop_var > end
		auto& neg_cmp = func.instructions.emplace_back(IROpcode::CMP_GT, IRValue::reg(cond_reg),
		                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
		neg_cmp.type_hint = IRInstruction::TypeHint::RAW_INT; // Integer comparison
		func.instructions.emplace_back(IROpcode::JUMP, IRValue::label(check_cond_label));

		// Positive step: loop_var < end
		func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(pos_step_label));
		auto& pos_cmp = func.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
		                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
		pos_cmp.type_hint = IRInstruction::TypeHint::RAW_INT; // Integer comparison

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
	add_instr.type_hint = IRInstruction::TypeHint::RAW_INT; // Integer addition
	auto& move_instr2 = func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(loop_var_reg), IRValue::reg(new_val_reg));
	move_instr2.type_hint = IRInstruction::TypeHint::RAW_INT; // Keep as integer
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
	} else {
		throw std::runtime_error("Unknown expression type");
	}
}

int CodeGenerator::gen_literal(const LiteralExpr* expr, IRFunction& func) {
	int reg = alloc_register();

	switch (expr->lit_type) {
		case LiteralExpr::Type::INTEGER:
			func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg),
			                               IRValue::imm(std::get<int64_t>(expr->value)));
			break;

		case LiteralExpr::Type::FLOAT: {
			// For float, we'll need to store as int64 bits for now
			double d = std::get<double>(expr->value);
			int64_t bits;
			memcpy(&bits, &d, sizeof(double));
			func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(bits));
			break;
		}

		case LiteralExpr::Type::BOOL:
			func.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(reg),
			                               IRValue::imm(std::get<bool>(expr->value) ? 1 : 0));
			break;

		case LiteralExpr::Type::STRING: {
			int str_idx = add_string_constant(std::get<std::string>(expr->value));
			func.instructions.emplace_back(IROpcode::LOAD_STRING, IRValue::reg(reg), IRValue::imm(str_idx));
			break;
		}

		case LiteralExpr::Type::NULL_VAL:
			func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
			break;
	}

	return reg;
}

int CodeGenerator::gen_variable(const VariableExpr* expr, IRFunction& func) {
	Variable* var = find_variable(expr->name);
	if (!var) {
		throw std::runtime_error("Undefined variable: " + expr->name);
	}

	// Return a copy in a new register
	int new_reg = alloc_register();
	func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(new_reg), IRValue::reg(var->register_num));
	return new_reg;
}

int CodeGenerator::gen_binary(const BinaryExpr* expr, IRFunction& func) {
	int left_reg = gen_expr(expr->left.get(), func);
	int right_reg = gen_expr(expr->right.get(), func);
	int result_reg = alloc_register();

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

	func.instructions.emplace_back(op, IRValue::reg(result_reg), IRValue::reg(left_reg), IRValue::reg(right_reg));

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

int CodeGenerator::gen_member_call(const MemberCallExpr* expr, IRFunction& func) {
	int obj_reg = gen_expr(expr->object.get(), func);

	// Generate code for arguments
	std::vector<int> arg_regs;
	for (const auto& arg : expr->arguments) {
		arg_regs.push_back(gen_expr(arg.get(), func));
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

void CodeGenerator::declare_variable(const std::string& name, int register_num) {
	if (m_scope_stack.empty()) {
		throw std::runtime_error("Cannot declare variable: no scope active");
	}

	// Check if variable already exists in current scope (shadowing is allowed, but redeclaration in same scope is not)
	auto& current_scope = m_scope_stack.back();
	if (current_scope.variables.find(name) != current_scope.variables.end()) {
		throw std::runtime_error("Variable '" + name + "' already declared in current scope");
	}

	current_scope.variables[name] = {name, register_num};
}

} // namespace gdscript
