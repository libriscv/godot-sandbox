#include "codegen.h"
#include "syscall_numbers.h"
#include "compiler_exception.h"
#include <map>
#include <stdexcept>
#include <sstream>
#include <cstring>

namespace gdscript {

static IRInstruction::TypeHint type_hint_from_string(const std::string& type_str) {
	if (type_str == "int") {
		return Variant::INT;
	} else if (type_str == "float") {
		return Variant::FLOAT;
	} else if (type_str == "bool") {
		return Variant::BOOL;
	} else if (type_str == "String") {
		return Variant::STRING;
	} else if (type_str == "StringName") {
		return Variant::STRING_NAME;
	} else if (type_str == "NodePath") {
		return Variant::NODE_PATH;
	}
	else if (type_str == "Vector2") {
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
	} else if (type_str == "Rect2") {
		return Variant::RECT2;
	} else if (type_str == "Rect2i") {
		return Variant::RECT2I;
	} else if (type_str == "Transform2D") {
		return Variant::TRANSFORM2D;
	} else if (type_str == "Transform3D") {
		return Variant::TRANSFORM3D;
	} else if (type_str == "Basis") {
		return Variant::BASIS;
	} else if (type_str == "Quaternion") {
		return Variant::QUATERNION;
	} else if (type_str == "Plane") {
		return Variant::PLANE;
	} else if (type_str == "AABB") {
		return Variant::AABB;
	} else if (type_str == "Projection") {
		return Variant::PROJECTION;
	} else if (type_str == "Color") {
		return Variant::COLOR;
	}
	else if (type_str == "Array") {
		return Variant::ARRAY;
	} else if (type_str == "Dictionary") {
		return Variant::DICTIONARY;
	}
	else if (type_str == "PackedByteArray") {
		return Variant::PACKED_BYTE_ARRAY;
	} else if (type_str == "PackedInt32Array") {
		return Variant::PACKED_INT32_ARRAY;
	} else if (type_str == "PackedInt64Array") {
		return Variant::PACKED_INT64_ARRAY;
	} else if (type_str == "PackedFloat32Array") {
		return Variant::PACKED_FLOAT32_ARRAY;
	} else if (type_str == "PackedFloat64Array") {
		return Variant::PACKED_FLOAT64_ARRAY;
	} else if (type_str == "PackedStringArray") {
		return Variant::PACKED_STRING_ARRAY;
	} else if (type_str == "PackedVector2Array") {
		return Variant::PACKED_VECTOR2_ARRAY;
	} else if (type_str == "PackedVector3Array") {
		return Variant::PACKED_VECTOR3_ARRAY;
	} else if (type_str == "PackedColorArray") {
		return Variant::PACKED_COLOR_ARRAY;
	} else if (type_str == "PackedVector4Array") {
		return Variant::PACKED_VECTOR4_ARRAY;
	}
	else if (type_str == "RID") {
		return Variant::RID;
	} else if (type_str == "Callable") {
		return Variant::CALLABLE;
	} else if (type_str == "Signal") {
		return Variant::SIGNAL;
	}

	return IRInstruction::TypeHint_NONE;
}

CodeGenerator::CodeGenerator() {}

IRProgram CodeGenerator::generate(const Program& program) {
	IRProgram ir_program;

	// Collect local function names and signatures for default-argument filling at call sites.
	m_local_functions.clear();
	m_local_signatures.clear();
	for (const auto& func : program.functions) {
		m_local_functions.insert(func.name);
		m_local_signatures[func.name] = &func;
	}

	// Collected before lowering so declaration order does not matter.
	m_structs.clear();
	m_struct_default_stack.clear();
	for (const auto& decl : program.structs) {
		if (m_structs.count(decl.name)) {
			error_at("Struct '" + decl.name + "' is declared more than once", decl.line, decl.column);
		}
		if (is_global_class(decl.name)) {
			error_at("Struct '" + decl.name + "' has the name of a Godot singleton",
				decl.line, decl.column,
				"Pick another name, so that '" + decl.name + "' still reaches the singleton");
		}
		if (m_local_functions.count(decl.name)) {
			error_at("Struct '" + decl.name + "' has the name of a function in this script",
				decl.line, decl.column);
		}
		m_structs[decl.name] = &decl;
	}

	// Enums resolve to integers here; nothing reaches the IR.
	m_enums.clear();
	m_enum_members.clear();
	for (const auto& decl : program.enums) {
		if (!decl.name.empty()) {
			if (m_enums.count(decl.name) || m_structs.count(decl.name)) {
				error_at("Enum '" + decl.name + "' has a name that is already taken",
					decl.line, decl.column);
			}
			m_enums[decl.name] = &decl;
		}
		for (const auto& member : decl.members) {
			// Unnamed members are file-scope; named members only reachable through the enum.
			if (decl.name.empty()) {
				auto existing = m_enum_members.find(member.name);
				if (existing != m_enum_members.end() && existing->second != member.value) {
					error_at("Enum member '" + member.name + "' is declared more than once"
						" with different values", member.line, member.column);
				}
				m_enum_members[member.name] = member.value;
			}
		}
	}

	// Constants fold into InitType; everything else evaluates in global_init.
	// Anything reaching neither path is an error: silent NIL caused regressions.
	m_global_variables.clear();
	m_global_consts.clear();
	m_global_const_values.clear();
	m_global_types.clear();
	m_global_structs.clear();
	ir_program.globals.resize(program.globals.size());

	// All names registered before any initializer, so initializers can reference each other.
	for (size_t i = 0; i < program.globals.size(); i++) {
		const auto& global = program.globals[i];
		if (m_global_variables.count(global.name)) {
			error_at("Global variable '" + global.name + "' is declared more than once",
				global.line, global.column);
		}
		if (find_struct(global.name) != nullptr) {
			error_at("Global variable '" + global.name + "' has the name of a struct",
				global.line, global.column,
				"'" + global.name + "' would no longer name the struct in this script");
		}
		m_global_variables[global.name] = i;
		if (global.is_const) {
			m_global_consts.insert(global.name);
		}

		// Struct-typed global: DICTIONARY downstream, struct tracked for field checking.
		const StructDecl* global_struct = find_struct(global.type_hint);
		m_global_structs.push_back(global_struct);
		if (global_struct != nullptr) {
			m_global_types.push_back(Variant::DICTIONARY);
		} else {
			m_global_types.push_back(global.type_hint.empty()
				? IRInstruction::TypeHint_NONE
				: type_hint_from_string(global.type_hint));
		}
	}

	FunctionContext init_func;
	init_func.ir.name = "__init_globals";
	m_current_function = "global initializers";
	m_globals_lowered = 0;
	push_scope(init_func);

	for (size_t i = 0; i < program.globals.size(); i++) {
		const auto& global = program.globals[i];
		IRGlobalVar& ir_global = ir_program.globals[i];

		ir_global.name = global.name;
		ir_global.is_const = global.is_const;
		ir_global.is_property = global.is_property;

		if (!global.type_hint.empty()) {
			ir_global.type_hint = m_global_types[i];
		}

		// Without a type or initializer, stores cannot determine whether VASSIGN is needed.
		if (global.type_hint.empty() && !global.initializer) {
			error_at("Global variable '" + global.name + "' requires either a type hint or an initializer",
				global.line, global.column,
				"Add ': type' (e.g. ': Array') or an initializer (e.g. '= []'). Without one, "
				"the compiler cannot tell whether stores into it need reference counting.");
		}

		if (!global.initializer) {
			// `var a: BankAccount` — fresh instance at startup.
			if (const StructDecl* global_struct = m_global_structs[i]) {
				int reg = gen_struct_construct(*global_struct, {}, NamedArguments{}, init_func, nullptr);
				init_func.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL,
					IRValue::imm(static_cast<int64_t>(i)), IRValue::reg(reg));
				free_register(init_func, reg);
				ir_global.init_type = IRGlobalVar::InitType::RUNTIME;
				ir_global.value_type = Variant::DICTIONARY;
				ir_program.has_global_init = true;
				m_globals_lowered = i + 1;
				continue;
			}

			// `var a: Array` defaults to empty Array, not NIL.
			apply_default_initializer(ir_global, init_func, i, ir_program.has_global_init);
			m_globals_lowered = i + 1;
			continue;
		}

		{
			if (fold_global_initializer(global.initializer.get(), ir_global)) {
				coerce_folded_initializer(ir_global, global.line, global.column);
				ir_global.value_type = derive_global_value_type(ir_global);
			} else {
				// Not a compile-time constant: evaluate it at startup.
				int reg = gen_expr(global.initializer.get(), init_func);
				init_func.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL,
					IRValue::imm(static_cast<int64_t>(i)), IRValue::reg(reg));
				ir_global.init_type = IRGlobalVar::InitType::RUNTIME;
				ir_global.value_type = ir_global.type_hint != IRInstruction::TypeHint_NONE
					? ir_global.type_hint
					: get_register_type(init_func, reg);
				if (m_global_structs[i] == nullptr) {
					m_global_structs[i] = get_register_struct(init_func, reg);
				}
				free_register(init_func, reg);
				ir_program.has_global_init = true;
			}

			if (global.is_const && ir_global.init_type != IRGlobalVar::InitType::RUNTIME) {
				m_global_const_values[global.name] = ir_global;
			}
		}

		m_globals_lowered = i + 1;
	}

	if (ir_program.has_global_init) {
		init_func.ir.instructions.emplace_back(IROpcode::RETURN);
		init_func.ir.max_registers = init_func.next_register;
	} else {
		init_func.ir.instructions.clear();
		init_func.ir.max_registers = 0;
	}
	pop_scope(init_func);
	ir_program.global_init = std::move(init_func.ir);

	m_globals_lowered = SIZE_MAX;

	for (const auto& decl : program.functions) {
		ir_program.signatures.push_back(build_signature(decl));
		ir_program.functions.push_back(generate_function(decl));
	}

	ir_program.string_constants = m_string_constants;
	return ir_program;
}

IRFunction CodeGenerator::generate_function(const FunctionDecl& decl) {
	FunctionContext func;
	func.ir.name = decl.name;
	m_current_function = decl.name;

	func.return_type = decl.return_type;

	push_scope(func);

	if (decl.parameters.size() > IRFunction::MAX_PARAMETERS) {
		error_at("Function '" + decl.name + "' takes " +
			std::to_string(decl.parameters.size()) + " parameters, but at most " +
			std::to_string(IRFunction::MAX_PARAMETERS) + " can be passed",
			decl.line, decl.column,
			"Pass the extra values in an Array or Dictionary instead");
	}
	for (size_t i = 0; i < decl.parameters.size(); i++) {
		const auto& param = decl.parameters[i];
		func.ir.parameters.push_back(param.name);

		int reg = alloc_register(func);
		declare_variable(func, param.name, reg);

		apply_declared_type(reg, param.type_hint, func);
	}

	for (const auto& stmt : decl.body) {
		gen_stmt(stmt.get(), func);
	}

	if (func.ir.instructions.empty() ||
	    func.ir.instructions.back().opcode != IROpcode::RETURN) {
		func.ir.instructions.emplace_back(IROpcode::RETURN);
	}

	func.ir.max_registers = func.next_register;
	pop_scope(func);

	return std::move(func.ir);
}

void CodeGenerator::error_at(const std::string& message, int line, int column,
	const std::string& hint) const
{
	throw CompilerException(ErrorType::CODEGEN_ERROR, message, line, column,
		m_current_function, "", "", hint);
}

void CodeGenerator::error_at(const std::string& message, const Expr* expr,
	const std::string& hint) const
{
	error_at(message, expr != nullptr ? expr->line : 0, expr != nullptr ? expr->column : 0, hint);
}

void CodeGenerator::error_at(const std::string& message, const Stmt* stmt,
	const std::string& hint) const
{
	error_at(message, stmt != nullptr ? stmt->line : 0, stmt != nullptr ? stmt->column : 0, hint);
}

void CodeGenerator::gen_stmt(const Stmt* stmt, FunctionContext& func) {
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
	} else if (auto* match_stmt = dynamic_cast<const MatchStmt*>(stmt)) {
		gen_match(match_stmt, func);
	} else if (dynamic_cast<const PassStmt*>(stmt)) {
	} else if (auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
		gen_expr_stmt(expr_stmt, func);
	} else {
		error_at("This kind of statement is not supported by the compiler yet", stmt);
	}
}

void CodeGenerator::gen_var_decl(const VarDeclStmt* stmt, FunctionContext& func) {
	const StructDecl* declared_struct = find_struct(stmt->type_hint);
	int reg = -1;

	if (stmt->initializer) {
		reg = gen_expr(stmt->initializer.get(), func);
	} else if (declared_struct != nullptr) {
		reg = gen_struct_construct(*declared_struct, {}, NamedArguments{}, func, nullptr);
	} else {
		reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
	}

	if (declared_struct != nullptr) {
		const StructDecl* actual = get_register_struct(func, reg);
		if (actual != nullptr && actual != declared_struct) {
			error_at("Cannot assign a '" + actual->name + "' to variable '" + stmt->name +
				"' of type '" + declared_struct->name + "'", stmt);
		}
		const IRInstruction::TypeHint actual_type = get_register_type(func, reg);
		if (actual == nullptr && actual_type != IRInstruction::TypeHint_NONE &&
		    actual_type != Variant::DICTIONARY) {
			error_at("Cannot assign a value of type " + std::string(variant_type_name(actual_type)) +
				" to variable '" + stmt->name + "' of type '" + declared_struct->name + "'", stmt);
		}
		set_register_struct(func, reg, declared_struct);
		declare_variable(func, stmt->name, reg, stmt->is_const, stmt);
		return;
	}

	if (!stmt->type_hint.empty()) {
		IRInstruction::TypeHint type = type_hint_from_string(stmt->type_hint);
		if (type != IRInstruction::TypeHint_NONE) {
			// Coerce so the Variant payload matches the declared type.
			reg = coerce_to_declared_type(reg, type, func, "variable '" + stmt->name + "'", stmt);
			set_register_type(func, reg, type);
		}
	} else if (stmt->initializer) {
		IRInstruction::TypeHint init_type = get_register_type(func, reg);
		if (init_type != IRInstruction::TypeHint_NONE) {
			set_register_type(func, reg, init_type);
		}
	}

	declare_variable(func, stmt->name, reg, stmt->is_const, stmt);
}

void CodeGenerator::gen_assign(const AssignStmt* stmt, FunctionContext& func) {
	int value_reg = gen_expr(stmt->value.get(), func);

	if (stmt->target) {
		gen_store_to(stmt->target.get(), value_reg, func, stmt);
		return;
	}

	gen_store_to_variable(stmt->name, value_reg, func, stmt);
}

