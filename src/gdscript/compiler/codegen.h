#pragma once
#include "ast.h"
#include "ir.h"
#include <unordered_map>
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

	// Utilities
	int alloc_register();
	void free_register(int reg);
	std::string make_label(const std::string& prefix);
	int add_string_constant(const std::string& str);

	// Variable management
	struct Variable {
		std::string name;
		int register_num; // Current register holding the value, or -1 if spilled
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

	// Scope management
	void push_scope();
	void pop_scope();
	Variable* find_variable(const std::string& name);
	void declare_variable(const std::string& name, int register_num);

	// Loop context for break/continue
	struct LoopContext {
		std::string break_label;
		std::string continue_label;
	};
	std::vector<LoopContext> m_loop_stack;
};

} // namespace gdscript
