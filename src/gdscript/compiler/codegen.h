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
	void gen_match(const MatchStmt* stmt, IRFunction& func);
	void gen_while(const WhileStmt* stmt, IRFunction& func);
	void gen_for(const ForStmt* stmt, IRFunction& func);
	void gen_break(const BreakStmt* stmt, IRFunction& func);
	void gen_continue(const ContinueStmt* stmt, IRFunction& func);
	void gen_expr_stmt(const ExprStmt* stmt, IRFunction& func);
	void emit_conditional_branch(IROpcode opcode, int cond_reg, const std::string& label, IRFunction& func);

	// Expression code generation (returns register containing result)
	int gen_expr(const Expr* expr, IRFunction& func);
	int gen_literal(const LiteralExpr* expr, IRFunction& func);
	int gen_variable(const VariableExpr* expr, IRFunction& func);
	int gen_binary(const BinaryExpr* expr, IRFunction& func);
	int gen_logical(const BinaryExpr* expr, IRFunction& func); // short-circuiting and/or
	int gen_unary(const UnaryExpr* expr, IRFunction& func);
	int gen_ternary(const TernaryExpr* expr, IRFunction& func);
	int gen_call(const CallExpr* expr, IRFunction& func);
	int gen_member_call(const MemberCallExpr* expr, IRFunction& func);
	int gen_index(const IndexExpr* expr, IRFunction& func);
	int gen_array_literal(const ArrayLiteralExpr* expr, IRFunction& func);
	int gen_dictionary_literal(const DictionaryLiteralExpr* expr, IRFunction& func);

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

	// Local function detection
	bool is_local_function(const std::string& name) const;

	// Set of global class names
	static std::unordered_set<std::string> get_global_classes();

	// Track locally defined functions
	std::unordered_set<std::string> m_local_functions;

	// Signatures of locally defined functions, for default-argument filling.
	// The Program outlives generate(), so borrowing pointers here is safe.
	std::unordered_map<std::string, const FunctionDecl*> m_local_signatures;

	// Global variables
	std::unordered_map<std::string, size_t> m_global_variables; // Maps global name to index
	std::unordered_set<std::string> m_global_consts; // Names of globals declared const
	bool is_global_variable(const std::string& name) const;
	bool is_global_const(const std::string& name) const;

	// Values of global consts that folded to a compile-time constant, so that a
	// later initializer can refer to them by name.
	std::unordered_map<std::string, IRGlobalVar> m_global_const_values;

	// Number of globals whose initializer has already been lowered. An
	// initializer referring to a global at or beyond this index is a forward
	// reference and would read NIL, so it is rejected instead.
	size_t m_globals_lowered = 0;

	// Fold a global initializer to a compile-time constant. Returns false when
	// the expression has to be evaluated at startup instead.
	bool fold_global_initializer(const Expr* expr, IRGlobalVar& out) const;

	// Variant type a global holds, when known at compile time.
	static IRInstruction::TypeHint derive_global_value_type(const IRGlobalVar& global);

	// Give a type-hinted global without an initializer the default value of its
	// declared type, the way GDScript does.
	void apply_default_initializer(IRGlobalVar& global, IRFunction& init_func,
		size_t global_index, bool& has_global_init);

	// Construction opcode for a packed array type, or IROpcode::LABEL when the
	// type is not a packed array.
	static IROpcode packed_array_opcode(IRInstruction::TypeHint type);

	// Make the value in `reg` match a declared type: performs GDScript's implicit
	// int -> float conversion and rejects mismatches that GDScript rejects.
	// Returns the register holding the coerced value.
	int coerce_to_declared_type(int reg, IRInstruction::TypeHint declared,
		IRFunction& func, const std::string& what);

	// Make a folded constant initializer match the global's declared type, or
	// reject it when it cannot.
	void coerce_folded_initializer(IRGlobalVar& global) const;

	// Declared type of each global by index, for coercing stores.
	std::vector<IRInstruction::TypeHint> m_global_types;
};

} // namespace gdscript