void CodeGenerator::gen_store_to_variable(const std::string& name, int value_reg,
	FunctionContext& func, const Stmt* site)
{
	// Locals shadow globals.
	Variable* var = find_variable(func, name);
	if (!var) {
		if (is_global_variable(name)) {
			if (is_global_const(name)) {
				error_at("Cannot assign to const variable: " + name, site);
			}
			size_t global_idx = m_global_variables.at(name);
			value_reg = coerce_to_declared_type(value_reg, m_global_types[global_idx], func,
				"global '" + name + "'", site);
			func.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL, IRValue::imm(global_idx), IRValue::reg(value_reg));
			free_register(func, value_reg);
			return;
		}
		error_at("Undefined variable: " + name, site,
			"Declare it with 'var " + name + " = ...' before assigning to it");
	}

	if (var->is_const) {
		error_at("Cannot assign to const variable: " + name, site);
	}

	value_reg = coerce_to_declared_type(value_reg, get_register_type(func, var->register_num), func,
		"variable '" + name + "'", site);

	if (var->register_num != value_reg) {
		func.ir.instructions.emplace_back(IROpcode::MOVE,
		                               IRValue::reg(var->register_num),
		                               IRValue::reg(value_reg));
	}

	free_register(func, value_reg);
}

void CodeGenerator::gen_store_to(const Expr* target, int value_reg, FunctionContext& func,
	const Stmt* site)
{
	if (auto* var_expr = dynamic_cast<const VariableExpr*>(target)) {
		gen_store_to_variable(var_expr->name, value_reg, func, site);
		return;
	}

	if (auto* index_expr = dynamic_cast<const IndexExpr*>(target)) {
		// Handles need no write-back, but the container may be a copy: resolve as lvalue.
		LValue base = resolve_lvalue(index_expr->object.get(), func);
		int idx_reg = gen_expr(index_expr->index.get(), func);
		gen_element_store(base.reg, idx_reg, value_reg, func);
		free_register(func, idx_reg);
		free_register(func, value_reg);
		free_lvalue(base, func);
		return;
	}

	if (auto* member_expr = dynamic_cast<const MemberCallExpr*>(target)) {
		if (member_expr->is_method_call) {
			error_at("Cannot assign to method call", site);
		}

		LValue base = resolve_lvalue(member_expr->object.get(), func);

		// Struct field: coerce to declared type.
		if (const StructDecl* decl = get_register_struct(func, base.reg)) {
			const StructField& field = require_struct_field(*decl, member_expr->member_name,
				member_expr->line, member_expr->column);
			if (!field.type_hint.empty()) {
				value_reg = coerce_to_declared_type(value_reg,
					type_hint_from_string(field.type_hint), func,
					"field '" + field.name + "' of struct '" + decl->name + "'", site);
			}
		}

		const bool mutated_copy = gen_member_store(base.reg, member_expr->member_name, value_reg, func);
		free_register(func, value_reg);

		// Value type: write the mutated copy back.
		if (mutated_copy) {
			store_lvalue(base, base.reg, func, site);
		}

		free_lvalue(base, func);
		return;
	}

	error_at("Invalid assignment target type", site);
}

// Track origin of result for write-back. Each chain link evaluated once.
CodeGenerator::LValue CodeGenerator::resolve_lvalue(const Expr* expr, FunctionContext& func) {
	LValue lvalue;

	if (auto* var_expr = dynamic_cast<const VariableExpr*>(expr)) {
		if (Variable* var = find_variable(func, var_expr->name)) {
			// Own register: member store mutates in place.
			lvalue.kind = var->is_const ? LValue::Kind::VALUE : LValue::Kind::LOCAL;
			lvalue.reg = var->register_num;
			lvalue.name = var_expr->name;
			lvalue.borrowed = true;
			return lvalue;
		}
		if (is_global_variable(var_expr->name) && !is_global_const(var_expr->name)) {
			lvalue.kind = LValue::Kind::GLOBAL;
			lvalue.reg = gen_expr(expr, func);
			lvalue.name = var_expr->name;
			return lvalue;
		}
		lvalue.reg = gen_expr(expr, func);
		return lvalue;
	}

	if (auto* member_expr = dynamic_cast<const MemberCallExpr*>(expr)) {
		if (!member_expr->is_method_call && member_expr->arguments.empty()) {
			auto container = std::make_shared<LValue>(resolve_lvalue(member_expr->object.get(), func));
			lvalue.kind = LValue::Kind::MEMBER;
			lvalue.name = member_expr->member_name;
			lvalue.reg = gen_member_read(container->reg, member_expr->member_name, func);
			lvalue.container = std::move(container);
			return lvalue;
		}
	}

	if (auto* index_expr = dynamic_cast<const IndexExpr*>(expr)) {
		auto container = std::make_shared<LValue>(resolve_lvalue(index_expr->object.get(), func));
		lvalue.kind = LValue::Kind::INDEX;
		lvalue.index_reg = gen_expr(index_expr->index.get(), func);
		lvalue.reg = gen_element_read(container->reg, lvalue.index_reg, func);
		lvalue.container = std::move(container);
		return lvalue;
	}

	// Temporary with no write-back target (Object handles mutate through the handle).
	lvalue.reg = gen_expr(expr, func);
	return lvalue;
}

void CodeGenerator::store_lvalue(const LValue& target, int value_reg, FunctionContext& func,
	const Stmt* site)
{
	switch (target.kind) {
		case LValue::Kind::LOCAL:
			// Registers usually match; MOVE only when they diverge.
			if (target.reg != value_reg) {
				func.ir.instructions.emplace_back(IROpcode::MOVE,
					IRValue::reg(target.reg), IRValue::reg(value_reg));
			}
			return;

		case LValue::Kind::GLOBAL: {
			size_t global_idx = m_global_variables.at(target.name);
			func.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL,
				IRValue::imm(global_idx), IRValue::reg(value_reg));
			return;
		}

		case LValue::Kind::MEMBER: {
			const bool mutated_copy = gen_member_store(target.container->reg, target.name, value_reg, func);
			if (mutated_copy) {
				store_lvalue(*target.container, target.container->reg, func, site);
			}
			return;
		}

		case LValue::Kind::INDEX:
			gen_element_store(target.container->reg, target.index_reg, value_reg, func);
			return;

		case LValue::Kind::VALUE:
			// No write-back target.
			return;
	}
}

void CodeGenerator::free_lvalue(const LValue& lvalue, FunctionContext& func) {
	if (lvalue.index_reg >= 0) {
		free_register(func, lvalue.index_reg);
	}
	if (lvalue.reg >= 0 && !lvalue.borrowed) {
		free_register(func, lvalue.reg);
	}
	if (lvalue.container) {
		free_lvalue(*lvalue.container, func);
	}
}

void CodeGenerator::gen_return(const ReturnStmt* stmt, FunctionContext& func) {
	if (stmt->value) {
		int reg = gen_expr(stmt->value.get(), func);
		// Coerce to declared return type before moving to r0.
		reg = coerce_to_declared_type(reg, type_hint_from_string(func.return_type), func,
			"the return value of '" + m_current_function + "'", stmt);
		if (reg != 0) {
			func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(0), IRValue::reg(reg));
		}
		free_register(func, reg);
	}

	func.ir.instructions.emplace_back(IROpcode::RETURN);
}

void CodeGenerator::emit_conditional_branch(IROpcode opcode, int cond_reg,
	const std::string& label, FunctionContext& func)
{
	// Type travels with the branch for inline truthiness in the backend.
	IRInstruction branch(opcode, IRValue::reg(cond_reg), IRValue::label(label));
	branch.type_hint = get_register_type(func, cond_reg);
	func.ir.instructions.push_back(branch);
}

void CodeGenerator::gen_if(const IfStmt* stmt, FunctionContext& func) {
	std::string else_label = make_label("else");
	std::string end_label = make_label("endif");
	int cond_reg = gen_expr(stmt->condition.get(), func);
	if (!stmt->else_branch.empty()) {
		emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, else_label, func);
	} else {
		emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
	}

	free_register(func, cond_reg);

	push_scope(func);
	for (const auto& s : stmt->then_branch) {
		gen_stmt(s.get(), func);
	}
	pop_scope(func);

	if (!stmt->else_branch.empty()) {
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(end_label));

		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(else_label));
		push_scope(func);
		for (const auto& s : stmt->else_branch) {
			gen_stmt(s.get(), func);
		}
		pop_scope(func);
	}

	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));
}

static constexpr size_t MIN_SWITCH_CASES = 4;
static constexpr size_t MAX_SWITCH_SPREAD = 3;
static constexpr int64_t MAX_SWITCH_ENTRIES = 4096;

bool CodeGenerator::gen_match_jump_table(const MatchStmt* stmt, int subject_reg,
                                         const std::vector<std::string>& body_labels,
                                         const std::string& default_label,
                                         FunctionContext& func) {
	// Every pattern must be an integer constant; non-integer patterns disqualify the table.
	std::vector<std::pair<int64_t, size_t>> cases;
	for (size_t i = 0; i < stmt->branches.size(); i++) {
		const auto& branch = stmt->branches[i];
		if (branch.is_catch_all()) {
			continue;
		}
		// Guards disqualify: an entry jumping to the body would skip the guard.
		if (branch.guard) {
			return false;
		}
		for (const auto& pattern : branch.patterns) {
			if (pattern->kind != MatchPattern::Kind::VALUE) {
				return false;
			}
			IRGlobalVar folded;
			if (!fold_global_initializer(pattern->value.get(), folded) ||
			    folded.init_type != IRGlobalVar::InitType::INT) {
				return false;
			}
			cases.emplace_back(std::get<int64_t>(folded.init_value), i);
		}
	}
	if (cases.size() < MIN_SWITCH_CASES) {
		return false;
	}

	std::map<int64_t, size_t> first_branch;
	for (const auto& [value, branch] : cases) {
		first_branch.emplace(value, branch);
	}

	const int64_t low = first_branch.begin()->first;
	const int64_t high = first_branch.rbegin()->first;
	const uint64_t span = static_cast<uint64_t>(high) - static_cast<uint64_t>(low) + 1;
	if (span > static_cast<uint64_t>(MAX_SWITCH_ENTRIES) ||
	    span > first_branch.size() * MAX_SWITCH_SPREAD) {
		return false;
	}

	IRInstruction table(IROpcode::SWITCH, IRValue::reg(subject_reg), IRValue::imm(low),
	                    IRValue::imm(static_cast<int64_t>(span)));
	if (get_register_type(func, subject_reg) == Variant::INT) {
		table.type_hint = Variant::INT;
	}
	for (uint64_t i = 0; i < span; i++) {
		auto it = first_branch.find(low + static_cast<int64_t>(i));
		table.operands.push_back(IRValue::label(
			it == first_branch.end() ? default_label : body_labels[it->second]));
	}
	func.ir.instructions.push_back(table);
	return true;
}

void CodeGenerator::gen_match(const MatchStmt* stmt, FunctionContext& func) {
	// Jump table for dense integer patterns, then a compare chain for the rest.
	// The table falls through for non-integer or out-of-range subjects.
	const std::string end_label = make_label("endmatch");

	const int subject_reg = gen_expr(stmt->subject.get(), func);

	std::vector<std::string> test_labels;
	std::vector<std::string> body_labels;
	test_labels.reserve(stmt->branches.size());
	body_labels.reserve(stmt->branches.size());
	for (size_t i = 0; i < stmt->branches.size(); i++) {
		test_labels.push_back(make_label("match_test"));
		body_labels.push_back(make_label("match_body"));
	}

	size_t catch_all = stmt->branches.size();
	for (size_t i = 0; i < stmt->branches.size(); i++) {
		if (stmt->branches[i].is_catch_all()) {
			catch_all = i;
			break;
		}
	}
	const std::string& default_label =
		catch_all < stmt->branches.size() ? body_labels[catch_all] : end_label;

	const bool has_table = gen_match_jump_table(stmt, subject_reg, body_labels, default_label, func);
	// Known INT subject: the table decides the whole match.
	const bool table_is_complete = has_table && get_register_type(func, subject_reg) == Variant::INT;
	if (table_is_complete) {
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(default_label));
	}

	for (size_t i = 0; i < stmt->branches.size(); i++) {
		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(test_labels[i]));

		push_scope(func);
		if (!table_is_complete) {
			const std::string& next_label =
				i + 1 < stmt->branches.size() ? test_labels[i + 1] : end_label;
			gen_branch_test(stmt->branches[i], subject_reg, next_label, func);
		}

		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(body_labels[i]));
		for (const auto& body_stmt : stmt->branches[i].body) {
			gen_stmt(body_stmt.get(), func);
		}
		pop_scope(func);
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(end_label));
	}

	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));
	free_register(func, subject_reg);
}

void CodeGenerator::gen_branch_test(const MatchStmt::Branch& branch, int subject_reg,
                                    const std::string& fail_label, FunctionContext& func) {
	if (branch.patterns.size() == 1) {
		gen_pattern_test(*branch.patterns[0], subject_reg, fail_label, func);
	} else {
		const std::string matched_label = make_label("match_any");
		for (size_t i = 0; i < branch.patterns.size(); i++) {
			const bool last = i + 1 == branch.patterns.size();
			if (last) {
				gen_pattern_test(*branch.patterns[i], subject_reg, fail_label, func);
				break;
			}
			const std::string next_pattern = make_label("match_or");
			gen_pattern_test(*branch.patterns[i], subject_reg, next_pattern, func);
			func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(matched_label));
			func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(next_pattern));
		}
		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(matched_label));
	}

	if (branch.guard) {
		int guard_reg = gen_expr(branch.guard.get(), func);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, guard_reg, fail_label, func);
		free_register(func, guard_reg);
	}
}

void CodeGenerator::gen_pattern_test(const MatchPattern& pattern, int subject_reg,
                                     const std::string& fail_label, FunctionContext& func) {
	switch (pattern.kind) {
		case MatchPattern::Kind::WILDCARD:
			return;

		case MatchPattern::Kind::BIND: {
			// Own register: assignment in the body must not write back into the subject.
			int bound_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(bound_reg),
				IRValue::reg(subject_reg));
			set_register_type(func, bound_reg, get_register_type(func, subject_reg));
			declare_variable(func, pattern.name, bound_reg);
			return;
		}

		case MatchPattern::Kind::VALUE: {
			int pattern_reg = gen_expr(pattern.value.get(), func);
			IRInstruction cmp(IROpcode::CMP_EQ, IRValue::reg(pattern_reg),
			                  IRValue::reg(subject_reg), IRValue::reg(pattern_reg));
			cmp.type_hint = fused_compare_type(get_register_type(func, subject_reg),
			                                   get_register_type(func, pattern_reg));
			func.ir.instructions.push_back(cmp);
			set_register_type(func, pattern_reg, Variant::BOOL);
			emit_conditional_branch(IROpcode::BRANCH_ZERO, pattern_reg, fail_label, func);
			free_register(func, pattern_reg);
			return;
		}

		case MatchPattern::Kind::ARRAY:
			gen_array_pattern_test(pattern, subject_reg, fail_label, func);
			return;

		case MatchPattern::Kind::DICTIONARY:
			gen_dictionary_pattern_test(pattern, subject_reg, fail_label, func);
			return;
	}
}

