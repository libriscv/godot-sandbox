#pragma once
#include "ast.h"
#include "ir.h"
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace gdscript {

class CodeGenerator {
public:
	CodeGenerator();

	IRProgram generate(const Program& program);

private:
	IRFunction generate_function(const FunctionDecl& func);

	// Statement code generation
	void gen_stmt(const Stmt* stmt, IRFunction& func);
	void gen_var_decl(const VarDeclStmt* stmt, IRFunction& func);
	void gen_assign(const AssignStmt* stmt, IRFunction& func);
	void gen_return(const ReturnStmt* stmt, IRFunction& func);
	void gen_if(const IfStmt* stmt, IRFunction& func);
	void gen_while(const WhileStmt* stmt, IRFunction& func);
	void gen_for(const ForStmt* stmt, IRFunction& func);
	void gen_break(const BreakStmt* stmt, IRFunction& func);
	void gen_continue(const ContinueStmt* stmt, IRFunction& func);
	void gen_expr_stmt(const ExprStmt* stmt, IRFunction& func);

	// Expression code generation (returns register containing result)
	int gen_expr(const Expr* expr, IRFunction& func);
	int gen_literal(const LiteralExpr* expr, IRFunction& func);
	int gen_variable(const VariableExpr* expr, IRFunction& func);
	int gen_binary(const BinaryExpr* expr, IRFunction& func);
	int gen_unary(const UnaryExpr* expr, IRFunction& func);
	int gen_call(const CallExpr* expr, IRFunction& func);
	int gen_member_call(const MemberCallExpr* expr, IRFunction& func);
	int gen_index(const IndexExpr* expr, IRFunction& func);
	int gen_array_literal(const ArrayLiteralExpr* expr, IRFunction& func);

	// Utilities
	int alloc_register();
	void free_register(int reg);
	std::string make_label(const std::string& prefix);
	int add_string_constant(const std::string& str);

	// Variable management
	struct Variable {
		std::string name;
		int register_num; // Current register holding the value, or -1 if spilled
		IRInstruction::TypeHint type_hint = IRInstruction::TypeHint_NONE;
		bool is_const = false; // Whether this is a const variable
	};

	// Scope stack for nested blocks
	struct Scope {
		std::unordered_map<std::string, Variable> variables;
		size_t parent_scope_idx; // Index into m_scope_stack, SIZE_MAX for root
	};

	std::vector<Scope> m_scope_stack;
	int m_next_register = 0;
	int m_next_label = 0;
	std::vector<std::string> m_string_constants;

	// Type tracking for registers
	std::unordered_map<int, IRInstruction::TypeHint> m_register_types;

	// Scope management
	void push_scope();
	void pop_scope();
	Variable* find_variable(const std::string& name);
	void declare_variable(const std::string& name, int register_num, bool is_const = false);

	// Loop context for break/continue
	struct LoopContext {
		std::string break_label;
		std::string continue_label;
	};
	std::vector<LoopContext> m_loop_stack;

	// Type tracking helpers
	void set_register_type(int reg, IRInstruction::TypeHint type);
	IRInstruction::TypeHint get_register_type(int reg) const;
	bool is_inline_primitive_constructor(const std::string& name) const;
	bool is_inline_member_access(IRInstruction::TypeHint type, const std::string& member) const;
	int gen_inline_constructor(const std::string& name, const std::vector<int>& arg_regs, IRFunction& func);
	int gen_inline_member_get(int obj_reg, IRInstruction::TypeHint obj_type, const std::string& member, IRFunction& func);

	// Global class detection
	bool is_global_class(const std::string& name) const;
	int gen_global_class_get(const std::string& class_name, IRFunction& func);

	// Set of global class names
	static std::unordered_set<std::string> get_global_classes();
};

} // namespace gdscript
