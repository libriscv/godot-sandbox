#include "codegen.h"
#include "compiler_exception.h"
#include <stdexcept>
#include <sstream>
#include <cstring>

namespace gdscript {

// Helper function to convert type hint string to Variant::Type
static IRInstruction::TypeHint type_hint_from_string(const std::string& type_str) {
	// Basic types
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

	// Math types
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

	// Collection types
	else if (type_str == "Array") {
		return Variant::ARRAY;
	} else if (type_str == "Dictionary") {
		return Variant::DICTIONARY;
	}

	// Packed array types
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

	// Special types
	else if (type_str == "RID") {
		return Variant::RID;
	} else if (type_str == "Callable") {
		return Variant::CALLABLE;
	} else if (type_str == "Signal") {
		return Variant::SIGNAL;
	}

	// Unknown type, return NONE
	return IRInstruction::TypeHint_NONE;
}

CodeGenerator::CodeGenerator() {}

IRProgram CodeGenerator::generate(const Program& program) {
	IRProgram ir_program;

	// Collect all locally defined function names, and remember their
	// signatures so that call sites can supply missing default arguments.
	m_local_functions.clear();
	m_local_signatures.clear();
	for (const auto& func : program.functions) {
		m_local_functions.insert(func.name);
		m_local_signatures[func.name] = &func;
	}

	// Structs are visible everywhere: to every function body and to every global
	// initializer. They are collected before anything is lowered so that
	// declaration order does not matter.
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

	// Process global variables.
	//
	// An initializer is lowered in one of two ways. Anything that folds to a
	// compile-time constant becomes an InitType that the backend writes straight
	// into the .globals array, which costs nothing at startup. Everything else -
	// array and dictionary literals, constructor calls, references to earlier
	// globals - is evaluated by the synthetic global_init function through the
	// ordinary expression path, so a global initializer supports exactly what a
	// function body supports. Anything that reaches neither path is an error:
	// leaving the global silently NIL is how initializer support used to
	// regress unnoticed.
	m_global_variables.clear();
	m_global_consts.clear();
	m_global_const_values.clear();
	m_global_types.clear();
	m_global_structs.clear();
	ir_program.globals.resize(program.globals.size());

	// Every global name has to be known before any initializer is lowered, so
	// that an initializer can name another global.
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

		// A global declared as a struct is a Dictionary as far as everything
		// downstream is concerned; which struct is remembered alongside, so that
		// a field access on the global is checked like one on a local.
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

	// The init function is one straight-line block, generated with the same
	// register and scope state that real function bodies use -- its own
	// context, on the stack, like every other function.
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

		// Convert type hint. A struct-typed global is a DICTIONARY, which
		// m_global_types already holds.
		if (!global.type_hint.empty()) {
			ir_global.type_hint = m_global_types[i];
		}

		// Validate that global variables have either a type hint or an initializer
		// This is necessary for complex types (String, Array, Dictionary, etc.) which
		// require VASSIGN for proper reference counting. Without type information, we
		// cannot determine at compile time whether VASSIGN is needed.
		if (global.type_hint.empty() && !global.initializer) {
			error_at("Global variable '" + global.name + "' requires either a type hint or an initializer",
				global.line, global.column,
				"Add ': type' (e.g. ': Array') or an initializer (e.g. '= []'). Without one, "
				"the compiler cannot tell whether stores into it need reference counting.");
		}

		if (!global.initializer) {
			// `var a: BankAccount` is a fresh instance, built at startup like any
			// other Dictionary.
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

			// `var a: Array` is an empty Array in GDScript, not NIL. Give every
			// type-hinted global without an initializer the default value of its
			// type, so that an @export property is registered holding a value of
			// the type it was declared with.
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
				// `var acct = BankAccount.new()` says which struct it is without
				// a type hint, and that is the usual way to write one.
				if (m_global_structs[i] == nullptr) {
					m_global_structs[i] = get_register_struct(init_func, reg);
				}
				free_register(init_func, reg);
				ir_program.has_global_init = true;
			}

			// Remember const values so later initializers can fold references to them.
			if (global.is_const && ir_global.init_type != IRGlobalVar::InitType::RUNTIME) {
				m_global_const_values[global.name] = ir_global;
			}
		}

		// Only globals lowered so far may be referenced by a later initializer.
		m_globals_lowered = i + 1;
	}

	if (ir_program.has_global_init) {
		init_func.ir.instructions.emplace_back(IROpcode::RETURN);
		init_func.ir.max_registers = init_func.next_register;
	} else {
		// Nothing to run: drop whatever was generated for folded initializers.
		init_func.ir.instructions.clear();
		init_func.ir.max_registers = 0;
	}
	pop_scope(init_func);
	ir_program.global_init = std::move(init_func.ir);

	// Function bodies run after every global has been initialized, so all of
	// them are visible regardless of declaration order.
	m_globals_lowered = SIZE_MAX;

	for (const auto& decl : program.functions) {
		ir_program.functions.push_back(generate_function(decl));
	}

	ir_program.string_constants = m_string_constants;
	return ir_program;
}

IRFunction CodeGenerator::generate_function(const FunctionDecl& decl) {
	// One context, on the stack, for exactly as long as this function is being
	// lowered. Nothing survives into the next function, so nothing has to be
	// cleared: virtual register numbers restart at 0 here, and a register type
	// left over from the previous function would put the backend on a native
	// path for a Variant that is not of that type.
	FunctionContext func;
	func.ir.name = decl.name;
	m_current_function = decl.name;

	// Create root scope for function
	push_scope(func);

	// Parameters are passed in registers a0-a7 (RISC-V convention)
	// For simplicity, we'll store them as variables immediately
	for (size_t i = 0; i < decl.parameters.size(); i++) {
		const auto& param = decl.parameters[i];
		func.ir.parameters.push_back(param.name);

		int reg = alloc_register(func);
		// In real implementation, would load from parameter registers
		// For now, assume parameters are already in variables
		declare_variable(func, param.name, reg);

		// Track parameter type if type hint is present. A struct name is a type
		// hint too: it makes the parameter a Dictionary whose fields are known.
		apply_declared_type(reg, param.type_hint, func);
	}

	// Generate code for function body
	for (const auto& stmt : decl.body) {
		gen_stmt(stmt.get(), func);
	}

	// Ensure function returns (add implicit return if needed)
	if (func.ir.instructions.empty() ||
	    func.ir.instructions.back().opcode != IROpcode::RETURN) {
		func.ir.instructions.emplace_back(IROpcode::RETURN);
	}

	func.ir.max_registers = func.next_register;

	// Pop root scope
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
		// No-op
	} else if (auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
		gen_expr_stmt(expr_stmt, func);
	} else {
		// A statement kind that reaches here has been added to the AST without
		// being given a lowering. Falling through instead would generate a
		// program that silently does not contain it.
		error_at("This kind of statement is not supported by the compiler yet", stmt);
	}
}