void CodeGenerator::gen_array_pattern_test(const MatchPattern& pattern, int subject_reg,
                                           const std::string& fail_label, FunctionContext& func) {
	// Type, length, then elements. Length tested before any element fetch.
	if (!emit_type_guard(subject_reg, Variant::ARRAY, fail_label, func)) {
		return;
	}

	const int size_reg = gen_array_size(subject_reg, func);
	const int wanted_reg = gen_int_immediate(static_cast<int64_t>(pattern.elements.size()), func);
	const int fits_reg = gen_compare(pattern.open ? IROpcode::CMP_GTE : IROpcode::CMP_EQ,
		size_reg, wanted_reg, func);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, fits_reg, fail_label, func);
	free_register(func, fits_reg);
	free_register(func, wanted_reg);
	free_register(func, size_reg);

	for (size_t i = 0; i < pattern.elements.size(); i++) {
		int index_reg = gen_int_immediate(static_cast<int64_t>(i), func);
		int element_reg = gen_array_element(subject_reg, index_reg, func);
		gen_pattern_test(*pattern.elements[i], element_reg, fail_label, func);
		free_register(func, element_reg);
		free_register(func, index_reg);
	}
}

void CodeGenerator::gen_dictionary_pattern_test(const MatchPattern& pattern, int subject_reg,
                                                const std::string& fail_label, FunctionContext& func) {
	constexpr int64_t DICT_OP_GET = 0;
	constexpr int64_t DICT_OP_HAS = 3;
	constexpr int64_t DICT_OP_GET_SIZE = 6;

	if (!emit_type_guard(subject_reg, Variant::DICTIONARY, fail_label, func)) {
		return;
	}

	if (!pattern.open) {
		const int size_reg = gen_dictionary_op(DICT_OP_GET_SIZE, subject_reg, -1, Variant::INT, func);
		const int wanted_reg = gen_int_immediate(static_cast<int64_t>(pattern.entries.size()), func);
		const int fits_reg = gen_compare(IROpcode::CMP_EQ, size_reg, wanted_reg, func);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, fits_reg, fail_label, func);
		free_register(func, fits_reg);
		free_register(func, wanted_reg);
		free_register(func, size_reg);
	}

	for (const auto& entry : pattern.entries) {
		int key_reg = gen_expr(entry.key.get(), func);

		int has_reg = gen_dictionary_op(DICT_OP_HAS, subject_reg, key_reg, Variant::BOOL, func);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, has_reg, fail_label, func);
		free_register(func, has_reg);

		if (entry.value) {
			int value_reg = gen_dictionary_op(DICT_OP_GET, subject_reg, key_reg,
				IRInstruction::TypeHint_NONE, func);
			gen_pattern_test(*entry.value, value_reg, fail_label, func);
			free_register(func, value_reg);
		}

		free_register(func, key_reg);
	}
}

bool CodeGenerator::emit_type_guard(int value_reg, IRInstruction::TypeHint type,
                                    const std::string& fail_label, FunctionContext& func) {
	const IRInstruction::TypeHint known = get_register_type(func, value_reg);
	if (known == type) {
		return true;
	}
	if (known != IRInstruction::TypeHint_NONE) {
		// Known different type: unconditional jump, no destructuring emitted.
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(fail_label));
		return false;
	}

	int test_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(test_reg),
		IRValue::reg(value_reg), IRValue::imm(static_cast<int64_t>(type)));
	set_register_type(func, test_reg, Variant::BOOL);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, test_reg, fail_label, func);
	free_register(func, test_reg);
	return true;
}

int CodeGenerator::gen_array_size(int array_reg, FunctionContext& func) {
	int size_reg = alloc_register(func);
	IRInstruction call(IROpcode::CALL_SYSCALL);
	call.operands.push_back(IRValue::reg(size_reg));
	call.operands.push_back(IRValue::imm(ECALL_ARRAY_SIZE));
	call.operands.push_back(IRValue::reg(array_reg));
	func.ir.instructions.push_back(call);
	set_register_type(func, size_reg, Variant::INT);
	return size_reg;
}

int CodeGenerator::gen_array_element(int array_reg, int index_reg, FunctionContext& func) {
	int element_reg = alloc_register(func);
	IRInstruction call(IROpcode::CALL_SYSCALL);
	call.operands.push_back(IRValue::reg(element_reg));
	call.operands.push_back(IRValue::imm(ECALL_ARRAY_AT));
	call.operands.push_back(IRValue::reg(array_reg));
	call.operands.push_back(IRValue::reg(index_reg));
	func.ir.instructions.push_back(call);
	return element_reg;
}

int CodeGenerator::gen_dictionary_op(int64_t op, int dict_reg, int key_reg,
                                     IRInstruction::TypeHint result_type, FunctionContext& func) {
	int result_reg = alloc_register(func);
	IRInstruction call(IROpcode::CALL_SYSCALL);
	call.operands.push_back(IRValue::reg(result_reg));
	call.operands.push_back(IRValue::imm(ECALL_DICTIONARY_OPS));
	call.operands.push_back(IRValue::imm(op));
	call.operands.push_back(IRValue::reg(dict_reg));
	if (key_reg >= 0) {
		call.operands.push_back(IRValue::reg(key_reg));
	}
	func.ir.instructions.push_back(call);
	if (result_type != IRInstruction::TypeHint_NONE) {
		set_register_type(func, result_reg, result_type);
	}
	return result_reg;
}

int CodeGenerator::gen_compare(IROpcode opcode, int left_reg, int right_reg, FunctionContext& func) {
	int result_reg = alloc_register(func);
	IRInstruction cmp(opcode, IRValue::reg(result_reg), IRValue::reg(left_reg),
	                  IRValue::reg(right_reg));
	cmp.type_hint = fused_compare_type(get_register_type(func, left_reg),
	                                   get_register_type(func, right_reg));
	func.ir.instructions.push_back(cmp);
	set_register_type(func, result_reg, Variant::BOOL);
	return result_reg;
}

void CodeGenerator::gen_while(const WhileStmt* stmt, FunctionContext& func) {
	std::string loop_label = make_label("loop");
	std::string end_label = make_label("endloop");

	func.loops.push_back({end_label, loop_label});
	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(loop_label));
	int cond_reg = gen_expr(stmt->condition.get(), func);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
	free_register(func, cond_reg);

	push_scope(func);
	for (const auto& s : stmt->body) {
		gen_stmt(s.get(), func);
	}
	pop_scope(func);
	func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(loop_label));
	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));

	func.loops.pop_back();
}

void CodeGenerator::gen_for(const ForStmt* stmt, FunctionContext& func) {
	auto* call_expr = dynamic_cast<const CallExpr*>(stmt->iterable.get());
	bool is_range = call_expr && call_expr->function_name == "range";

	auto* literal = dynamic_cast<const LiteralExpr*>(stmt->iterable.get());
	if (literal) {
		if (literal->lit_type == LiteralExpr::Type::INTEGER ||
		    literal->lit_type == LiteralExpr::Type::FLOAT ||
		    literal->lit_type == LiteralExpr::Type::BOOL ||
		    literal->lit_type == LiteralExpr::Type::NULL_VAL) {
			error_at("Cannot iterate over a non-iterable value in a 'for' loop", stmt,
				"Did you mean 'for " + stmt->variable + " in range(" +
				(literal->lit_type == LiteralExpr::Type::INTEGER ?
				 std::to_string(std::get<int64_t>(literal->value)) : "N") + "):'?");
		}
	}

	if (!is_range) {
		std::string loop_label = make_label("for_loop");
		std::string continue_label = make_label("for_continue");
		std::string end_label = make_label("for_end");

		func.loops.push_back({end_label, continue_label});
		push_scope(func);

		int array_reg = gen_expr(stmt->iterable.get(), func);

		// Packed arrays use VCALL size()/get(); ECALL_ARRAY_SIZE/AT are Array-only.
		const bool packed_walk = is_packed_array_type(get_register_type(func, array_reg));

		if (get_register_type(func, array_reg) == Variant::STRING) {
			error_at("Cannot iterate over a String yet", stmt,
				"String has no get(); walking one needs a host syscall that yields"
				" its characters");
		}

		// Dictionary: replaced by its keys array once before the loop.
		if (!packed_walk) {
			gen_dictionary_keys_for_iteration(array_reg, func);
		}

		int index_reg = alloc_register(func);
		auto& index_load = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(index_reg), IRValue::imm(0));
		index_load.type_hint = Variant::INT;
		set_register_type(func, index_reg, Variant::INT);

		int one_reg = alloc_register(func);
		auto& one_load = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(one_reg), IRValue::imm(1));
		one_load.type_hint = Variant::INT;
		set_register_type(func, one_reg, Variant::INT);

		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(loop_label));
		int size_reg = alloc_register(func);
		if (packed_walk) {
			IRInstruction size_call(IROpcode::VCALL);
			size_call.operands.push_back(IRValue::reg(size_reg));
			size_call.operands.push_back(IRValue::reg(array_reg));
			size_call.operands.push_back(IRValue::str("size"));
			size_call.operands.push_back(IRValue::imm(0));
			func.ir.instructions.push_back(size_call);
		} else {
			IRInstruction size_syscall(IROpcode::CALL_SYSCALL);
			size_syscall.operands.push_back(IRValue::reg(size_reg));
			size_syscall.operands.push_back(IRValue::imm(ECALL_ARRAY_SIZE));
			size_syscall.operands.push_back(IRValue::reg(array_reg));
			func.ir.instructions.push_back(size_syscall);
		}
		set_register_type(func, size_reg, Variant::INT);

		int cond_reg = alloc_register(func);
		auto& cmp_instr = func.ir.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
		                               IRValue::reg(index_reg), IRValue::reg(size_reg));
		cmp_instr.type_hint = Variant::INT;
		set_register_type(func, cond_reg, Variant::BOOL);

		emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
		free_register(func, cond_reg);

		int elem_reg = alloc_register(func);
		if (packed_walk) {
			IRInstruction at_call(IROpcode::VCALL);
			at_call.operands.push_back(IRValue::reg(elem_reg));
			at_call.operands.push_back(IRValue::reg(array_reg));
			at_call.operands.push_back(IRValue::str("get"));
			at_call.operands.push_back(IRValue::imm(1));
			at_call.operands.push_back(IRValue::reg(index_reg));
			func.ir.instructions.push_back(at_call);
		} else {
			IRInstruction at_syscall(IROpcode::CALL_SYSCALL);
			at_syscall.operands.push_back(IRValue::reg(elem_reg));
			at_syscall.operands.push_back(IRValue::imm(ECALL_ARRAY_AT));
			at_syscall.operands.push_back(IRValue::reg(array_reg));
			at_syscall.operands.push_back(IRValue::reg(index_reg));
			func.ir.instructions.push_back(at_syscall);
		}

		declare_variable(func, stmt->variable, elem_reg, false, stmt);
		push_scope(func);
		for (const auto& s : stmt->body) {
			gen_stmt(s.get(), func);
		}
		pop_scope(func);

		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(continue_label));
		int new_idx_reg = alloc_register(func);
		auto& add_instr = func.ir.instructions.emplace_back(IROpcode::ADD, IRValue::reg(new_idx_reg),
		                               IRValue::reg(index_reg), IRValue::reg(one_reg));
		add_instr.type_hint = Variant::INT;
		set_register_type(func, new_idx_reg, Variant::INT);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(index_reg), IRValue::reg(new_idx_reg));
		free_register(func, new_idx_reg);

		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(loop_label));
		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));
		pop_scope(func);
		func.loops.pop_back();
		free_register(func, one_reg);
		free_register(func, index_reg);
		free_register(func, elem_reg);
		return;
	}

	int start_reg = -1, end_reg = -1, step_reg = -1;

	if (call_expr->arguments.size() == 1) {
		start_reg = gen_int_immediate(0, func);
		end_reg = gen_expr(call_expr->arguments[0].get(), func);
		step_reg = gen_int_immediate(1, func);
	} else if (call_expr->arguments.size() == 2) {
		start_reg = gen_expr(call_expr->arguments[0].get(), func);
		end_reg = gen_expr(call_expr->arguments[1].get(), func);
		step_reg = gen_int_immediate(1, func);
	} else if (call_expr->arguments.size() == 3) {
		start_reg = gen_expr(call_expr->arguments[0].get(), func);
		end_reg = gen_expr(call_expr->arguments[1].get(), func);
		step_reg = gen_expr(call_expr->arguments[2].get(), func);
	} else {
		error_at("range() takes 1, 2, or 3 arguments, got " +
			std::to_string(call_expr->arguments.size()), call_expr);
	}

	std::string loop_label = make_label("for_loop");
	std::string continue_label = make_label("for_continue");
	std::string end_label = make_label("for_end");

	func.loops.push_back({end_label, continue_label});
	push_scope(func);
	int loop_var_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(loop_var_reg), IRValue::reg(start_reg));
	declare_variable(func, stmt->variable, loop_var_reg, false, stmt);

	// INT when both start and step are INT; end only bounds the loop.
	if (get_register_type(func, start_reg) == Variant::INT &&
		get_register_type(func, step_reg) == Variant::INT) {
		set_register_type(func, loop_var_reg, Variant::INT);
	}

	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(loop_label));

	int cond_reg = alloc_register(func);
	bool step_is_constant = false;
	int64_t step_value = 0;
	if (func.ir.instructions.size() >= 3) {
		auto& prev_instr = func.ir.instructions[func.ir.instructions.size() - 3];
		if (prev_instr.opcode == IROpcode::LOAD_IMM &&
		    std::get<int>(prev_instr.operands[0].value) == step_reg) {
			step_is_constant = true;
			step_value = std::get<int64_t>(prev_instr.operands[1].value);
		}
	}

	if (step_is_constant) {
		if (step_value >= 0) {
			auto& cmp_instr = func.ir.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
			                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
			cmp_instr.type_hint = Variant::INT;
		} else {
			auto& cmp_instr = func.ir.instructions.emplace_back(IROpcode::CMP_GT, IRValue::reg(cond_reg),
			                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
			cmp_instr.type_hint = Variant::INT;
		}
	} else {
		std::string pos_step_label = make_label("for_pos_step");
		std::string check_cond_label = make_label("for_check_cond");

		int zero_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(zero_reg), IRValue::imm(0));

		int step_sign_reg = alloc_register(func);
		auto& step_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_GTE, IRValue::reg(step_sign_reg),
		                               IRValue::reg(step_reg), IRValue::reg(zero_reg));
		step_cmp.type_hint = Variant::INT;
		free_register(func, zero_reg);
		emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, step_sign_reg, pos_step_label, func);

		auto& neg_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_GT, IRValue::reg(cond_reg),
		                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
		neg_cmp.type_hint = Variant::INT;
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(check_cond_label));

		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(pos_step_label));
		auto& pos_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
		                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
		pos_cmp.type_hint = Variant::INT;

		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(check_cond_label));
		free_register(func, step_sign_reg);
	}

	emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
	free_register(func, cond_reg);

	push_scope(func);
	for (const auto& s : stmt->body) {
		gen_stmt(s.get(), func);
	}
	pop_scope(func);

	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(continue_label));

	int new_val_reg = alloc_register(func);
	auto& add_instr = func.ir.instructions.emplace_back(IROpcode::ADD, IRValue::reg(new_val_reg),
	                               IRValue::reg(loop_var_reg), IRValue::reg(step_reg));
	add_instr.type_hint = Variant::INT;
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(loop_var_reg), IRValue::reg(new_val_reg));
	free_register(func, new_val_reg);

	func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(loop_label));
	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));

	pop_scope(func);
	func.loops.pop_back();
	free_register(func, start_reg);
	free_register(func, end_reg);
	free_register(func, step_reg);
}