void CodeGenerator::gen_var_decl(const VarDeclStmt* stmt, FunctionContext& func) {
	const StructDecl* declared_struct = find_struct(stmt->type_hint);
	int reg = -1;

	if (stmt->initializer) {
		reg = gen_expr(stmt->initializer.get(), func);
	} else if (declared_struct != nullptr) {
		// `var a: BankAccount` is a fresh instance, the way `var a: Array` is an
		// empty Array rather than NIL.
		reg = gen_struct_construct(*declared_struct, {}, NamedArguments{}, func, nullptr);
	} else {
		reg = alloc_register(func);
		// Initialize to null/0
		func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
	}

	if (declared_struct != nullptr) {
		// The annotation says which struct this is; the initializer has to be
		// able to be one.
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

	// Track type hint if provided
	if (!stmt->type_hint.empty()) {
		IRInstruction::TypeHint type = type_hint_from_string(stmt->type_hint);
		if (type != IRInstruction::TypeHint_NONE) {
			// The value has to actually become that type, not merely be labelled
			// with it: `var f: float = 0` holds 0.0, and marking an INT Variant
			// as FLOAT would have the backend read its payload as a double.
			reg = coerce_to_declared_type(reg, type, func, "variable '" + stmt->name + "'", stmt);
			set_register_type(func, reg, type);
		}
	} else if (stmt->initializer) {
		// Infer type from initializer
		IRInstruction::TypeHint init_type = get_register_type(func, reg);
		if (init_type != IRInstruction::TypeHint_NONE) {
			set_register_type(func, reg, init_type);
		}
	}

	declare_variable(func, stmt->name, reg, stmt->is_const, stmt);
}

void CodeGenerator::gen_assign(const AssignStmt* stmt, FunctionContext& func) {
	int value_reg = gen_expr(stmt->value.get(), func);

	// Check if this is an indexed assignment (arr[0] = value) or property assignment (obj.prop = value)
	if (stmt->target) {
		// Check for indexed assignment: arr[idx] = value
		if (auto* index_expr = dynamic_cast<const IndexExpr*>(stmt->target.get())) {
			int obj_reg = gen_expr(index_expr->object.get(), func);
			int idx_reg = gen_expr(index_expr->index.get(), func);

			// Use VCALL to call .set(index, value)
			// Format: VCALL result_reg, obj_reg, method_name, arg_count, arg1_reg, arg2_reg
			int result_reg = alloc_register(func);
			IRInstruction vcall_instr(IROpcode::VCALL);
			vcall_instr.operands.push_back(IRValue::reg(result_reg));
			vcall_instr.operands.push_back(IRValue::reg(obj_reg));
			vcall_instr.operands.push_back(IRValue::str("set"));
			vcall_instr.operands.push_back(IRValue::imm(2)); // 2 arguments
			vcall_instr.operands.push_back(IRValue::reg(idx_reg));
			vcall_instr.operands.push_back(IRValue::reg(value_reg));
			func.ir.instructions.push_back(vcall_instr);

			free_register(func, obj_reg);
			free_register(func, idx_reg);
			free_register(func, value_reg);
			free_register(func, result_reg);
			return;
		}

		// Check for property assignment: obj.prop = value
		if (auto* member_expr = dynamic_cast<const MemberCallExpr*>(stmt->target.get())) {
			// Verify this is a property access (not a method call)
			if (member_expr->is_method_call) {
				error_at("Cannot assign to method call", stmt);
			}

			int obj_reg = gen_expr(member_expr->object.get(), func);

			// A struct field, or a key of a Dictionary: both are element
			// writes, not property writes. See the matching read in
			// gen_member_call().
			if (const StructDecl* decl = get_register_struct(func, obj_reg)) {
				const StructField& field = require_struct_field(*decl, member_expr->member_name,
					member_expr->line, member_expr->column);
				if (!field.type_hint.empty()) {
					value_reg = coerce_to_declared_type(value_reg,
						type_hint_from_string(field.type_hint), func,
						"field '" + field.name + "' of struct '" + decl->name + "'", stmt);
				}
				gen_dict_set(obj_reg, member_expr->member_name, value_reg, func);
				free_register(func, obj_reg);
				free_register(func, value_reg);
				return;
			}
			if (get_register_type(func, obj_reg) == Variant::DICTIONARY) {
				gen_dict_set(obj_reg, member_expr->member_name, value_reg, func);
				free_register(func, obj_reg);
				free_register(func, value_reg);
				return;
			}

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
			func.ir.instructions.push_back(vset_instr);

			free_register(func, obj_reg);
			free_register(func, value_reg);
			return;
		}

		error_at("Invalid assignment target type", stmt);
	}

	// Simple variable assignment.
	// Locals shadow globals, so the enclosing scopes are searched first and the
	// global table is only consulted when no local of that name is in scope.
	Variable* var = find_variable(func, stmt->name);
	if (!var) {
		if (is_global_variable(stmt->name)) {
			if (is_global_const(stmt->name)) {
				error_at("Cannot assign to const variable: " + stmt->name, stmt);
			}
			size_t global_idx = m_global_variables.at(stmt->name);
			value_reg = coerce_to_declared_type(value_reg, m_global_types[global_idx], func,
				"global '" + stmt->name + "'", stmt);
			func.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL, IRValue::imm(global_idx), IRValue::reg(value_reg));
			free_register(func, value_reg);
			return;
		}
		error_at("Undefined variable: " + stmt->name, stmt,
			"Declare it with 'var " + stmt->name + " = ...' before assigning to it");
	}

	// Check if variable is const
	if (var->is_const) {
		error_at("Cannot assign to const variable: " + stmt->name, stmt);
	}

	value_reg = coerce_to_declared_type(value_reg, get_register_type(func, var->register_num), func,
		"variable '" + stmt->name + "'", stmt);

	// Store value into variable's register
	if (var->register_num != value_reg) {
		func.ir.instructions.emplace_back(IROpcode::MOVE,
		                               IRValue::reg(var->register_num),
		                               IRValue::reg(value_reg));
	}

	free_register(func, value_reg);
}