void CodeGenerator::gen_break(const BreakStmt* stmt, FunctionContext& func) {
	if (func.loops.empty()) {
		error_at("'break' outside of loop", stmt);
	}

	func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(func.loops.back().break_label));
}

void CodeGenerator::gen_continue(const ContinueStmt* stmt, FunctionContext& func) {
	if (func.loops.empty()) {
		error_at("'continue' outside of loop", stmt);
	}

	func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(func.loops.back().continue_label));
}

void CodeGenerator::gen_expr_stmt(const ExprStmt* stmt, FunctionContext& func) {
	int reg = gen_expr(stmt->expression.get(), func);
	free_register(func, reg);
}

int CodeGenerator::gen_expr(const Expr* expr, FunctionContext& func) {
	if (auto* lit = dynamic_cast<const LiteralExpr*>(expr)) {
		return gen_literal(lit, func);
	} else if (auto* var = dynamic_cast<const VariableExpr*>(expr)) {
		return gen_variable(var, func);
	} else if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
		return gen_binary(bin, func);
	} else if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
		return gen_unary(un, func);
	} else if (auto* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
		return gen_ternary(ternary, func);
	} else if (auto* type_test = dynamic_cast<const TypeTestExpr*>(expr)) {
		return gen_type_test(type_test, func);
	} else if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
		return gen_call(call, func);
	} else if (auto* member = dynamic_cast<const MemberCallExpr*>(expr)) {
		return gen_member_call(member, func);
	} else if (auto* index = dynamic_cast<const IndexExpr*>(expr)) {
		return gen_index(index, func);
	} else if (auto* array_lit = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
		return gen_array_literal(array_lit, func);
	} else if (auto* dict_lit = dynamic_cast<const DictionaryLiteralExpr*>(expr)) {
		return gen_dictionary_literal(dict_lit, func);
	} else {
		// Every expression kind must be lowered above; a default register would silently mis-compile.
		error_at("This kind of expression is not supported by the compiler yet", expr);
	}
}

void CodeGenerator::gen_dictionary_keys_for_iteration(int iterable_reg, FunctionContext& func) {
	// GET_KEYS: result in a2, not a3 (no key argument).
	constexpr int64_t DICT_OP_GET_KEYS = 4;

	auto emit_get_keys = [&]() {
		int keys_reg = alloc_register(func);
		IRInstruction keys(IROpcode::CALL_SYSCALL);
		keys.operands.push_back(IRValue::reg(keys_reg));
		keys.operands.push_back(IRValue::imm(ECALL_DICTIONARY_OPS));
		keys.operands.push_back(IRValue::imm(DICT_OP_GET_KEYS));
		keys.operands.push_back(IRValue::reg(iterable_reg));
		func.ir.instructions.push_back(keys);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(iterable_reg),
			IRValue::reg(keys_reg));
		set_register_type(func, iterable_reg, Variant::ARRAY);
		free_register(func, keys_reg);
	};

	const IRInstruction::TypeHint known = get_register_type(func, iterable_reg);
	if (known == Variant::DICTIONARY) {
		// Known Dictionary: no type test.
		emit_get_keys();
		return;
	}
	if (known != IRInstruction::TypeHint_NONE) {
		// Known to be another type, so not a Dictionary: nothing to do.
		return;
	}

	// Unknown type: one type-tag test per loop, at run time.
	const std::string skip_label = make_label("for_not_a_dict");
	int is_dict_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(is_dict_reg),
		IRValue::reg(iterable_reg), IRValue::imm(static_cast<int64_t>(Variant::DICTIONARY)));
	set_register_type(func, is_dict_reg, Variant::BOOL);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, is_dict_reg, skip_label, func);
	free_register(func, is_dict_reg);

	emit_get_keys();

	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(skip_label));
	// Join: only one path made it an Array, so type is unknown.
	set_register_type(func, iterable_reg, IRInstruction::TypeHint_NONE);
}

int CodeGenerator::gen_int_immediate(int64_t value, FunctionContext& func) {
	int reg = alloc_register(func);
	IRInstruction instr(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(value));
	instr.type_hint = Variant::INT;
	func.ir.instructions.push_back(instr);
	set_register_type(func, reg, Variant::INT);
	return reg;
}

int CodeGenerator::gen_literal(const LiteralExpr* expr, FunctionContext& func) {
	int reg = alloc_register(func);

	switch (expr->lit_type) {
		case LiteralExpr::Type::INTEGER: {
			IRInstruction instr(IROpcode::LOAD_IMM, IRValue::reg(reg),
			                    IRValue::imm(std::get<int64_t>(expr->value)));
			instr.type_hint = Variant::INT;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::INT);
			break;
		}

		case LiteralExpr::Type::FLOAT: {
			// Float literals are always 64-bit doubles in GDScript
			double d = std::get<double>(expr->value);
			IRInstruction instr(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(reg), IRValue::fimm(d));
			instr.type_hint = Variant::FLOAT;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::FLOAT);
			break;
		}

		case LiteralExpr::Type::BOOL: {
			IRInstruction instr(IROpcode::LOAD_BOOL, IRValue::reg(reg),
			                    IRValue::imm(std::get<bool>(expr->value) ? 1 : 0));
			instr.type_hint = Variant::BOOL;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::BOOL);
			break;
		}

		case LiteralExpr::Type::STRING: {
			int str_idx = add_string_constant(std::get<std::string>(expr->value));
			IRInstruction instr(IROpcode::LOAD_STRING, IRValue::reg(reg), IRValue::imm(str_idx));
			instr.type_hint = Variant::STRING;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::STRING);
			break;
		}

		case LiteralExpr::Type::NULL_VAL:
			func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
			break;
	}

	return reg;
}

int CodeGenerator::gen_variable(const VariableExpr* expr, FunctionContext& func) {
	// Locals shadow globals; 'self' is not declarable, handled below.
	if (Variable* local = find_variable(func, expr->name)) {
		int new_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(new_reg), IRValue::reg(local->register_num));

		IRInstruction::TypeHint type = get_register_type(func, local->register_num);
		if (type != IRInstruction::TypeHint_NONE) {
			set_register_type(func, new_reg, type);
		}
		// Dictionary handle: copy refers to the same fields, so propagate struct.
		set_register_struct(func, new_reg, get_register_struct(func, local->register_num));
		return new_reg;
	}

	// Unnamed enum member: compile-time integer. Local of same name shadows above.
	if (auto member = m_enum_members.find(expr->name); member != m_enum_members.end()) {
		return gen_int_immediate(member->second, func);
	}

	if (is_global_class(expr->name)) {
		return gen_global_class_get(expr->name, func);
	}

	if (expr->name == "self") {
		int result_reg = alloc_register(func);
		IRInstruction instr(IROpcode::CALL_SYSCALL);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(ECALL_GET_NODE));
		instr.operands.push_back(IRValue::imm(0));
		func.ir.instructions.push_back(instr);

		return result_reg;
	}

	// Folded const: immediate instead of LOAD_GLOBAL (carries type for match/arithmetic).
	if (int const_reg = gen_const_global_value(expr->name, func); const_reg >= 0) {
		return const_reg;
	}

	if (is_global_variable(expr->name)) {
		size_t global_idx = m_global_variables.at(expr->name);
		// Forward reference: global not yet initialized, still NIL.
		if (global_idx >= m_globals_lowered) {
			error_at("Global variable '" + expr->name + "' is used in the initializer of a global "
				"declared before it", expr,
				"Move the declaration of '" + expr->name + "' above that global");
		}
		int result_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::LOAD_GLOBAL, IRValue::reg(result_reg), IRValue::imm(global_idx));
		set_register_struct(func, result_reg, m_global_structs[global_idx]);
		// Propagate declared type so member access skips the run-time tag test.
		if (m_global_types[global_idx] != IRInstruction::TypeHint_NONE) {
			set_register_type(func, result_reg, m_global_types[global_idx]);
		}
		return result_reg;
	}

	if (const StructDecl* decl = find_struct(expr->name)) {
		error_at("Struct '" + decl->name + "' is a type, not a value", expr,
			"Create an instance with '" + decl->name + ".new()'");
	}

	error_at("Undefined variable: " + expr->name, expr,
		"Make sure '" + expr->name + "' is declared before use");
}

int CodeGenerator::gen_logical(const BinaryExpr* expr, FunctionContext& func) {
	// Short-circuit: only evaluate RHS when LHS doesn't decide the result.
	const bool is_and = expr->op == BinaryExpr::Op::AND;
	const std::string end_label = make_label(is_and ? "and_end" : "or_end");
	const IROpcode short_circuit_branch = is_and ? IROpcode::BRANCH_ZERO : IROpcode::BRANCH_NOT_ZERO;

	int result_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result_reg),
		IRValue::imm(is_and ? 0 : 1));
	set_register_type(func, result_reg, Variant::BOOL);

	int left_reg = gen_expr(expr->left.get(), func);
	IRInstruction left_branch(short_circuit_branch, IRValue::reg(left_reg), IRValue::label(end_label));
	// Type hint lets backend test truthiness inline.
	left_branch.type_hint = get_register_type(func, left_reg);
	func.ir.instructions.push_back(left_branch);
	free_register(func, left_reg);

	int right_reg = gen_expr(expr->right.get(), func);
	IRInstruction right_branch(short_circuit_branch, IRValue::reg(right_reg), IRValue::label(end_label));
	right_branch.type_hint = get_register_type(func, right_reg);
	func.ir.instructions.push_back(right_branch);
	free_register(func, right_reg);

	func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result_reg),
		IRValue::imm(is_and ? 1 : 0));
	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));

	return result_reg;
}

int CodeGenerator::gen_binary(const BinaryExpr* expr, FunctionContext& func) {
	if (expr->op == BinaryExpr::Op::AND || expr->op == BinaryExpr::Op::OR) {
		return gen_logical(expr, func);
	}

	int left_reg = gen_expr(expr->left.get(), func);
	int right_reg = gen_expr(expr->right.get(), func);
	int result_reg = alloc_register(func);

	IRInstruction::TypeHint left_type = get_register_type(func, left_reg);
	IRInstruction::TypeHint right_type = get_register_type(func, right_reg);

	bool is_arithmetic = (expr->op == BinaryExpr::Op::ADD ||
	                      expr->op == BinaryExpr::Op::SUB ||
	                      expr->op == BinaryExpr::Op::MUL ||
	                      expr->op == BinaryExpr::Op::DIV ||
	                      expr->op == BinaryExpr::Op::MOD);
	bool is_bitwise = (expr->op == BinaryExpr::Op::BIT_AND ||
	                   expr->op == BinaryExpr::Op::BIT_OR ||
	                   expr->op == BinaryExpr::Op::BIT_XOR ||
	                   expr->op == BinaryExpr::Op::SHL ||
	                   expr->op == BinaryExpr::Op::SHR);
	bool is_comparison = (expr->op == BinaryExpr::Op::EQ ||
	                      expr->op == BinaryExpr::Op::NEQ ||
	                      expr->op == BinaryExpr::Op::LT ||
	                      expr->op == BinaryExpr::Op::LTE ||
	                      expr->op == BinaryExpr::Op::GT ||
	                      expr->op == BinaryExpr::Op::GTE);

	// Same-type operands enable native codegen; mixed types fall back to VEVAL.
	IRInstruction::TypeHint result_type = IRInstruction::TypeHint_NONE;
	if (is_bitwise) {
		// Bitwise is integer-only; unknown types fall back to VEVAL for runtime error.
		if (left_type == Variant::INT && right_type == Variant::INT) {
			result_type = Variant::INT;
		}
	} else if (is_arithmetic || is_comparison) {
		if (left_type == Variant::INT && right_type == Variant::INT) {
			result_type = Variant::INT;
		} else if (left_type == Variant::FLOAT && right_type == Variant::FLOAT) {
			result_type = Variant::FLOAT;
		} else if (left_type != IRInstruction::TypeHint_NONE &&
		           right_type != IRInstruction::TypeHint_NONE &&
		           left_type == right_type &&
		           TypeHintUtils::is_vector(left_type)) {
			result_type = left_type;
		}
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
		case BinaryExpr::Op::BIT_AND: op = IROpcode::BIT_AND; break;
		case BinaryExpr::Op::BIT_OR: op = IROpcode::BIT_OR; break;
		case BinaryExpr::Op::BIT_XOR: op = IROpcode::BIT_XOR; break;
		case BinaryExpr::Op::SHL: op = IROpcode::SHL; break;
		case BinaryExpr::Op::SHR: op = IROpcode::SHR; break;
		case BinaryExpr::Op::POW: op = IROpcode::POW; break;
		case BinaryExpr::Op::IN: op = IROpcode::IN; break;
		default:
			error_at("Unknown binary operator", expr);
	}

	IRInstruction instr(op, IRValue::reg(result_reg), IRValue::reg(left_reg), IRValue::reg(right_reg));
	instr.type_hint = result_type;
	func.ir.instructions.push_back(instr);

	if (expr->op == BinaryExpr::Op::IN) {
		set_register_type(func, result_reg, Variant::BOOL);
	} else if (is_comparison) {
		// type_hint describes the operands (selects native compare); result is BOOL.
		set_register_type(func, result_reg, Variant::BOOL);
	} else if (result_type != IRInstruction::TypeHint_NONE) {
		set_register_type(func, result_reg, result_type);
	}

	free_register(func, left_reg);
	free_register(func, right_reg);

	return result_reg;
}

int CodeGenerator::gen_type_test(const TypeTestExpr* expr, FunctionContext& func) {
	const IRInstruction::TypeHint tested = type_hint_from_string(expr->type_name);
	if (tested == IRInstruction::TypeHint_NONE) {
		// Class names need an engine inheritance walk; type tag can't express it.
		error_at("'is " + expr->type_name + "' is not supported: only the built-in"
			" Variant types can be tested, not class names", expr);
	}

	int value_reg = gen_expr(expr->value.get(), func);
	int result_reg = alloc_register(func);

	// Known type folds to a constant.
	const IRInstruction::TypeHint known = get_register_type(func, value_reg);
	if (known != IRInstruction::TypeHint_NONE) {
		func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result_reg),
			IRValue::imm(known == tested ? 1 : 0));
	} else {
		func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(result_reg),
			IRValue::reg(value_reg), IRValue::imm(static_cast<int64_t>(tested)));
	}
	set_register_type(func, result_reg, Variant::BOOL);

	free_register(func, value_reg);
	return result_reg;
}