void CodeGenerator::gen_return(const ReturnStmt* stmt, FunctionContext& func) {
	if (stmt->value) {
		int reg = gen_expr(stmt->value.get(), func);
		// Move return value to register 0 (return register)
		// Skip if already in register 0
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
	// The tested register's type travels with the branch so that the backend can
	// decide truthiness inline for the types it knows and only ask the host about
	// the ones it does not.
	IRInstruction branch(opcode, IRValue::reg(cond_reg), IRValue::label(label));
	branch.type_hint = get_register_type(func, cond_reg);
	func.ir.instructions.push_back(branch);
}

void CodeGenerator::gen_if(const IfStmt* stmt, FunctionContext& func) {
	std::string else_label = make_label("else");
	std::string end_label = make_label("endif");

	// Evaluate condition
	int cond_reg = gen_expr(stmt->condition.get(), func);

	// Branch to else if condition is zero (false)
	if (!stmt->else_branch.empty()) {
		emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, else_label, func);
	} else {
		emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
	}

	free_register(func, cond_reg);

	// Then branch (new scope)
	push_scope(func);
	for (const auto& s : stmt->then_branch) {
		gen_stmt(s.get(), func);
	}
	pop_scope(func);

	if (!stmt->else_branch.empty()) {
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(end_label));

		// Else branch (new scope)
		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(else_label));
		push_scope(func);
		for (const auto& s : stmt->else_branch) {
			gen_stmt(s.get(), func);
		}
		pop_scope(func);
	}

	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));
}

void CodeGenerator::gen_match(const MatchStmt* stmt, FunctionContext& func) {
	// Lowered to a chain of equality tests against the subject, which is
	// evaluated exactly once:
	//
	//     match x:      ->    t = x
	//         1, 2:           if t == 1 or t == 2: <body>; goto end
	//             body        ...
	//         _:              <wildcard body>
	//             body    end:
	std::string end_label = make_label("endmatch");

	int subject_reg = gen_expr(stmt->subject.get(), func);

	for (const auto& branch : stmt->branches) {
		if (branch.patterns.empty()) {
			// Wildcard '_': always taken, and the parser guarantees it is last
			push_scope(func);
			for (const auto& s : branch.body) {
				gen_stmt(s.get(), func);
			}
			pop_scope(func);
			break;
		}

		std::string body_label = make_label("match_body");
		std::string next_label = make_label("match_next");

		// Jump to the body when any of this branch's patterns compares equal
		for (const auto& pattern : branch.patterns) {
			int pattern_reg = gen_expr(pattern.get(), func);
			IRInstruction cmp(IROpcode::CMP_EQ, IRValue::reg(pattern_reg),
			                  IRValue::reg(subject_reg), IRValue::reg(pattern_reg));
			func.ir.instructions.push_back(cmp);
			emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, pattern_reg, body_label, func);
			free_register(func, pattern_reg);
		}
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(next_label));

		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(body_label));
		push_scope(func);
		for (const auto& s : branch.body) {
			gen_stmt(s.get(), func);
		}
		pop_scope(func);
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(end_label));

		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(next_label));
	}

	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));
	free_register(func, subject_reg);
}

void CodeGenerator::gen_while(const WhileStmt* stmt, FunctionContext& func) {
	std::string loop_label = make_label("loop");
	std::string end_label = make_label("endloop");

	// Push loop context for break/continue
	func.loops.push_back({end_label, loop_label});

	// Loop start
	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(loop_label));

	// Evaluate condition
	int cond_reg = gen_expr(stmt->condition.get(), func);
	emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
	free_register(func, cond_reg);

	// Loop body (new scope)
	push_scope(func);
	for (const auto& s : stmt->body) {
		gen_stmt(s.get(), func);
	}
	pop_scope(func);

	// Jump back to loop start
	func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(loop_label));

	// Loop end
	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));

	func.loops.pop_back();
}

void CodeGenerator::gen_for(const ForStmt* stmt, FunctionContext& func) {
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
			error_at("Cannot iterate over a non-iterable value in a 'for' loop", stmt,
				"Did you mean 'for " + stmt->variable + " in range(" +
				(literal->lit_type == LiteralExpr::Type::INTEGER ?
				 std::to_string(std::get<int64_t>(literal->value)) : "N") + "):'?");
		}
	}

	if (!is_range) {
		// Array iteration
		std::string loop_label = make_label("for_loop");
		std::string continue_label = make_label("for_continue");
		std::string end_label = make_label("for_end");

		// Push loop context for break/continue
		func.loops.push_back({end_label, continue_label});

		// Create new scope for loop (includes loop variable)
		push_scope(func);

		int array_reg = gen_expr(stmt->iterable.get(), func);

		// Initialize index counter with 0
		int index_reg = alloc_register(func);
		auto& index_load = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(index_reg), IRValue::imm(0));

		// Loop start
		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(loop_label));

		// Call ECALL_ARRAY_SIZE to get the array size
		// ECALL_ARRAY_SIZE = GAME_API_BASE + 23 = 523
		int size_reg = alloc_register(func);
		IRInstruction size_syscall(IROpcode::CALL_SYSCALL);
		size_syscall.operands.push_back(IRValue::reg(size_reg));  // result register
		size_syscall.operands.push_back(IRValue::imm(523));         // ECALL_ARRAY_SIZE
		size_syscall.operands.push_back(IRValue::reg(array_reg));   // array register
		func.ir.instructions.push_back(size_syscall);

		// Condition: index < size
		int cond_reg = alloc_register(func);
		auto& cmp_instr = func.ir.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
		                               IRValue::reg(index_reg), IRValue::reg(size_reg));

		emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
		free_register(func, cond_reg);

		// Get element from array using ECALL_ARRAY_AT
		// ECALL_ARRAY_AT = GAME_API_BASE + 22 = 522
		int elem_reg = alloc_register(func);
		IRInstruction at_syscall(IROpcode::CALL_SYSCALL);
		at_syscall.operands.push_back(IRValue::reg(elem_reg));    // result register (element)
		at_syscall.operands.push_back(IRValue::imm(522));         // ECALL_ARRAY_AT
		at_syscall.operands.push_back(IRValue::reg(array_reg));   // array register
		at_syscall.operands.push_back(IRValue::reg(index_reg));   // index register
		func.ir.instructions.push_back(at_syscall);

		// Assign the element to the loop variable
		declare_variable(func, stmt->variable, elem_reg, false, stmt);

		// Loop body (new scope for body, separate from loop variable scope)
		push_scope(func);
		for (const auto& s : stmt->body) {
			gen_stmt(s.get(), func);
		}
		pop_scope(func);

		// Continue label - where continue jumps to
		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(continue_label));

		// Increment: index = index + 1
		int new_idx_reg = alloc_register(func);
		auto& add_instr = func.ir.instructions.emplace_back(IROpcode::ADD, IRValue::reg(new_idx_reg),
		                               IRValue::reg(index_reg), IRValue::imm(1));
		auto& move_instr = func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(index_reg), IRValue::reg(new_idx_reg));
		free_register(func, new_idx_reg);

		// Jump back to loop start
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(loop_label));

		// Loop end
		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));

		// Clean up
		pop_scope(func);
		func.loops.pop_back();
		// Note: array_reg and size_reg are allocated inside the loop, so they're freed each iteration
		free_register(func, index_reg);
		free_register(func, elem_reg);
		return;
	}

	// Generate range() arguments
	// range(n) -> 0 to n-1
	// range(start, end) -> start to end-1
	// range(start, end, step) -> start to end-1 by step

	int start_reg = -1, end_reg = -1, step_reg = -1;

	if (call_expr->arguments.size() == 1) {
		// range(n): start=0, end=n, step=1
		start_reg = alloc_register(func);
		auto& start_instr = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(start_reg), IRValue::imm(0));
		end_reg = gen_expr(call_expr->arguments[0].get(), func);
		step_reg = alloc_register(func);
		auto& step_instr = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(step_reg), IRValue::imm(1));
	} else if (call_expr->arguments.size() == 2) {
		// range(start, end): step=1
		start_reg = gen_expr(call_expr->arguments[0].get(), func);
		end_reg = gen_expr(call_expr->arguments[1].get(), func);
		step_reg = alloc_register(func);
		auto& step_instr = func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(step_reg), IRValue::imm(1));
	} else if (call_expr->arguments.size() == 3) {
		// range(start, end, step)
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

	// Push loop context for break/continue
	// Continue should jump to the increment step, not the condition check
	func.loops.push_back({end_label, continue_label});

	// Create new scope for loop (includes loop variable)
	push_scope(func);

	// Initialize loop variable with start value
	int loop_var_reg = alloc_register(func);
	auto& move_instr = func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(loop_var_reg), IRValue::reg(start_reg));
	declare_variable(func, stmt->variable, loop_var_reg, false, stmt);

	// Loop start
	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(loop_label));

	// Condition depends on step direction:
	// - If step > 0: loop_var < end
	// - If step < 0: loop_var > end
	// - If step == 0: infinite loop (but that's a user error)
	//
	// For runtime step values, we need to check the sign dynamically.
	// For now, we'll check if step is a compile-time constant to optimize.

	int cond_reg = alloc_register(func);

	// Check if step is a constant value
	bool step_is_constant = false;
	int64_t step_value = 0;

	// Look back one instruction to see if step_reg was loaded with LOAD_IMM
	if (func.ir.instructions.size() >= 3) {
		auto& prev_instr = func.ir.instructions[func.ir.instructions.size() - 3];
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
			auto& cmp_instr = func.ir.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
			                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
			cmp_instr.type_hint = Variant::INT; // range() always produces integers
		} else {
			// Backward iteration: loop_var > end
			auto& cmp_instr = func.ir.instructions.emplace_back(IROpcode::CMP_GT, IRValue::reg(cond_reg),
			                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
			cmp_instr.type_hint = Variant::INT; // range() always produces integers
		}
	} else {
		// Runtime step: check sign dynamically
		// if step >= 0: check loop_var < end
		// else: check loop_var > end
		std::string pos_step_label = make_label("for_pos_step");
		std::string check_cond_label = make_label("for_check_cond");

		int zero_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(zero_reg), IRValue::imm(0));

		int step_sign_reg = alloc_register(func);
		auto& step_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_GTE, IRValue::reg(step_sign_reg),
		                               IRValue::reg(step_reg), IRValue::reg(zero_reg));
		step_cmp.type_hint = Variant::INT; // range() always produces integers
		free_register(func, zero_reg);

		// If step >= 0, use loop_var < end
		emit_conditional_branch(IROpcode::BRANCH_NOT_ZERO, step_sign_reg, pos_step_label, func);

		// Negative step: loop_var > end
		auto& neg_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_GT, IRValue::reg(cond_reg),
		                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
		neg_cmp.type_hint = Variant::INT; // range() always produces integers
		func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(check_cond_label));

		// Positive step: loop_var < end
		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(pos_step_label));
		auto& pos_cmp = func.ir.instructions.emplace_back(IROpcode::CMP_LT, IRValue::reg(cond_reg),
		                               IRValue::reg(loop_var_reg), IRValue::reg(end_reg));
		pos_cmp.type_hint = Variant::INT; // range() always produces integers

		func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(check_cond_label));
		free_register(func, step_sign_reg);
	}

	emit_conditional_branch(IROpcode::BRANCH_ZERO, cond_reg, end_label, func);
	free_register(func, cond_reg);

	// Loop body (new scope for body, separate from loop variable scope)
	push_scope(func);
	for (const auto& s : stmt->body) {
		gen_stmt(s.get(), func);
	}
	pop_scope(func);

	// Continue label - where continue jumps to
	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(continue_label));

	// Increment: loop_var = loop_var + step
	int new_val_reg = alloc_register(func);
	auto& add_instr = func.ir.instructions.emplace_back(IROpcode::ADD, IRValue::reg(new_val_reg),
	                               IRValue::reg(loop_var_reg), IRValue::reg(step_reg));
	add_instr.type_hint = Variant::INT; // range() always produces integers
	auto& move_instr2 = func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(loop_var_reg), IRValue::reg(new_val_reg));
	free_register(func, new_val_reg);

	// Jump back to loop start
	func.ir.instructions.emplace_back(IROpcode::JUMP, IRValue::label(loop_label));

	// Loop end
	func.ir.instructions.emplace_back(IROpcode::LABEL, IRValue::label(end_label));

	// Clean up
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
		// Every expression kind has to be lowered by one of the branches above.
		// Returning a default register here instead would compile an expression
		// the program never evaluates.
		error_at("This kind of expression is not supported by the compiler yet", expr);
	}
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
	// Locals shadow everything else, so the enclosing scopes are searched before
	// globals and global class names. 'self' is not a declarable name, so it is
	// handled below rather than here.
	if (Variable* local = find_variable(func, expr->name)) {
		int new_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(new_reg), IRValue::reg(local->register_num));

		IRInstruction::TypeHint type = get_register_type(func, local->register_num);
		if (type != IRInstruction::TypeHint_NONE) {
			set_register_type(func, new_reg, type);
		}
		// A struct instance is a Dictionary, and a Dictionary is a handle: the
		// copy refers to the same fields, so it knows the same struct.
		set_register_struct(func, new_reg, get_register_struct(func, local->register_num));
		return new_reg;
	}

	// Check if this is a global class reference
	if (is_global_class(expr->name)) {
		return gen_global_class_get(expr->name, func);
	}

	// Handle 'self' as an alias for get_node()
	if (expr->name == "self") {
		// Generate get_node() call
		int result_reg = alloc_register(func);

		// CALL_SYSCALL result_reg, ECALL_GET_NODE, 0
		IRInstruction instr(IROpcode::CALL_SYSCALL);
		instr.operands.push_back(IRValue::reg(result_reg));    // result register
		instr.operands.push_back(IRValue::imm(507));            // ECALL_GET_NODE
		instr.operands.push_back(IRValue::imm(0));              // addr = 0 (owner node)
		func.ir.instructions.push_back(instr);

		return result_reg;
	}

	// Check if this is a global variable
	if (is_global_variable(expr->name)) {
		size_t global_idx = m_global_variables.at(expr->name);
		// While the global initializers are being lowered, a global that has not
		// been initialized yet still holds NIL. Reading it would silently produce
		// the wrong value, so a forward reference is rejected instead.
		if (global_idx >= m_globals_lowered) {
			error_at("Global variable '" + expr->name + "' is used in the initializer of a global "
				"declared before it", expr,
				"Move the declaration of '" + expr->name + "' above that global");
		}
		int result_reg = alloc_register(func);
		func.ir.instructions.emplace_back(IROpcode::LOAD_GLOBAL, IRValue::reg(result_reg), IRValue::imm(global_idx));
		set_register_struct(func, result_reg, m_global_structs[global_idx]);
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
	// GDScript's 'and' and 'or' short-circuit: the right-hand side is only
	// evaluated when the left-hand side does not already decide the result, and
	// the result is a bool rather than one of the operands. Lowering them to a
	// plain binary IR op evaluates both sides unconditionally, which runs the
	// right side's side effects even when it should never have been reached.
	//
	//     a and b   ->   r = false; if !a goto end; if !b goto end; r = true
	//     a or b    ->   r = true;  if a  goto end; if b  goto end; r = false
	const bool is_and = expr->op == BinaryExpr::Op::AND;
	const std::string end_label = make_label(is_and ? "and_end" : "or_end");
	const IROpcode short_circuit_branch = is_and ? IROpcode::BRANCH_ZERO : IROpcode::BRANCH_NOT_ZERO;

	int result_reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(result_reg),
		IRValue::imm(is_and ? 0 : 1));
	set_register_type(func, result_reg, Variant::BOOL);

	int left_reg = gen_expr(expr->left.get(), func);
	IRInstruction left_branch(short_circuit_branch, IRValue::reg(left_reg), IRValue::label(end_label));
	// The type hint lets the backend test truthiness inline instead of asking
	// the host what the Variant booleanizes to.
	left_branch.type_hint = get_register_type(func, left_reg);
	func.ir.instructions.push_back(left_branch);
	free_register(func, left_reg);

	int right_reg = gen_expr(expr->right.get(), func);
	IRInstruction right_branch(short_circuit_branch, IRValue::reg(right_reg), IRValue::label(end_label));
	right_branch.type_hint = get_register_type(func, right_reg);
	func.ir.instructions.push_back(right_branch);
	free_register(func, right_reg);

	// Neither test short-circuited, so the result is the opposite of the value
	// the short circuit would have produced.
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

	// Check type hints for operands to determine if result should be float
	IRInstruction::TypeHint left_type = get_register_type(func, left_reg);
	IRInstruction::TypeHint right_type = get_register_type(func, right_reg);

	// Determine if this is an arithmetic operation (vs comparison or logical)
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

	// For arithmetic operations and comparisons:
	// ONLY set type hint when BOTH operands have the SAME type
	// This enables native RISC-V codegen optimizations
	//
	// When types don't match (e.g. INT + FLOAT), we leave result_type as NONE
	// and fall back to VEVAL syscall which handles type coercion correctly
	IRInstruction::TypeHint result_type = IRInstruction::TypeHint_NONE;
	if (is_bitwise) {
		// Bitwise operators are integer-only in GDScript. Only take the native
		// path when both operands are known to be integers; otherwise fall
		// back to VEVAL, which reports the type error at runtime.
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
		case BinaryExpr::Op::BIT_AND: op = IROpcode::BIT_AND; break;
		case BinaryExpr::Op::BIT_OR: op = IROpcode::BIT_OR; break;
		case BinaryExpr::Op::BIT_XOR: op = IROpcode::BIT_XOR; break;
		case BinaryExpr::Op::SHL: op = IROpcode::SHL; break;
		case BinaryExpr::Op::SHR: op = IROpcode::SHR; break;
		default:
			error_at("Unknown binary operator", expr);
	}

	IRInstruction instr(op, IRValue::reg(result_reg), IRValue::reg(left_reg), IRValue::reg(right_reg));
	instr.type_hint = result_type;
	func.ir.instructions.push_back(instr);

	if (is_comparison) {
		// A comparison always produces a bool, whatever its operands were. The
		// instruction's type hint describes the *operands* - it is what selects
		// the backend's native compare path - so only the destination register's
		// tracked type is corrected here.
		set_register_type(func, result_reg, Variant::BOOL);
	} else if (result_type != IRInstruction::TypeHint_NONE) {
		set_register_type(func, result_reg, result_type);
	}

	free_register(func, left_reg);
	free_register(func, right_reg);

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
		// ~int is always an int, so the native path can be used
		instr.type_hint = Variant::INT;
		set_register_type(func, result_reg, Variant::INT);
	} else if (expr->op == UnaryExpr::Op::NOT) {
		// 'not' booleanizes its operand, so the result is a bool for every
		// operand type.
		set_register_type(func, result_reg, Variant::BOOL);
	} else if (expr->op == UnaryExpr::Op::NEG) {
		// Negation preserves the numeric type.
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
	// <true_value> if <condition> else <false_value>
	//
	// Only the taken branch is evaluated, so this is lowered to real control
	// flow rather than evaluating both sides.
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

	// The result type is only known when both arms agree on it
	if (true_type != IRInstruction::TypeHint_NONE && true_type == false_type) {
		set_register_type(func, result_reg, true_type);
	}

	return result_reg;
}