int CodeGenerator::gen_unary(const UnaryExpr* expr, FunctionContext& func) {
	int operand_reg = gen_expr(expr->operand.get(), func);
	int result_reg = alloc_register(func);

	IROpcode op;
	switch (expr->op) {
		case UnaryExpr::Op::NEG: op = IROpcode::NEG; break;
		case UnaryExpr::Op::NOT: op = IROpcode::NOT; break;
		case UnaryExpr::Op::BIT_NOT: op = IROpcode::BIT_NOT; break;
		default:
			error_at("Unknown unary operator", expr);
	}

	IRInstruction instr(op, IRValue::reg(result_reg), IRValue::reg(operand_reg));
	if (expr->op == UnaryExpr::Op::BIT_NOT && get_register_type(func, operand_reg) == Variant::INT) {
		instr.type_hint = Variant::INT;
		set_register_type(func, result_reg, Variant::INT);
	} else if (expr->op == UnaryExpr::Op::NOT) {
		set_register_type(func, result_reg, Variant::BOOL);
	} else if (expr->op == UnaryExpr::Op::NEG) {
		const IRInstruction::TypeHint operand_type = get_register_type(func, operand_reg);
		if (operand_type == Variant::INT || operand_type == Variant::FLOAT) {
			instr.type_hint = operand_type;
			set_register_type(func, result_reg, operand_type);
		}
	}
	func.ir.instructions.push_back(instr);

	free_register(func, operand_reg);
	return result_reg;
}

int CodeGenerator::gen_ternary(const TernaryExpr* expr, FunctionContext& func) {
	// Only the taken branch is evaluated.
	std::string else_label = make_label("ternary_else");
	std::string end_label = make_label("ternary_end");

	int result_reg = alloc_register(func);

	int cond_reg = gen_expr(expr->condition.get(), func);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, else_label, func);
	free_register(func, cond_reg);

	int true_reg = gen_expr(expr->true_value.get(), func);
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result_reg), IRValue::reg(true_reg));
	IRInstruction::TypeHint true_type = get_register_type(func, true_reg);
	free_register(func, true_reg);
	func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(end_label));

	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(else_label));
	int false_reg = gen_expr(expr->false_value.get(), func);
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result_reg), IRValue::reg(false_reg));
	IRInstruction::TypeHint false_type = get_register_type(func, false_reg);
	free_register(func, false_reg);

	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));

	if (true_type != IRInstruction::TypeHint_NONE && true_type == false_type) {
		set_register_type(func, result_reg, true_type);
	}

	return result_reg;
}

int CodeGenerator::gen_call(const CallExpr* expr, FunctionContext& func) {
	// Struct constructor: checked before args are lowered (only call that names them).
	if (const StructDecl* decl = find_struct(expr->function_name)) {
		return gen_struct_construct(*decl, expr->arguments, *expr, func, expr);
	}
	reject_named_arguments(*expr, "'" + expr->function_name + "'", expr);

	std::vector<int> arg_regs;
	for (const auto& arg : expr->arguments) {
		arg_regs.push_back(gen_expr(arg.get(), func));
	}

	if (is_inline_primitive_constructor(expr->function_name)) {
		int result = gen_inline_constructor(expr->function_name, arg_regs, func, expr);
		for (int reg : arg_regs) {
			free_register(func, reg);
		}
		return result;
	}

	// typeof(): guest-side tag read, not a globals.h entry.
	if (expr->function_name == "typeof") {
		if (arg_regs.size() != 1) {
			error_at("typeof() takes exactly 1 argument", expr);
		}
		int result_reg = alloc_register(func);
		IRInstruction instr(IROpcode::TYPE_OF);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::reg(arg_regs[0]));
		instr.type_hint = Variant::INT;
		func.ir.instructions.push_back(instr);
		set_register_type(func, result_reg, Variant::INT);
		free_register(func, arg_regs[0]);
		return result_reg;
	}

	if (expr->function_name == "get_node") {
		if (arg_regs.size() > 1) {
			error_at("get_node() takes at most 1 argument", expr);
		}

		int result_reg = alloc_register(func);

		if (arg_regs.empty()) {
			IRInstruction instr(IROpcode::CALL_SYSCALL);
			instr.operands.push_back(IRValue::reg(result_reg));
			instr.operands.push_back(IRValue::imm(ECALL_GET_NODE));
			instr.operands.push_back(IRValue::imm(0));
			func.ir.instructions.push_back(instr);
		} else {
			int path_reg = arg_regs[0];
			IRInstruction instr(IROpcode::CALL_SYSCALL);
			instr.operands.push_back(IRValue::reg(result_reg));
			instr.operands.push_back(IRValue::imm(ECALL_GET_NODE));
			instr.operands.push_back(IRValue::imm(0));
			instr.operands.push_back(IRValue::reg(path_reg));
			func.ir.instructions.push_back(instr);
		}

		for (int reg : arg_regs) {
			free_register(func, reg);
		}

		return result_reg;
	}

	if (is_local_function(expr->function_name)) {
		// Defaults materialized here: ABI has no argument count, callee can't distinguish.
		auto sig = m_local_signatures.find(expr->function_name);
		if (sig != m_local_signatures.end()) {
			const auto& params = sig->second->parameters;
			if (arg_regs.size() > params.size()) {
				error_at("Too many arguments to '" + expr->function_name + "': expected at most " +
					std::to_string(params.size()) + ", got " + std::to_string(arg_regs.size()), expr);
			}
			for (size_t i = arg_regs.size(); i < params.size(); i++) {
				if (!params[i].default_value) {
					error_at("Missing argument '" + params[i].name + "' in call to '" +
						expr->function_name + "'", expr);
				}
				arg_regs.push_back(gen_expr(params[i].default_value.get(), func));
			}
		}

		int result_reg = alloc_register(func);

		// Return type on result register: enables field checks and native arithmetic.
		if (sig != m_local_signatures.end()) {
			apply_declared_type(result_reg, sig->second->return_type, func);
		}

		IRInstruction call_instr(IROpcode::CALL);
		call_instr.operands.push_back(IRValue::str(expr->function_name));
		call_instr.operands.push_back(IRValue::reg(result_reg));
		call_instr.operands.push_back(IRValue::imm(arg_regs.size()));
		for (int arg_reg : arg_regs) {
			call_instr.operands.push_back(IRValue::reg(arg_reg));
		}
		func.ir.instructions.push_back(call_instr);

		for (int reg : arg_regs) {
			free_register(func, reg);
		}

		return result_reg;
	}

	// After local functions (user print() wins), before self-call fallback.
	if (is_global_function(expr->function_name)) {
		int result = gen_global_function(expr, arg_regs, func);
		for (int reg : arg_regs) {
			free_register(func, reg);
		}
		return result;
	}

	// Refuse unimplemented globals; the self-call fallback would be silently dropped.
	if (const char* reason = unimplemented_global_reason(expr->function_name)) {
		error_at("'" + expr->function_name + "' is not supported: " + reason, expr,
			"Calling it would compile to self." + expr->function_name +
			"(), which Godot accepts and silently ignores");
	}

	// Unknown freestanding call -> self-call: foo(args) becomes self.foo(args).
	int self_reg = alloc_register(func);
	IRInstruction get_self_instr(IROpcode::CALL_SYSCALL);
	get_self_instr.operands.push_back(IRValue::reg(self_reg));
	get_self_instr.operands.push_back(IRValue::imm(ECALL_GET_NODE));
	get_self_instr.operands.push_back(IRValue::imm(0));
	func.ir.instructions.push_back(get_self_instr);

	int result_reg = alloc_register(func);

	IRInstruction vcall_instr(IROpcode::VCALL);
	vcall_instr.operands.push_back(IRValue::reg(result_reg));
	vcall_instr.operands.push_back(IRValue::reg(self_reg));
	vcall_instr.operands.push_back(IRValue::str(expr->function_name));
	vcall_instr.operands.push_back(IRValue::imm(arg_regs.size()));
	for (int arg_reg : arg_regs) {
		vcall_instr.operands.push_back(IRValue::reg(arg_reg));
	}
	func.ir.instructions.push_back(vcall_instr);

	free_register(func, self_reg);
	for (int reg : arg_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

int CodeGenerator::gen_member_call(const MemberCallExpr* expr, FunctionContext& func) {
	// Struct.new(): resolved before object is lowered (struct is a type, not a value).
	if (expr->is_method_call && expr->member_name == "new") {
		if (auto* object = dynamic_cast<const VariableExpr*>(expr->object.get())) {
			if (const StructDecl* decl = find_struct(object->name)) {
				return gen_struct_construct(*decl, expr->arguments, *expr, func, expr);
			}
		}
	}
	// Enum member access: resolve before object generation (enum is a type).
	if (!expr->is_method_call && expr->arguments.empty()) {
		if (auto* object = dynamic_cast<const VariableExpr*>(expr->object.get())) {
			if (find_variable(func, object->name) == nullptr) {
				if (auto found = m_enums.find(object->name); found != m_enums.end()) {
					const EnumDecl* decl = found->second;
					const EnumDecl::Member* member = decl->find_member(expr->member_name);
					if (member == nullptr) {
						error_at("Enum '" + decl->name + "' has no member named '"
							+ expr->member_name + "'", expr);
					}
					return gen_int_immediate(member->value, func);
				}
			}
		}
	}

	reject_named_arguments(*expr, "'" + expr->member_name + "'", expr);

	int obj_reg = gen_expr(expr->object.get(), func);

	std::vector<int> arg_regs;
	for (const auto& arg : expr->arguments) {
		arg_regs.push_back(gen_expr(arg.get(), func));
	}

	if (!expr->is_method_call && arg_regs.empty()) {
		int result = gen_member_read(obj_reg, expr->member_name, func, expr);
		free_register(func, obj_reg);
		return result;
	}

	int result_reg = alloc_register(func);

	// Array.append: dedicated syscall, avoids StringName lookup per element.
	if (expr->is_method_call && arg_regs.size() == 1 &&
		(expr->member_name == "append" || expr->member_name == "push_back") &&
		get_register_type(func, obj_reg) == Variant::ARRAY)
	{
		func.ir.instructions.emplace_back(IROpcode::ARRAY_APPEND, IRValue::reg(result_reg),
			IRValue::reg(obj_reg), IRValue::reg(arg_regs[0]));
		free_register(func, obj_reg);
		free_register(func, arg_regs[0]);
		return result_reg;
	}

	IRInstruction vcall_instr(IROpcode::VCALL);
	vcall_instr.operands.push_back(IRValue::reg(result_reg));
	vcall_instr.operands.push_back(IRValue::reg(obj_reg));
	vcall_instr.operands.push_back(IRValue::str(expr->member_name));
	vcall_instr.operands.push_back(IRValue::imm(arg_regs.size()));
	for (int arg_reg : arg_regs) {
		vcall_instr.operands.push_back(IRValue::reg(arg_reg));
	}
	func.ir.instructions.push_back(vcall_instr);

	free_register(func, obj_reg);
	for (int reg : arg_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

// Both ARRAY and INT types required: backend reads Variants directly, no check.
bool CodeGenerator::is_array_element_access(int obj_reg, int idx_reg, FunctionContext& func) {
	return get_register_type(func, obj_reg) == Variant::ARRAY &&
		get_register_type(func, idx_reg) == Variant::INT;
}

int CodeGenerator::gen_index(const IndexExpr* expr, FunctionContext& func) {
	int obj_reg = gen_expr(expr->object.get(), func);
	int idx_reg = gen_expr(expr->index.get(), func);

	int result_reg = gen_element_read(obj_reg, idx_reg, func, expr);

	free_register(func, obj_reg);
	free_register(func, idx_reg);

	return result_reg;
}

int CodeGenerator::gen_array_literal(const ArrayLiteralExpr* expr, FunctionContext& func) {
	std::vector<int> elem_regs;
	for (const auto& elem : expr->elements) {
		int reg = gen_expr(elem.get(), func);
		elem_regs.push_back(reg);
	}

	int result_reg = alloc_register(func);

	IRInstruction instr(IROpcode::MAKE_ARRAY);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(static_cast<int>(elem_regs.size())));
	for (int reg : elem_regs) {
		instr.operands.push_back(IRValue::reg(reg));
	}

	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, Variant::ARRAY);

	for (int reg : elem_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

int CodeGenerator::gen_dictionary_literal(const DictionaryLiteralExpr* expr, FunctionContext& func) {
	std::vector<int> key_regs;
	std::vector<int> value_regs;
	for (const auto& [key, value] : expr->elements) {
		int key_reg = gen_expr(key.get(), func);
		int value_reg = gen_expr(value.get(), func);
		key_regs.push_back(key_reg);
		value_regs.push_back(value_reg);
	}

	int result_reg = alloc_register(func);

	IRInstruction instr(IROpcode::MAKE_DICTIONARY);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(static_cast<int>(key_regs.size())));
	for (size_t i = 0; i < key_regs.size(); i++) {
		instr.operands.push_back(IRValue::reg(key_regs[i]));
		instr.operands.push_back(IRValue::reg(value_regs[i]));
	}

	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, Variant::DICTIONARY);

	for (int reg : key_regs) {
		free_register(func, reg);
	}
	for (int reg : value_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

const StructDecl* CodeGenerator::find_struct(const std::string& name) const {
	auto it = m_structs.find(name);
	return it == m_structs.end() ? nullptr : it->second;
}

const StructField& CodeGenerator::require_struct_field(const StructDecl& decl,
	const std::string& field_name, int line, int column) const
{
	if (const StructField* field = decl.find_field(field_name)) {
		return *field;
	}
	error_at("Struct '" + decl.name + "' has no field '" + field_name + "'", line, column,
		"Fields of '" + decl.name + "' are: " + decl.field_list());
}

void CodeGenerator::set_register_struct(FunctionContext& func, int reg, const StructDecl* decl) {
	if (decl == nullptr) {
		func.register_structs.erase(reg);
		return;
	}
	func.register_structs[reg] = decl;
	set_register_type(func, reg, Variant::DICTIONARY);
}

const StructDecl* CodeGenerator::get_register_struct(const FunctionContext& func, int reg) const {
	auto it = func.register_structs.find(reg);
	return it == func.register_structs.end() ? nullptr : it->second;
}

FunctionSignature CodeGenerator::build_signature(const FunctionDecl& decl) const {
	FunctionSignature sig;
	sig.name = decl.name;
	sig.line = decl.line;
	sig.description = decl.doc_comment;
	sig.return_type = find_struct(decl.return_type) != nullptr
		? int32_t(Variant::DICTIONARY)
		: int32_t(type_hint_from_string(decl.return_type));

	for (const Parameter& param : decl.parameters) {
		FunctionParameter out;
		out.name = param.name;
		out.type = find_struct(param.type_hint) != nullptr
			? int32_t(Variant::DICTIONARY)
			: int32_t(type_hint_from_string(param.type_hint));

		IRGlobalVar folded;
		if (param.default_value && fold_global_initializer(param.default_value.get(), folded)) {
			using InitType = IRGlobalVar::InitType;
			using DefaultKind = FunctionParameter::DefaultKind;
			switch (folded.init_type) {
				case InitType::INT: out.default_kind = DefaultKind::INT; break;
				case InitType::FLOAT: out.default_kind = DefaultKind::FLOAT; break;
				case InitType::BOOL: out.default_kind = DefaultKind::BOOL; break;
				case InitType::STRING: out.default_kind = DefaultKind::STRING; break;
				case InitType::NULL_VAL: out.default_kind = DefaultKind::NIL; break;
				case InitType::EMPTY_ARRAY: out.default_kind = DefaultKind::EMPTY_ARRAY; break;
				case InitType::EMPTY_DICT: out.default_kind = DefaultKind::EMPTY_DICT; break;
				case InitType::NONE:
				case InitType::RUNTIME: break;
			}
			out.default_value = folded.init_value;
		}
		sig.parameters.push_back(std::move(out));
	}

	sig.required_arguments = sig.parameters.size();
	while (sig.required_arguments > 0 && sig.parameters[sig.required_arguments - 1].optional()) {
		sig.required_arguments--;
	}
	return sig;
}

void CodeGenerator::apply_declared_type(int reg, const std::string& type_hint, FunctionContext& func) {
	if (type_hint.empty()) {
		return;
	}
	if (const StructDecl* decl = find_struct(type_hint)) {
		set_register_struct(func, reg, decl);
		return;
	}
	const IRInstruction::TypeHint type = type_hint_from_string(type_hint);
	if (type != IRInstruction::TypeHint_NONE) {
		set_register_type(func, reg, type);
	}
}

void CodeGenerator::reject_named_arguments(const NamedArguments& names, const std::string& what,
	const Expr* site) const
{
	for (const auto& name : names.argument_names) {
		if (name.empty()) {
			continue;
		}
		error_at("Named arguments are only supported when constructing a struct, and " +
			what + " is not one", site,
			"Pass the value by position, without '" + name + " ='");
	}
}

int CodeGenerator::gen_dict_get(int obj_reg, const std::string& key, FunctionContext& func) {
	int key_reg = alloc_register(func);
	IRInstruction load_key(IROpcode::LOAD_STRING, IRValue::reg(key_reg),
		IRValue::imm(add_string_constant(key)));
	load_key.type_hint = Variant::STRING;
	func.ir.instructions.push_back(load_key);
	set_register_type(func, key_reg, Variant::STRING);

	constexpr int64_t DICT_OP_GET = 0;
	int result_reg = gen_dictionary_op(DICT_OP_GET, obj_reg, key_reg,
		IRInstruction::TypeHint_NONE, func);

	free_register(func, key_reg);
	return result_reg;
}

void CodeGenerator::gen_dict_set(int obj_reg, const std::string& key, int value_reg,
	FunctionContext& func)
{
	int key_reg = alloc_register(func);
	IRInstruction load_key(IROpcode::LOAD_STRING, IRValue::reg(key_reg),
		IRValue::imm(add_string_constant(key)));
	load_key.type_hint = Variant::STRING;
	func.ir.instructions.push_back(load_key);
	set_register_type(func, key_reg, Variant::STRING);

	func.ir.instructions.emplace_back(IROpcode::DICT_SET, IRValue::reg(obj_reg),
		IRValue::reg(key_reg), IRValue::reg(value_reg));

	free_register(func, key_reg);
}

int CodeGenerator::gen_default_value(const std::string& type_hint, FunctionContext& func) {
	switch (type_hint_from_string(type_hint)) {
		case Variant::INT: {
			int reg = alloc_register(func);
			IRInstruction load(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
			load.type_hint = Variant::INT;
			func.ir.instructions.push_back(load);
			set_register_type(func, reg, Variant::INT);
			return reg;
		}
		case Variant::FLOAT: {
			int reg = alloc_register(func);
			IRInstruction load(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(reg), IRValue::fimm(0.0));
			load.type_hint = Variant::FLOAT;
			func.ir.instructions.push_back(load);
			set_register_type(func, reg, Variant::FLOAT);
			return reg;
		}
		case Variant::BOOL: {
			int reg = alloc_register(func);
			IRInstruction load(IROpcode::LOAD_BOOL, IRValue::reg(reg), IRValue::imm(0));
			load.type_hint = Variant::BOOL;
			func.ir.instructions.push_back(load);
			set_register_type(func, reg, Variant::BOOL);
			return reg;
		}
		case Variant::STRING: {
			int reg = alloc_register(func);
			IRInstruction load(IROpcode::LOAD_STRING, IRValue::reg(reg),
				IRValue::imm(add_string_constant("")));
			load.type_hint = Variant::STRING;
			func.ir.instructions.push_back(load);
			set_register_type(func, reg, Variant::STRING);
			return reg;
		}
		default:
			break;
	}

	// Component-based types: zero-constructed.
	static const struct { const char* name; int components; bool integer; } zero_constructors[] = {
		{ "Vector2", 2, false }, { "Vector2i", 2, true },
		{ "Vector3", 3, false }, { "Vector3i", 3, true },
		{ "Vector4", 4, false }, { "Vector4i", 4, true },
		{ "Rect2",   4, false }, { "Rect2i",   4, true },
		{ "Plane",   4, false },
	};
	for (const auto& constructor : zero_constructors) {
		if (type_hint != constructor.name) {
			continue;
		}
		std::vector<int> components;
		for (int i = 0; i < constructor.components; i++) {
			int reg = alloc_register(func);
			if (constructor.integer) {
				IRInstruction load(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
				load.type_hint = Variant::INT;
				func.ir.instructions.push_back(load);
				set_register_type(func, reg, Variant::INT);
			} else {
				IRInstruction load(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(reg), IRValue::fimm(0.0));
				load.type_hint = Variant::FLOAT;
				func.ir.instructions.push_back(load);
				set_register_type(func, reg, Variant::FLOAT);
			}
			components.push_back(reg);
		}
		int result_reg = gen_inline_constructor(type_hint, components, func, nullptr);
		for (int reg : components) {
			free_register(func, reg);
		}
		return result_reg;
	}

	// Zero-argument constructors: Color, Array, Dictionary, packed arrays.
	if (is_inline_primitive_constructor(type_hint)) {
		return gen_inline_constructor(type_hint, {}, func, nullptr);
	}

	// No guest-constructible default.
	return -1;
}

int CodeGenerator::gen_field_default(const StructDecl& decl, const StructField& field,
	FunctionContext& func)
{
	if (field.default_value) {
		int reg = gen_expr(field.default_value.get(), func);
		if (!field.type_hint.empty()) {
			reg = coerce_to_declared_type(reg, type_hint_from_string(field.type_hint), func,
				"field '" + field.name + "' of struct '" + decl.name + "'",
				field.line, field.column);
		}
		apply_declared_type(reg, field.type_hint, func);
		return reg;
	}

	// Nested struct: default to a fresh instance.
	if (const StructDecl* nested = find_struct(field.type_hint)) {
		for (const StructDecl* active : m_struct_default_stack) {
			if (active != nested) {
				continue;
			}
			error_at("Struct '" + nested->name + "' contains itself through field '" +
				decl.name + "." + field.name + "'", field.line, field.column,
				"A struct is a value, so it cannot hold one of its own type by default. "
				"Give the field a default value such as 'null'.");
		}
		m_struct_default_stack.push_back(nested);
		int reg = gen_struct_construct(*nested, {}, NamedArguments{}, func, nullptr);
		m_struct_default_stack.pop_back();
		return reg;
	}

	int reg = gen_default_value(field.type_hint, func);
	if (reg >= 0) {
		return reg;
	}

	// No constructible default; NIL.
	reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
	return reg;
}

int CodeGenerator::gen_struct_construct(const StructDecl& decl, const std::vector<ExprPtr>& arguments,
	const NamedArguments& names, FunctionContext& func, const Expr* site)
{
	// Resolve field indices before evaluation; report bad call sites before lowering.
	std::vector<int> field_of_argument(arguments.size(), -1);
	std::vector<bool> field_supplied(decl.fields.size(), false);

	for (size_t i = 0; i < arguments.size(); i++) {
		const Expr* argument = arguments[i].get();
		const std::string& name = names.argument_name(i);
		int field_index = -1;

		if (name.empty()) {
			if (i >= decl.fields.size()) {
				error_at("Too many values constructing '" + decl.name + "': it has " +
					std::to_string(decl.fields.size()) +
					(decl.fields.size() == 1 ? " field" : " fields"), site,
					"Fields of '" + decl.name + "' are: " + decl.field_list());
			}
			field_index = static_cast<int>(i);
		} else {
			require_struct_field(decl, name, argument->line, argument->column);
			field_index = decl.field_index(name);
		}

		if (field_supplied[field_index]) {
			error_at("Field '" + decl.fields[field_index].name + "' of '" + decl.name +
				"' is given a value twice", argument);
		}
		field_supplied[field_index] = true;
		field_of_argument[i] = field_index;
	}

	// Evaluate in call-site order; named args may differ from field order.
	std::vector<int> value_regs(decl.fields.size(), -1);
	for (size_t i = 0; i < arguments.size(); i++) {
		const StructField& field = decl.fields[field_of_argument[i]];
		int reg = gen_expr(arguments[i].get(), func);
		if (!field.type_hint.empty()) {
			reg = coerce_to_declared_type(reg, type_hint_from_string(field.type_hint), func,
				"field '" + field.name + "' of struct '" + decl.name + "'",
				arguments[i]->line, arguments[i]->column);
		}
		apply_declared_type(reg, field.type_hint, func);
		value_regs[field_of_argument[i]] = reg;
	}

	// Unsupplied fields get declaration defaults; m_struct_default_stack guards recursion.
	m_struct_default_stack.push_back(&decl);
	for (size_t i = 0; i < decl.fields.size(); i++) {
		if (value_regs[i] < 0) {
			value_regs[i] = gen_field_default(decl, decl.fields[i], func);
		}
	}
	m_struct_default_stack.pop_back();

	int result_reg = alloc_register(func);
	IRInstruction make(IROpcode::MAKE_DICTIONARY);
	make.operands.push_back(IRValue::reg(result_reg));
	make.operands.push_back(IRValue::imm(static_cast<int>(decl.fields.size())));

	std::vector<int> key_regs;
	for (size_t i = 0; i < decl.fields.size(); i++) {
		int key_reg = alloc_register(func);
		IRInstruction load_key(IROpcode::LOAD_STRING, IRValue::reg(key_reg),
			IRValue::imm(add_string_constant(decl.fields[i].name)));
		load_key.type_hint = Variant::STRING;
		func.ir.instructions.push_back(load_key);
		set_register_type(func, key_reg, Variant::STRING);
		key_regs.push_back(key_reg);

		make.operands.push_back(IRValue::reg(key_reg));
		make.operands.push_back(IRValue::reg(value_regs[i]));
	}

	make.type_hint = Variant::DICTIONARY;
	func.ir.instructions.push_back(make);
	set_register_struct(func, result_reg, &decl);

	for (int reg : key_regs) {
		free_register(func, reg);
	}
	for (int reg : value_regs) {
		free_register(func, reg);
	}
	return result_reg;
}

int CodeGenerator::alloc_register(FunctionContext& func) {
	return func.next_register++;
}

void CodeGenerator::free_register(FunctionContext& func, int reg) {
	(void) func;
	(void) reg;
}

std::string CodeGenerator::make_label(const std::string& prefix) {
	return prefix + "_" + std::to_string(m_next_label++);
}

int CodeGenerator::add_string_constant(const std::string& str) {
	for (size_t i = 0; i < m_string_constants.size(); i++) {
		if (m_string_constants[i] == str) {
			return static_cast<int>(i);
		}
	}

	m_string_constants.push_back(str);
	return static_cast<int>(m_string_constants.size() - 1);
}

void CodeGenerator::push_scope(FunctionContext& func) {
	Scope new_scope;
	if (func.scopes.empty()) {
		new_scope.parent_scope_idx = SIZE_MAX;
	} else {
		new_scope.parent_scope_idx = func.scopes.size() - 1;
	}
	func.scopes.push_back(new_scope);
}

void CodeGenerator::pop_scope(FunctionContext& func) {
	if (func.scopes.empty()) {
		throw CompilerException(ErrorType::CODEGEN_ERROR, "Cannot pop scope: scope stack is empty");
	}
	func.scopes.pop_back();
}

CodeGenerator::Variable* CodeGenerator::find_variable(FunctionContext& func, const std::string& name) {
	for (int i = static_cast<int>(func.scopes.size()) - 1; i >= 0; i--) {
		auto it = func.scopes[i].variables.find(name);
		if (it != func.scopes[i].variables.end()) {
			return &it->second;
		}
	}
	return nullptr;
}

void CodeGenerator::declare_variable(FunctionContext& func, const std::string& name, int register_num, bool is_const,
	const Stmt* site)
{
	if (func.scopes.empty()) {
		throw CompilerException(ErrorType::CODEGEN_ERROR, "Cannot declare variable: no scope active");
	}

	auto& current_scope = func.scopes.back();
	if (current_scope.variables.find(name) != current_scope.variables.end()) {
		error_at("Variable '" + name + "' is already declared in this scope", site);
	}

	current_scope.variables[name] = {name, register_num, IRInstruction::TypeHint_NONE, is_const};
}

void CodeGenerator::set_register_type(FunctionContext& func, int reg, IRInstruction::TypeHint type) {
	func.register_types[reg] = type;
}

IRInstruction::TypeHint CodeGenerator::get_register_type(const FunctionContext& func, int reg) const {
	auto it = func.register_types.find(reg);
	if (it != func.register_types.end()) {
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

bool CodeGenerator::is_global_function(const std::string& name) const {
	return find_global_function(name) != nullptr;
}

int CodeGenerator::gen_global_function(const CallExpr* expr, std::vector<int>& arg_regs, FunctionContext& func) {
	const GlobalFunction* info = find_global_function(expr->function_name);
	if (info == nullptr) {
		throw CompilerException(ErrorType::CODEGEN_ERROR,
			"is_global_function() accepts '" + expr->function_name + "' but there is no table entry for it");
	}

	const size_t given = arg_regs.size();
	if (given < info->min_args || given > info->max_args) {
		std::string expected;
		if (info->min_args == info->max_args) {
			expected = std::to_string(info->min_args);
		} else if (info->max_args == 63) {
			expected = "at least " + std::to_string(info->min_args);
		} else {
			expected = std::to_string(info->min_args) + " to " + std::to_string(info->max_args);
		}
		error_at(expr->function_name + "() takes " + expected + " argument" +
			(info->min_args == 1 && info->max_args == 1 ? "" : "s") + ", got " +
			std::to_string(given), expr);
	}

	if (info->kind == GlobalKind::PRINT) {
		int result_reg = alloc_register(func);

		IRInstruction instr(IROpcode::PRINT);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(arg_regs.size()));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		func.ir.instructions.push_back(instr);

		set_register_type(func, result_reg, Variant::NIL);
		return result_reg;
	}

	// min/max variadic: fold to chain of two-argument calls.
	if ((info->fn == GlobalFn::MIN || info->fn == GlobalFn::MAX) && given > 2) {
		int accumulated = arg_regs[0];
		for (size_t i = 1; i < arg_regs.size(); i++) {
			const std::vector<int> pair { accumulated, arg_regs[i] };
			const int folded = gen_global_call(*info, pair, func, expr);
			if (i > 1) {
				free_register(func, accumulated);
			}
			accumulated = folded;
		}
		return accumulated;
	}

	return gen_global_call(*info, arg_regs, func, expr);
}

int CodeGenerator::gen_global_call(const GlobalFunction& info, const std::vector<int>& arg_regs,
	FunctionContext& func, const Expr* site)
{
	(void)site;

	// NUMERIC: resolved to int or float form by operand types; unresolved stays runtime.
	const GlobalFunction* chosen = &info;
	if (info.kind == GlobalKind::NUMERIC) {
		bool all_integer = true;
		bool all_known = true;
		for (int reg : arg_regs) {
			const IRInstruction::TypeHint hint = get_register_type(func, reg);
			if (hint == Variant::INT) {
				continue;
			}
			all_integer = false;
			if (hint != Variant::FLOAT) {
				all_known = false;
			}
		}
		if (all_integer || all_known) {
			chosen = &global_function(resolve_numeric_form(info, all_integer));
		}
	}

	// CAST: inline when operand is a known numeric/bool, host otherwise (String parse).
	if (info.kind == GlobalKind::CAST) {
		const IRInstruction::TypeHint hint = arg_regs.empty()
			? IRInstruction::TypeHint_NONE
			: get_register_type(func, arg_regs[0]);
		chosen = &global_function(resolve_cast_form(info, hint));
	}

	// Implicit INT->FLOAT widening emitted as CONVERT here, foldable by optimizer.
	std::vector<int> call_args = arg_regs;
	std::vector<int> converted;
	IRInstruction::TypeHint wanted = IRInstruction::TypeHint_NONE;
	switch (chosen->kind) {
		case GlobalKind::INT_OP:
		case GlobalKind::SYSCALL_INT:
			wanted = Variant::INT;
			break;
		case GlobalKind::FLOAT_OP:
		case GlobalKind::SYSCALL:
			wanted = Variant::FLOAT;
			break;
		case GlobalKind::PRINT:
		case GlobalKind::NUMERIC:
		case GlobalKind::CAST:
		case GlobalKind::HOST:
			break;
	}

	bool typed = wanted != IRInstruction::TypeHint_NONE;
	if (typed) {
		for (int& reg : call_args) {
			const IRInstruction::TypeHint hint = get_register_type(func, reg);
			if (hint == wanted) {
				continue;
			}
			if (wanted == Variant::FLOAT && hint == Variant::INT) {
				const int widened = alloc_register(func);
				IRInstruction convert(IROpcode::CONVERT, IRValue::reg(widened), IRValue::reg(reg),
					IRValue::imm(Variant::INT));
				convert.type_hint = Variant::FLOAT;
				func.ir.instructions.push_back(convert);
				set_register_type(func, widened, Variant::FLOAT);
				converted.push_back(widened);
				reg = widened;
				continue;
			}
			typed = false;
			break;
		}
	}

	int result_reg = alloc_register(func);

	IRInstruction instr(IROpcode::GLOBAL_CALL);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(chosen->fn)));
	instr.operands.push_back(IRValue::imm(typed ? 1 : 0));
	instr.operands.push_back(IRValue::imm(call_args.size()));
	for (int arg_reg : call_args) {
		instr.operands.push_back(IRValue::reg(arg_reg));
	}

	IRInstruction::TypeHint result_type = IRInstruction::TypeHint_NONE;
	switch (chosen->result) {
		case GlobalResult::NIL: result_type = Variant::NIL; break;
		case GlobalResult::BOOL: result_type = Variant::BOOL; break;
		case GlobalResult::INT: result_type = Variant::INT; break;
		case GlobalResult::FLOAT: result_type = Variant::FLOAT; break;
		case GlobalResult::STRING: result_type = Variant::STRING; break;
		case GlobalResult::NUMERIC: break;
	}
	instr.type_hint = result_type;
	func.ir.instructions.push_back(instr);

	for (int reg : converted) {
		free_register(func, reg);
	}
	if (result_type != IRInstruction::TypeHint_NONE) {
		set_register_type(func, result_reg, result_type);
	}
	return result_reg;
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
			return false;

		case Variant::PLANE:
			return false;

		default:
			return false;
	}
}

int CodeGenerator::gen_inline_constructor(const std::string& name, const std::vector<int>& arg_regs,
	FunctionContext& func, const Expr* site)
{
	int result_reg = alloc_register(func);
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
		if (arg_regs.size() == 0) {
			int r_reg = alloc_register(func);
			int g_reg = alloc_register(func);
			int b_reg = alloc_register(func);
			int a_reg = alloc_register(func);

			func.ir.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(r_reg), IRValue::fimm(1.0));
			func.ir.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(g_reg), IRValue::fimm(1.0));
			func.ir.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(b_reg), IRValue::fimm(1.0));
			func.ir.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(a_reg), IRValue::fimm(1.0));

			instr = IRInstruction(IROpcode::MAKE_COLOR);
			instr.operands.push_back(IRValue::reg(result_reg));
			instr.operands.push_back(IRValue::reg(r_reg));
			instr.operands.push_back(IRValue::reg(g_reg));
			instr.operands.push_back(IRValue::reg(b_reg));
			instr.operands.push_back(IRValue::reg(a_reg));
			result_type = Variant::COLOR;
		} else if (arg_regs.size() == 3) {
			int a_reg = alloc_register(func);
			func.ir.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(a_reg), IRValue::fimm(1.0));

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
			error_at("Color constructor requires 0, 3, or 4 arguments, got " +
				std::to_string(arg_regs.size()), site);
		}
	} else if (name == "Array") {
		instr = IRInstruction(IROpcode::MAKE_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size())));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::ARRAY;
	} else if (name == "PackedByteArray") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_BYTE_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size())));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_BYTE_ARRAY;
	} else if (name == "PackedInt32Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_INT32_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size())));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_INT32_ARRAY;
	} else if (name == "PackedInt64Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_INT64_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size())));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_INT64_ARRAY;
	} else if (name == "PackedFloat32Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_FLOAT32_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size())));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_FLOAT32_ARRAY;
	} else if (name == "PackedFloat64Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_FLOAT64_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size())));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_FLOAT64_ARRAY;
	} else if (name == "PackedStringArray") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_STRING_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size())));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_STRING_ARRAY;
	} else if (name == "PackedVector2Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_VECTOR2_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size())));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_VECTOR2_ARRAY;
	} else if (name == "PackedVector3Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_VECTOR3_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size())));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_VECTOR3_ARRAY;
	} else if (name == "PackedColorArray") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_COLOR_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size())));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_COLOR_ARRAY;
	} else if (name == "PackedVector4Array") {
		instr = IRInstruction(IROpcode::MAKE_PACKED_VECTOR4_ARRAY);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(static_cast<int>(arg_regs.size())));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		result_type = Variant::PACKED_VECTOR4_ARRAY;
	} else if (name == "Dictionary") {
		instr = IRInstruction(IROpcode::MAKE_DICTIONARY);
		instr.operands.push_back(IRValue::reg(result_reg));
		result_type = Variant::DICTIONARY;
	} else {
		instr.operands.push_back(IRValue::str(name));
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(arg_regs.size()));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
	}

	if (result_type != IRInstruction::TypeHint_NONE) {
		instr.type_hint = result_type;
		set_register_type(func, result_reg, result_type);
	}

	func.ir.instructions.push_back(instr);

	return result_reg;
}