int CodeGenerator::gen_call(const CallExpr* expr, FunctionContext& func) {
	// A struct name used as a call constructs an instance: BankAccount(10, 50).
	// Checked before the arguments are lowered, because a struct constructor is
	// the one call that can name them.
	if (const StructDecl* decl = find_struct(expr->function_name)) {
		return gen_struct_construct(*decl, expr->arguments, *expr, func, expr);
	}
	reject_named_arguments(*expr, "'" + expr->function_name + "'", expr);

	// Generate code for arguments
	std::vector<int> arg_regs;
	for (const auto& arg : expr->arguments) {
		arg_regs.push_back(gen_expr(arg.get(), func));
	}

	// Check if this is an inline primitive constructor
	if (is_inline_primitive_constructor(expr->function_name)) {
		int result = gen_inline_constructor(expr->function_name, arg_regs, func, expr);
		for (int reg : arg_regs) {
			free_register(func, reg);
		}
		return result;
	}

	// Handle get_node() as a special syscall
	if (expr->function_name == "get_node") {
		// get_node() takes 0 or 1 argument (node path)
		if (arg_regs.size() > 1) {
			error_at("get_node() takes at most 1 argument", expr);
		}

		int result_reg = alloc_register(func);

		if (arg_regs.empty()) {
			// get_node() with no args - get the owner node
			// CALL_SYSCALL result_reg, ECALL_GET_NODE, 0
			IRInstruction instr(IROpcode::CALL_SYSCALL);
			instr.operands.push_back(IRValue::reg(result_reg));    // result register
			instr.operands.push_back(IRValue::imm(507));            // ECALL_GET_NODE
			instr.operands.push_back(IRValue::imm(0));              // addr = 0 (owner node)
			func.ir.instructions.push_back(instr);
		} else {
			// get_node(path) - will be handled in RISC-V codegen
			// For now, convert to CALL_SYSCALL with the path argument
			int path_reg = arg_regs[0];

			IRInstruction instr(IROpcode::CALL_SYSCALL);
			instr.operands.push_back(IRValue::reg(result_reg));    // result register
			instr.operands.push_back(IRValue::imm(507));            // ECALL_GET_NODE
			instr.operands.push_back(IRValue::imm(0));              // addr = 0 (owner node)
			instr.operands.push_back(IRValue::reg(path_reg));       // path register
			func.ir.instructions.push_back(instr);
		}

		for (int reg : arg_regs) {
			free_register(func, reg);
		}

		return result_reg;
	}

	// Check if this is a call to a locally defined function
	if (is_local_function(expr->function_name)) {
		// Fill in default values for any arguments the call site omitted.
		// The Sandbox ABI does not pass an argument count to the guest, so a
		// callee cannot tell which arguments it actually received; defaults
		// therefore have to be materialized here, at the call site.
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

		// Local function call - use regular CALL instruction
		int result_reg = alloc_register(func);

		// `func open() -> BankAccount` makes the result a known instance, so a
		// field access on it is checked at the call site.
		if (sig != m_local_signatures.end()) {
			if (const StructDecl* returned = find_struct(sig->second->return_type)) {
				set_register_struct(func, result_reg, returned);
			}
		}

		// Generate CALL instruction with function name, result register, and argument registers
		// Format: CALL function_name, result_reg, arg_count, arg1_reg, arg2_reg, ...
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

	// GDScript globals. Checked after local functions, so a script that defines
	// its own print() still calls its own, and before the self-call fallback,
	// which would turn print(x) into a Node.print(x) that does nothing.
	if (is_global_function(expr->function_name)) {
		int result = gen_global_function(expr, arg_regs, func);
		for (int reg : arg_regs) {
			free_register(func, reg);
		}
		return result;
	}

	// Treat all other freestanding function calls as self-calls
	// Convert foo(arg1, arg2) to self.foo(arg1, arg2)
	int self_reg = alloc_register(func);

	// Generate get_node() for self
	// CALL_SYSCALL self_reg, ECALL_GET_NODE, 0
	IRInstruction get_self_instr(IROpcode::CALL_SYSCALL);
	get_self_instr.operands.push_back(IRValue::reg(self_reg));     // result register
	get_self_instr.operands.push_back(IRValue::imm(507));            // ECALL_GET_NODE
	get_self_instr.operands.push_back(IRValue::imm(0));              // addr = 0 (owner node)
	func.ir.instructions.push_back(get_self_instr);

	int result_reg = alloc_register(func);

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
	func.ir.instructions.push_back(vcall_instr);

	free_register(func, self_reg);
	for (int reg : arg_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

int CodeGenerator::gen_member_call(const MemberCallExpr* expr, FunctionContext& func) {
	// BankAccount.new(...) constructs an instance. The object is a struct name
	// rather than a value, so this comes before the object is lowered.
	if (expr->is_method_call && expr->member_name == "new") {
		if (auto* object = dynamic_cast<const VariableExpr*>(expr->object.get())) {
			if (const StructDecl* decl = find_struct(object->name)) {
				return gen_struct_construct(*decl, expr->arguments, *expr, func, expr);
			}
		}
	}
	reject_named_arguments(*expr, "'" + expr->member_name + "'", expr);

	int obj_reg = gen_expr(expr->object.get(), func);

	// Generate code for arguments
	std::vector<int> arg_regs;
	for (const auto& arg : expr->arguments) {
		arg_regs.push_back(gen_expr(arg.get(), func));
	}

	// Check if this is inline member access (x, y, z, r, g, b, a on vectors)
	if (!expr->is_method_call && arg_regs.empty()) {
		IRInstruction::TypeHint obj_type = get_register_type(func, obj_reg);
		if (is_inline_member_access(obj_type, expr->member_name)) {
			int result = gen_inline_member_get(obj_reg, obj_type, expr->member_name, func);
			free_register(func, obj_reg);
			return result;
		}

		// A struct field. The name has to be one the struct declares: catching
		// the misspelling here is what a struct buys over a bare Dictionary.
		if (const StructDecl* decl = get_register_struct(func, obj_reg)) {
			const StructField& field = require_struct_field(*decl, expr->member_name,
				expr->line, expr->column);
			int result_reg = gen_dict_get(obj_reg, expr->member_name, func);
			apply_declared_type(result_reg, field.type_hint, func);
			free_register(func, obj_reg);
			return result_reg;
		}

		// A key of a Dictionary. The property-get syscall below reaches an
		// Object's properties and nothing else, so a Dictionary -- which in
		// GDScript answers d.key exactly as d["key"] does -- takes the element
		// path instead of throwing at run time.
		if (obj_type == Variant::DICTIONARY) {
			int result_reg = gen_dict_get(obj_reg, expr->member_name, func);
			free_register(func, obj_reg);
			return result_reg;
		}

		// Property access: obj.property (no parentheses)
		// Use dedicated VGET instruction with ECALL_OBJ_PROP_GET syscall
		int result_reg = alloc_register(func);

		// Get string index for property name
		int str_idx = add_string_constant(expr->member_name);

		// Emit VGET instruction
		// Format: VGET result_reg, obj_reg, string_idx, string_len
		IRInstruction vget_instr(IROpcode::VGET);
		vget_instr.operands.push_back(IRValue::reg(result_reg));
		vget_instr.operands.push_back(IRValue::reg(obj_reg));
		vget_instr.operands.push_back(IRValue::imm(str_idx));
		vget_instr.operands.push_back(IRValue::imm(static_cast<int64_t>(expr->member_name.length())));
		func.ir.instructions.push_back(vget_instr);

		free_register(func, obj_reg);
		return result_reg;
	}

	int result_reg = alloc_register(func);

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
	func.ir.instructions.push_back(vcall_instr);

	free_register(func, obj_reg);
	for (int reg : arg_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

int CodeGenerator::gen_index(const IndexExpr* expr, FunctionContext& func) {
	int obj_reg = gen_expr(expr->object.get(), func);
	int idx_reg = gen_expr(expr->index.get(), func);

	int result_reg = alloc_register(func);

	// Transform arr[x] to arr.get(x) using VCALL
	// Format: VCALL result_reg, obj_reg, method_name, arg_count, arg1_reg
	IRInstruction vcall_instr(IROpcode::VCALL);
	vcall_instr.operands.push_back(IRValue::reg(result_reg));
	vcall_instr.operands.push_back(IRValue::reg(obj_reg));
	vcall_instr.operands.push_back(IRValue::str("get"));
	vcall_instr.operands.push_back(IRValue::imm(1)); // 1 argument
	vcall_instr.operands.push_back(IRValue::reg(idx_reg));
	func.ir.instructions.push_back(vcall_instr);

	free_register(func, obj_reg);
	free_register(func, idx_reg);

	return result_reg;
}

int CodeGenerator::gen_array_literal(const ArrayLiteralExpr* expr, FunctionContext& func) {
	std::vector<int> elem_regs;

	// Generate code for each element
	for (const auto& elem : expr->elements) {
		int reg = gen_expr(elem.get(), func);
		elem_regs.push_back(reg);
	}

	int result_reg = alloc_register(func);

	// Create MAKE_ARRAY instruction
	// Format: MAKE_ARRAY result_reg, element_count, elem1_reg, elem2_reg, ...
	IRInstruction instr(IROpcode::MAKE_ARRAY);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(static_cast<int>(elem_regs.size()))); // element count
	for (int reg : elem_regs) {
		instr.operands.push_back(IRValue::reg(reg));
	}

	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, Variant::ARRAY);

	// Free element registers
	for (int reg : elem_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

int CodeGenerator::gen_dictionary_literal(const DictionaryLiteralExpr* expr, FunctionContext& func) {
	std::vector<int> key_regs;
	std::vector<int> value_regs;

	// Generate code for each key-value pair
	for (const auto& [key, value] : expr->elements) {
		int key_reg = gen_expr(key.get(), func);
		int value_reg = gen_expr(value.get(), func);
		key_regs.push_back(key_reg);
		value_regs.push_back(value_reg);
	}

	int result_reg = alloc_register(func);

	// Create MAKE_DICTIONARY instruction
	// Format: MAKE_DICTIONARY result_reg, pair_count, key1_reg, val1_reg, key2_reg, val2_reg, ...
	IRInstruction instr(IROpcode::MAKE_DICTIONARY);
	instr.operands.push_back(IRValue::reg(result_reg));
	instr.operands.push_back(IRValue::imm(static_cast<int>(key_regs.size()))); // pair count

	// Interleave keys and values: key1, val1, key2, val2, ...
	for (size_t i = 0; i < key_regs.size(); i++) {
		instr.operands.push_back(IRValue::reg(key_regs[i]));
		instr.operands.push_back(IRValue::reg(value_regs[i]));
	}

	func.ir.instructions.push_back(instr);
	set_register_type(func, result_reg, Variant::DICTIONARY);

	// Free key and value registers
	for (int reg : key_regs) {
		free_register(func, reg);
	}
	for (int reg : value_regs) {
		free_register(func, reg);
	}

	return result_reg;
}

// -= Structs =-
//
// Nothing about a struct survives into the IR: an instance is a Dictionary, a
// field read is Dictionary.get(), a field write is Dictionary.set(). What the
// declaration buys is the set of keys, which is what lets a misspelled field
// be a compile error instead of a silently added key.

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
	// An instance is a Dictionary and nothing else, so the two facts are set
	// together and cannot disagree.
	set_register_type(func, reg, Variant::DICTIONARY);
}

const StructDecl* CodeGenerator::get_register_struct(const FunctionContext& func, int reg) const {
	auto it = func.register_structs.find(reg);
	return it == func.register_structs.end() ? nullptr : it->second;
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

	int result_reg = alloc_register(func);
	IRInstruction vcall(IROpcode::VCALL);
	vcall.operands.push_back(IRValue::reg(result_reg));
	vcall.operands.push_back(IRValue::reg(obj_reg));
	vcall.operands.push_back(IRValue::str("get"));
	vcall.operands.push_back(IRValue::imm(1));
	vcall.operands.push_back(IRValue::reg(key_reg));
	func.ir.instructions.push_back(vcall);

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

	// Dictionary.set() returns whether the key was already there. Nothing reads
	// it, but VCALL defines a destination, so it gets one of its own.
	int result_reg = alloc_register(func);
	IRInstruction vcall(IROpcode::VCALL);
	vcall.operands.push_back(IRValue::reg(result_reg));
	vcall.operands.push_back(IRValue::reg(obj_reg));
	vcall.operands.push_back(IRValue::str("set"));
	vcall.operands.push_back(IRValue::imm(2));
	vcall.operands.push_back(IRValue::reg(key_reg));
	vcall.operands.push_back(IRValue::reg(value_reg));
	func.ir.instructions.push_back(vcall);

	free_register(func, key_reg);
	free_register(func, result_reg);
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

	// The types built from components: a zeroed one is the same instruction the
	// constructor written out by hand would produce.
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

	// Color, Array, Dictionary and the packed arrays each have a zero-argument
	// form already.
	if (is_inline_primitive_constructor(type_hint)) {
		return gen_inline_constructor(type_hint, {}, func, nullptr);
	}

	// Everything else (Object, Callable, Transform3D, a class name) has no
	// default the guest can construct.
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

	// A field declared as another struct defaults to an instance of it, the way
	// a field declared as an Array defaults to an empty Array.
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

	// No declared type, or one with no constructible default: NIL, which is
	// what `var x` alone means in GDScript.
	reg = alloc_register(func);
	func.ir.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(0));
	return reg;
}

int CodeGenerator::gen_struct_construct(const StructDecl& decl, const std::vector<ExprPtr>& arguments,
	const NamedArguments& names, FunctionContext& func, const Expr* site)
{
	// Which field each argument supplies, resolved before anything is
	// evaluated, so that a bad call site is reported rather than half-lowered.
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

	// The call site's expressions run in the order they were written, which is
	// not the order the fields are declared in once names are involved.
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

	// Everything the call site left out comes from the declaration. The struct
	// stays on the stack for as long as its defaults are being built, so a
	// struct that holds itself is reported instead of recursing forever.
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
	// In a more sophisticated version, would track free registers
	// For now, registers are never reused within a function
	(void) func;
	(void) reg;
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

void CodeGenerator::push_scope(FunctionContext& func) {
	Scope new_scope;
	if (func.scopes.empty()) {
		new_scope.parent_scope_idx = SIZE_MAX; // Root scope
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
	// Search from innermost to outermost scope
	for (int i = static_cast<int>(func.scopes.size()) - 1; i >= 0; i--) {
		auto it = func.scopes[i].variables.find(name);
		if (it != func.scopes[i].variables.end()) {
			return &it->second;
		}
	}
	return nullptr; // Not found
}

void CodeGenerator::declare_variable(FunctionContext& func, const std::string& name, int register_num, bool is_const,
	const Stmt* site)
{
	if (func.scopes.empty()) {
		throw CompilerException(ErrorType::CODEGEN_ERROR, "Cannot declare variable: no scope active");
	}

	// Check if variable already exists in current scope (shadowing is allowed, but redeclaration in same scope is not)
	auto& current_scope = func.scopes.back();
	if (current_scope.variables.find(name) != current_scope.variables.end()) {
		error_at("Variable '" + name + "' is already declared in this scope", site);
	}

	current_scope.variables[name] = {name, register_num, IRInstruction::TypeHint_NONE, is_const};
}

// Type tracking helpers
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
	return name == "print";
}

int CodeGenerator::gen_global_function(const CallExpr* expr, std::vector<int>& arg_regs, FunctionContext& func) {
	// print(...) -> ECALL_PRINT. The host takes one contiguous array of
	// Variants, so the count travels with the instruction and the backend does
	// the gathering.
	if (expr->function_name == "print") {
		// The syscall rejects 64 or more, and it does so by throwing inside the
		// guest. Catching it here names the call site instead.
		if (arg_regs.size() >= 64) {
			error_at("print() takes at most 63 arguments", expr);
		}

		int result_reg = alloc_register(func);

		IRInstruction instr(IROpcode::PRINT);
		instr.operands.push_back(IRValue::reg(result_reg));
		instr.operands.push_back(IRValue::imm(arg_regs.size()));
		for (int arg_reg : arg_regs) {
			instr.operands.push_back(IRValue::reg(arg_reg));
		}
		func.ir.instructions.push_back(instr);

		// GDScript's print() evaluates to null.
		set_register_type(func, result_reg, Variant::NIL);
		return result_reg;
	}

	throw CompilerException(ErrorType::CODEGEN_ERROR,
		"is_global_function() accepts '" + expr->function_name + "' but gen_global_function() does not handle it");
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
		// Color() with 0 args: white (1, 1, 1, 1)
		// Color(r, g, b) with 3 args: default alpha to 1.0
		// Color(r, g, b, a) with 4 args: full specification
		if (arg_regs.size() == 0) {
			// Color() - white with alpha 1.0
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
			// Color(r, g, b) - default alpha to 1.0
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

	// Result is always a float or int Variant
	bool is_int_vector = (obj_type == Variant::VECTOR2I ||
	                      obj_type == Variant::VECTOR3I ||
	                      obj_type == Variant::VECTOR4I);

	set_register_type(func, result_reg, is_int_vector ? Variant::INT : Variant::FLOAT);

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
		// Either already the right type, or not known until run time - in which
		// case the host checks it on assignment.
		return reg;
	}

	// int -> float is GDScript's one implicit numeric conversion. Without it the
	// register keeps a Variant of type INT while the compiler believes it is a
	// FLOAT, and the backend then reads the payload as a double.
	if (declared == Variant::FLOAT && actual == Variant::INT) {
		int converted = alloc_register(func);
		IRInstruction convert(IROpcode::CONVERT, IRValue::reg(converted), IRValue::reg(reg));
		convert.type_hint = Variant::FLOAT;
		func.ir.instructions.push_back(convert);
		set_register_type(func, converted, Variant::FLOAT);
		free_register(func, reg);
		return converted;
	}

	// Narrowing float to int, or any other mismatch between two types that are
	// both known, is an error in GDScript rather than a silent reinterpretation.
	error_at("Cannot assign a value of type " + std::string(variant_type_name(actual)) +
		" to " + what + " of type " + std::string(variant_type_name(declared)), line, column);
}

void CodeGenerator::coerce_folded_initializer(IRGlobalVar& global, int line, int column) const {
	if (global.type_hint == IRInstruction::TypeHint_NONE ||
	    global.init_type == IRGlobalVar::InitType::NULL_VAL) {
		return;
	}

	// `var f: float = 0` declares a float, so the constant is folded to 0.0
	// rather than stored as the integer 0 under a FLOAT label.
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
		// No type hint and no initializer is rejected before this point.
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

	// Packed arrays have a construction opcode but no compile-time
	// representation, so an empty one is built by the init function.
	const IROpcode make_op = packed_array_opcode(global.type_hint);
	if (make_op != IROpcode::LABEL) {
		int reg = alloc_register(init_func);
		IRInstruction make(make_op);
		make.operands.push_back(IRValue::reg(reg));
		make.operands.push_back(IRValue::imm(0)); // no elements
		make.type_hint = global.type_hint;
		init_func.ir.instructions.push_back(make);
		init_func.ir.instructions.emplace_back(IROpcode::STORE_GLOBAL,
			IRValue::imm(static_cast<int64_t>(global_index)), IRValue::reg(reg));
		free_register(init_func, reg);
		global.init_type = IRGlobalVar::InitType::RUNTIME;
		has_global_init = true;
		return;
	}

	// Everything else (Object, Callable, Transform3D, ...) has no default the
	// guest can construct, so it stays NIL until it is assigned.
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

	// Unary operators over a constant. '-5' parses as a unary minus applied to a
	// literal, not as a negative literal, so without this a negated constant is
	// not a constant.
	if (auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
		IRGlobalVar inner;
		if (!fold_global_initializer(unary->operand.get(), inner)) {
			return false;
		}
		switch (unary->op) {
			case UnaryExpr::Op::NEG:
				if (inner.init_type == InitType::INT) {
					// Negating INT64_MIN overflows a signed negate, so it is done
					// in unsigned arithmetic and reinterpreted.
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

	// A reference to a global const that already folded to a constant.
	if (auto* var = dynamic_cast<const VariableExpr*>(expr)) {
		auto it = m_global_const_values.find(var->name);
		if (it == m_global_const_values.end()) {
			return false;
		}
		out.init_type = it->second.init_type;
		out.init_value = it->second.init_value;
		return true;
	}

	// Empty containers need no elements, so they are written directly by the
	// backend rather than through the init function.
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

IRInstruction::TypeHint CodeGenerator::derive_global_value_type(const IRGlobalVar& global) {
	// An explicit type hint always wins: it is what the declaration promised.
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
			// RUNTIME globals carry the type of the value the init function
			// produced, which the caller fills in; an untyped one is just a
			// Variant of any type.
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
	// Generate a CALL_SYSCALL instruction to get the global class object
	// ECALL_GET_OBJ (504) takes: a0 = result pointer, a1 = class name pointer, a2 = class name length
	// Returns: a0 contains the object data

	int result_reg = alloc_register(func);

	// Add the class name as a string constant
	int str_idx = add_string_constant(class_name);

	// Generate CALL_SYSCALL instruction
	// Format: CALL_SYSCALL result_reg, syscall_number, string_index, string_length
	IRInstruction instr(IROpcode::CALL_SYSCALL);
	instr.operands.push_back(IRValue::reg(result_reg));              // result register
	instr.operands.push_back(IRValue::imm(504));                     // ECALL_GET_OBJ
	instr.operands.push_back(IRValue::imm(str_idx));                 // string constant index
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(class_name.length()))); // string length

	func.ir.instructions.push_back(instr);

	// The result is an OBJECT Variant
	set_register_type(func, result_reg, IRInstruction::TypeHint_NONE); // Objects don't have a specific primitive type

	return result_reg;
}

} // namespace gdscript