int CodeGenerator::gen_inline_member_get(int obj_reg, IRInstruction::TypeHint obj_type, const std::string& member, FunctionContext& func) {
	int result_reg = alloc_register(func);

	IRInstruction instr(IROpcode::VGET_INLINE);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(IRValue::str(member));
	instr.operands.push_back(IRValue::imm(static_cast<int>(obj_type)));

	func.ir.instructions.push_back(instr);

	bool is_int_vector = (obj_type == Variant::VECTOR2I ||
	                      obj_type == Variant::VECTOR3I ||
	                      obj_type == Variant::VECTOR4I);

	set_register_type(func, result_reg, is_int_vector ? Variant::INT : Variant::FLOAT);

	return result_reg;
}

void CodeGenerator::gen_inline_member_set(int obj_reg, IRInstruction::TypeHint obj_type,
	const std::string& member, int value_reg, FunctionContext& func)
{
	IRInstruction instr(IROpcode::VSET_INLINE);
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(IRValue::str(member));
	instr.operands.push_back(IRValue::imm(static_cast<int>(obj_type)));
	instr.operands.push_back(IRValue::reg(value_reg));
	func.ir.instructions.push_back(instr);
}

// Packed arrays have size()/get() via VCALL; String has neither.
bool CodeGenerator::is_packed_array_type(IRInstruction::TypeHint type) {
	switch (type) {
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_STRING_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY:
		case Variant::PACKED_VECTOR4_ARRAY:
		case Variant::PACKED_COLOR_ARRAY:
			return true;
		default:
			return false;
	}
}

std::vector<IRInstruction::TypeHint> CodeGenerator::inline_member_types(const std::string& member) const {
	// Kept in sync with is_inline_member_access.
	static constexpr IRInstruction::TypeHint candidates[] = {
		Variant::VECTOR2, Variant::VECTOR2I,
		Variant::VECTOR3, Variant::VECTOR3I,
		Variant::VECTOR4, Variant::VECTOR4I,
		Variant::COLOR,
	};

	std::vector<IRInstruction::TypeHint> types;
	for (IRInstruction::TypeHint type : candidates) {
		if (is_inline_member_access(type, member)) {
			types.push_back(type);
		}
	}
	return types;
}

int CodeGenerator::gen_vget(int obj_reg, const std::string& member, FunctionContext& func) {
	int result_reg = alloc_register(func);
	int str_idx = add_string_constant(member);

	IRInstruction instr(IROpcode::VGET);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(IRValue::imm(str_idx));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(member.length())));
	func.ir.instructions.push_back(instr);

	return result_reg;
}

void CodeGenerator::gen_vset(int obj_reg, const std::string& member, int value_reg, FunctionContext& func) {
	int str_idx = add_string_constant(member);

	IRInstruction instr(IROpcode::VSET);
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(IRValue::imm(str_idx));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(member.length())));
	instr.operands.push_back(IRValue::reg(value_reg));
	func.ir.instructions.push_back(instr);
}

// Untyped `.x`: branch on tag to inline payload or VGET (Object-only).
int CodeGenerator::gen_dynamic_member_get(int obj_reg, const std::string& member, FunctionContext& func) {
	const std::vector<IRInstruction::TypeHint> types = inline_member_types(member);
	const std::string end_label = make_label("member_get_end");
	int result_reg = alloc_register(func);

	for (IRInstruction::TypeHint type : types) {
		const std::string next_label = make_label("member_get_next");
		int test_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(test_reg),
			IRValue::reg(obj_reg), IRValue::imm(static_cast<int64_t>(type)));
		set_register_type(func, test_reg, Variant::BOOL);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, test_reg, next_label, func);
		free_register(func, test_reg);

		int inline_reg = gen_inline_member_get(obj_reg, type, member, func);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result_reg),
			IRValue::reg(inline_reg));
		free_register(func, inline_reg);
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(end_label));
		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(next_label));
	}

	int property_reg = gen_vget(obj_reg, member, func);
	func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(result_reg),
		IRValue::reg(property_reg));
	free_register(func, property_reg);
	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));

	// Result type unknown: paths yield int, float or property.
	return result_reg;
}

void CodeGenerator::gen_dynamic_member_set(int obj_reg, const std::string& member, int value_reg,
	FunctionContext& func)
{
	const std::vector<IRInstruction::TypeHint> types = inline_member_types(member);
	const std::string end_label = make_label("member_set_end");

	for (IRInstruction::TypeHint type : types) {
		const std::string next_label = make_label("member_set_next");
		int test_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::TYPE_TEST, IRValue::reg(test_reg),
			IRValue::reg(obj_reg), IRValue::imm(static_cast<int64_t>(type)));
		set_register_type(func, test_reg, Variant::BOOL);
		emit_conditional_branch(IROpcode::BRANCH_ZERO, test_reg, next_label, func);
		free_register(func, test_reg);

		gen_inline_member_set(obj_reg, type, member, value_reg, func);
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(end_label));
		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(next_label));
	}

	gen_vset(obj_reg, member, value_reg, func);
	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));
}

int CodeGenerator::gen_member_read(int obj_reg, const std::string& member, FunctionContext& func,
	const Expr* site)
{
	const IRInstruction::TypeHint obj_type = get_register_type(func, obj_reg);

	if (is_inline_member_access(obj_type, member)) {
		return gen_inline_member_get(obj_reg, obj_type, member, func);
	}

	// Struct field: validate and apply declared type.
	if (const StructDecl* decl = get_register_struct(func, obj_reg)) {
		const StructField& field = require_struct_field(*decl, member,
			site ? site->line : 0, site ? site->column : 0);
		int result_reg = gen_dict_get(obj_reg, member, func);
		apply_declared_type(result_reg, field.type_hint, func);
		return result_reg;
	}

	// Dictionary: element read, not VGET (Object-only).
	if (obj_type == Variant::DICTIONARY) {
		return gen_dict_get(obj_reg, member, func);
	}

	if (obj_type == IRInstruction::TypeHint_NONE && !inline_member_types(member).empty()) {
		return gen_dynamic_member_get(obj_reg, member, func);
	}

	return gen_vget(obj_reg, member, func);
}

bool CodeGenerator::gen_member_store(int obj_reg, const std::string& member, int value_reg,
	FunctionContext& func)
{
	// Struct/Dictionary: element write, no write-back needed.
	if (get_register_struct(func, obj_reg) != nullptr ||
		get_register_type(func, obj_reg) == Variant::DICTIONARY)
	{
		gen_dict_set(obj_reg, member, value_reg, func);
		return false;
	}

	const IRInstruction::TypeHint obj_type = get_register_type(func, obj_reg);

	if (is_inline_member_access(obj_type, member)) {
		gen_inline_member_set(obj_reg, obj_type, member, value_reg, func);
		return true;
	}

	if (obj_type == IRInstruction::TypeHint_NONE && !inline_member_types(member).empty()) {
		gen_dynamic_member_set(obj_reg, member, value_reg, func);
		return true;
	}

	gen_vset(obj_reg, member, value_reg, func);
	return false;
}

int CodeGenerator::gen_element_read(int obj_reg, int idx_reg, FunctionContext& func,
	const Expr* site)
{
	// Fallback is VCALL("get"). String lacks get(), so refuse it.
	if (get_register_type(func, obj_reg) == Variant::STRING) {
		error_at("Cannot subscript a String yet", site ? site->line : 0, site ? site->column : 0,
			"String has no get(); indexing one needs a host syscall that yields a"
			" one-character String");
	}

	if (is_array_element_access(obj_reg, idx_reg, func)) {
		int element_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::ARRAY_GET, IRValue::reg(element_reg),
			IRValue::reg(obj_reg), IRValue::reg(idx_reg));
		return element_reg;
	}

	if (get_register_type(func, obj_reg) == Variant::DICTIONARY) {
		constexpr int64_t DICT_OP_GET = 0;
		return gen_dictionary_op(DICT_OP_GET, obj_reg, idx_reg,
			IRInstruction::TypeHint_NONE, func);
	}

	// Fallback: obj[x] -> VCALL("get", x).
	int result_reg = alloc_register(func);
	IRInstruction instr(IROpcode::VCALL);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(IRValue::str("get"));
	instr.operands.push_back(IRValue::imm(1));
	instr.operands.push_back(IRValue::reg(idx_reg));
	func.ir.instructions.push_back(instr);

	return result_reg;
}

void CodeGenerator::gen_element_store(int obj_reg, int idx_reg, int value_reg, FunctionContext& func) {
	if (is_array_element_access(obj_reg, idx_reg, func)) {
		func.ir.instructions.emplace_back(IROpcode::ARRAY_SET, IRValue::reg(obj_reg),
			IRValue::reg(idx_reg), IRValue::reg(value_reg));
		return;
	}

	if (get_register_type(func, obj_reg) == Variant::DICTIONARY) {
		func.ir.instructions.emplace_back(IROpcode::DICT_SET, IRValue::reg(obj_reg),
			IRValue::reg(idx_reg), IRValue::reg(value_reg));
		return;
	}

	int result_reg = alloc_register(func);
	IRInstruction instr(IROpcode::VCALL);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::reg(obj_reg));
	instr.operands.push_back(IRValue::str("set"));
	instr.operands.push_back(IRValue::imm(2));
	instr.operands.push_back(IRValue::reg(idx_reg));
	instr.operands.push_back(IRValue::reg(value_reg));
	func.ir.instructions.push_back(instr);
	free_register(func, result_reg);
}

std::unordered_set<std::string> CodeGenerator::get_global_classes() {
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

int CodeGenerator::coerce_to_declared_type(int reg, IRInstruction::TypeHint declared,
	FunctionContext& func, const std::string& what, const Stmt* site)
{
	return coerce_to_declared_type(reg, declared, func, what,
		site != nullptr ? site->line : 0, site != nullptr ? site->column : 0);
}

int CodeGenerator::coerce_to_declared_type(int reg, IRInstruction::TypeHint declared,
	FunctionContext& func, const std::string& what, int line, int column)
{
	if (declared == IRInstruction::TypeHint_NONE) {
		return reg;
	}
	const IRInstruction::TypeHint actual = get_register_type(func, reg);
	if (actual == IRInstruction::TypeHint_NONE || actual == declared) {
		return reg;
	}

	// INT->FLOAT, BOOL->INT/FLOAT: payload size mismatch without explicit convert.
	const bool widening = (declared == Variant::FLOAT && actual == Variant::INT) ||
		(actual == Variant::BOOL && (declared == Variant::INT || declared == Variant::FLOAT));
	if (widening) {
		int converted = alloc_register(func);
		IRInstruction convert(IROpcode::CONVERT, IRValue::reg(converted), IRValue::reg(reg),
			IRValue::imm(actual));
		convert.type_hint = declared;
		func.ir.instructions.push_back(convert);
		set_register_type(func, converted, declared);
		free_register(func, reg);
		return converted;
	}

	error_at("Cannot assign a value of type " + std::string(variant_type_name(actual)) +
		" to " + what + " of type " + std::string(variant_type_name(declared)), line, column);
}

void CodeGenerator::coerce_folded_initializer(IRGlobalVar& global, int line, int column) const {
	if (global.type_hint == IRInstruction::TypeHint_NONE ||
	    global.init_type == IRGlobalVar::InitType::NULL_VAL) {
		return;
	}

	// `var f: float = 0` folds INT to FLOAT.
	if (global.type_hint == Variant::FLOAT && global.init_type == IRGlobalVar::InitType::INT) {
		global.init_type = IRGlobalVar::InitType::FLOAT;
		global.init_value = static_cast<double>(std::get<int64_t>(global.init_value));
		return;
	}

	IRGlobalVar probe = global;
	probe.type_hint = IRInstruction::TypeHint_NONE;
	const IRInstruction::TypeHint actual = derive_global_value_type(probe);
	if (actual != IRInstruction::TypeHint_NONE && actual != global.type_hint) {
		error_at("Global variable '" + global.name + "' is declared as " +
			std::string(variant_type_name(global.type_hint)) +
			" but initialized with a value of type " + std::string(variant_type_name(actual)),
			line, column);
	}
}

void CodeGenerator::apply_default_initializer(IRGlobalVar& global, FunctionContext& init_func,
	size_t global_index, bool& has_global_init)
{
	global.value_type = global.type_hint;
	if (global.type_hint == IRInstruction::TypeHint_NONE) {
		return;
	}

	switch (global.type_hint) {
		case Variant::INT:
			global.init_type = IRGlobalVar::InitType::INT;
			global.init_value = int64_t(0);
			return;
		case Variant::FLOAT:
			global.init_type = IRGlobalVar::InitType::FLOAT;
			global.init_value = 0.0;
			return;
		case Variant::BOOL:
			global.init_type = IRGlobalVar::InitType::BOOL;
			global.init_value = false;
			return;
		case Variant::STRING:
			global.init_type = IRGlobalVar::InitType::STRING;
			global.init_value = std::string();
			return;
		case Variant::ARRAY:
			global.init_type = IRGlobalVar::InitType::EMPTY_ARRAY;
			return;
		case Variant::DICTIONARY:
			global.init_type = IRGlobalVar::InitType::EMPTY_DICT;
			return;
		default:
			break;
	}

	// Packed arrays: no compile-time representation, built by init function.
	const IROpcode make_op = packed_array_opcode(global.type_hint);
	if (make_op != IROpcode::LABEL) {
		int reg = alloc_register(init_func);
		IRInstruction make(make_op);
		make.operands.push_back(IRValue::reg(reg));
		make.operands.push_back(IRValue::imm(0));
		make.type_hint = global.type_hint;
		init_func.ir.instructions.push_back(make);
		init_func.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL,
			IRValue::imm(static_cast<int64_t>(global_index)), IRValue::reg(reg));
		free_register(init_func, reg);
		global.init_type = IRGlobalVar::InitType::RUNTIME;
		has_global_init = true;
		return;
	}

	// Object, Callable, Transform3D, etc.: no guest-constructible default; stays NIL.
}

IROpcode CodeGenerator::packed_array_opcode(IRInstruction::TypeHint type) {
	switch (type) {
		case Variant::PACKED_BYTE_ARRAY: return IROpcode::MAKE_PACKED_BYTE_ARRAY;
		case Variant::PACKED_INT32_ARRAY: return IROpcode::MAKE_PACKED_INT32_ARRAY;
		case Variant::PACKED_INT64_ARRAY: return IROpcode::MAKE_PACKED_INT64_ARRAY;
		case Variant::PACKED_FLOAT32_ARRAY: return IROpcode::MAKE_PACKED_FLOAT32_ARRAY;
		case Variant::PACKED_FLOAT64_ARRAY: return IROpcode::MAKE_PACKED_FLOAT64_ARRAY;
		case Variant::PACKED_STRING_ARRAY: return IROpcode::MAKE_PACKED_STRING_ARRAY;
		case Variant::PACKED_VECTOR2_ARRAY: return IROpcode::MAKE_PACKED_VECTOR2_ARRAY;
		case Variant::PACKED_VECTOR3_ARRAY: return IROpcode::MAKE_PACKED_VECTOR3_ARRAY;
		case Variant::PACKED_VECTOR4_ARRAY: return IROpcode::MAKE_PACKED_VECTOR4_ARRAY;
		case Variant::PACKED_COLOR_ARRAY: return IROpcode::MAKE_PACKED_COLOR_ARRAY;
		default:
			// LABEL is never a construction opcode, so it doubles as "no default".
			return IROpcode::LABEL;
	}
}

bool CodeGenerator::fold_global_initializer(const Expr* expr, IRGlobalVar& out) const {
	using InitType = IRGlobalVar::InitType;

	if (auto* lit = dynamic_cast<const LiteralExpr*>(expr)) {
		switch (lit->lit_type) {
			case LiteralExpr::Type::INTEGER:
				out.init_type = InitType::INT;
				out.init_value = std::get<int64_t>(lit->value);
				return true;
			case LiteralExpr::Type::FLOAT:
				out.init_type = InitType::FLOAT;
				out.init_value = std::get<double>(lit->value);
				return true;
			case LiteralExpr::Type::STRING:
				out.init_type = InitType::STRING;
				out.init_value = std::get<std::string>(lit->value);
				return true;
			case LiteralExpr::Type::BOOL:
				out.init_type = InitType::BOOL;
				out.init_value = std::get<bool>(lit->value);
				return true;
			case LiteralExpr::Type::NULL_VAL:
				out.init_type = InitType::NULL_VAL;
				return true;
		}
		return false;
	}

	// '-5' parses as NEG(5); fold here so it remains a constant.
	if (auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
		IRGlobalVar inner;
		if (!fold_global_initializer(unary->operand.get(), inner)) {
			return false;
		}
		switch (unary->op) {
			case UnaryExpr::Op::NEG:
				if (inner.init_type == InitType::INT) {
					// Unsigned: signed negate of INT64_MIN overflows.
					const uint64_t value = static_cast<uint64_t>(std::get<int64_t>(inner.init_value));
					out.init_type = InitType::INT;
					out.init_value = static_cast<int64_t>(0u - value);
					return true;
				}
				if (inner.init_type == InitType::FLOAT) {
					out.init_type = InitType::FLOAT;
					out.init_value = -std::get<double>(inner.init_value);
					return true;
				}
				return false;
			case UnaryExpr::Op::BIT_NOT:
				if (inner.init_type == InitType::INT) {
					out.init_type = InitType::INT;
					out.init_value = ~std::get<int64_t>(inner.init_value);
					return true;
				}
				return false;
			case UnaryExpr::Op::NOT:
				out.init_type = InitType::BOOL;
				switch (inner.init_type) {
					case InitType::BOOL: out.init_value = !std::get<bool>(inner.init_value); return true;
					case InitType::INT: out.init_value = std::get<int64_t>(inner.init_value) == 0; return true;
					case InitType::FLOAT: out.init_value = std::get<double>(inner.init_value) == 0.0; return true;
					case InitType::NULL_VAL: out.init_value = true; return true;
					case InitType::STRING: out.init_value = std::get<std::string>(inner.init_value).empty(); return true;
					default: return false;
				}
		}
		return false;
	}

	// Folded global const.
	if (auto* var = dynamic_cast<const VariableExpr*>(expr)) {
		auto it = m_global_const_values.find(var->name);
		if (it == m_global_const_values.end()) {
			return false;
		}
		out.init_type = it->second.init_type;
		out.init_value = it->second.init_value;
		return true;
	}

	// Empty containers fold to InitType; backend writes directly.
	if (auto* array_lit = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
		if (array_lit->elements.empty()) {
			out.init_type = InitType::EMPTY_ARRAY;
			return true;
		}
		return false;
	}
	if (auto* dict_lit = dynamic_cast<const DictionaryLiteralExpr*>(expr)) {
		if (dict_lit->elements.empty()) {
			out.init_type = InitType::EMPTY_DICT;
			return true;
		}
		return false;
	}

	return false;
}

IRInstruction::TypeHint CodeGenerator::fused_compare_type(IRInstruction::TypeHint left,
                                                          IRInstruction::TypeHint right) {
	// Native compare only when both operands share a known type.
	if (left == IRInstruction::TypeHint_NONE || left != right) {
		return IRInstruction::TypeHint_NONE;
	}
	if (left == Variant::INT || left == Variant::FLOAT || TypeHintUtils::is_vector(left)) {
		return left;
	}
	return IRInstruction::TypeHint_NONE;
}

int CodeGenerator::gen_const_global_value(const std::string& name, FunctionContext& func) {
	auto it = m_global_const_values.find(name);
	if (it == m_global_const_values.end()) {
		return -1;
	}
	const IRGlobalVar& global = it->second;

	// Container consts are shared handles; stays on LOAD_GLOBAL.
	using InitType = IRGlobalVar::InitType;
	switch (global.init_type) {
		case InitType::INT: {
			int reg = alloc_register(func);
			IRInstruction instr(IROpcode::LOAD_IMM, IRValue::reg(reg),
			                    IRValue::imm(std::get<int64_t>(global.init_value)));
			instr.type_hint = Variant::INT;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::INT);
			return reg;
		}
		case InitType::FLOAT: {
			int reg = alloc_register(func);
			IRInstruction instr(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(reg),
			                    IRValue::fimm(std::get<double>(global.init_value)));
			instr.type_hint = Variant::FLOAT;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::FLOAT);
			return reg;
		}
		case InitType::BOOL: {
			int reg = alloc_register(func);
			IRInstruction instr(IROpcode::LOAD_BOOL, IRValue::reg(reg),
			                    IRValue::imm(std::get<bool>(global.init_value) ? 1 : 0));
			instr.type_hint = Variant::BOOL;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::BOOL);
			return reg;
		}
		case InitType::STRING: {
			int reg = alloc_register(func);
			int str_idx = add_string_constant(std::get<std::string>(global.init_value));
			IRInstruction instr(IROpcode::LOAD_STRING, IRValue::reg(reg), IRValue::imm(str_idx));
			instr.type_hint = Variant::STRING;
			func.ir.instructions.push_back(instr);
			set_register_type(func, reg, Variant::STRING);
			return reg;
		}
		case InitType::NONE:
		case InitType::NULL_VAL:
		case InitType::EMPTY_ARRAY:
		case InitType::EMPTY_DICT:
		case InitType::RUNTIME:
			return -1;
	}
	return -1;
}

IRInstruction::TypeHint CodeGenerator::derive_global_value_type(const IRGlobalVar& global) {
	// Explicit type hint wins.
	if (global.type_hint != IRInstruction::TypeHint_NONE) {
		return global.type_hint;
	}
	switch (global.init_type) {
		case IRGlobalVar::InitType::INT: return Variant::INT;
		case IRGlobalVar::InitType::FLOAT: return Variant::FLOAT;
		case IRGlobalVar::InitType::BOOL: return Variant::BOOL;
		case IRGlobalVar::InitType::STRING: return Variant::STRING;
		case IRGlobalVar::InitType::EMPTY_ARRAY: return Variant::ARRAY;
		case IRGlobalVar::InitType::EMPTY_DICT: return Variant::DICTIONARY;
		case IRGlobalVar::InitType::NULL_VAL:
		case IRGlobalVar::InitType::NONE:
		case IRGlobalVar::InitType::RUNTIME:
			// RUNTIME: caller fills type; untyped stays any-Variant.
			return IRInstruction::TypeHint_NONE;
	}
	return IRInstruction::TypeHint_NONE;
}

bool CodeGenerator::is_global_variable(const std::string& name) const {
	return m_global_variables.find(name) != m_global_variables.end();
}

bool CodeGenerator::is_global_const(const std::string& name) const {
	return m_global_consts.find(name) != m_global_consts.end();
}

int CodeGenerator::gen_global_class_get(const std::string& class_name, FunctionContext& func) {
	int result_reg = alloc_register(func);
	int str_idx = add_string_constant(class_name);

	IRInstruction instr(IROpcode::CALL_SYSCALL);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(ECALL_GET_OBJ));
	instr.operands.push_back(IRValue::imm(str_idx));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(class_name.length())));

	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, IRInstruction::TypeHint_NONE);

	return result_reg;
}

} // namespace gdscript
