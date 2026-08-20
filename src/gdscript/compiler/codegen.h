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
	// -= Per-function state =-
	//
	// Virtual register numbers restart at 0 in every function, so anything keyed
	// by register number means nothing once the function ends. Keeping that
	// state on the CodeGenerator meant clearing each piece of it by hand at the
	// top of generate_function(), and the one that was missed -- the register
	// type map -- let a function inherit the previous one's types and put the
	// backend on a native integer path for a Variant that was not an integer.
	//
	// It is a value type held on the stack for exactly as long as one function
	// is being lowered, so "forgot to reset it" is not a mistake that can be
	// made: adding a field here gives it a fresh value automatically. Only
	// state that genuinely spans the program -- the string constants, the
	// global variables, the label counter -- stays on the object.

	// A variable in scope, and the register currently holding it.
	struct Variable {
		std::string name;
		int register_num; // Current register holding the value, or -1 if spilled
		IRInstruction::TypeHint type_hint = IRInstruction::TypeHint_NONE;
		bool is_const = false; // Whether this is a const variable
	};

	// Scope stack entry for nested blocks
	struct Scope {
		std::unordered_map<std::string, Variable> variables;
		size_t parent_scope_idx; // Index into the scope stack, SIZE_MAX for root
	};

	// Where 'break' and 'continue' go, innermost loop last.
	struct LoopContext {
		std::string break_label;
		std::string continue_label;
	};

	struct FunctionContext {
		// The IR being built.
		IRFunction ir;
		// Variables in scope, innermost last.
		std::vector<Scope> scopes;
		// The Variant type each virtual register is known to hold.
		std::unordered_map<int, IRInstruction::TypeHint> register_types;
		// Enclosing loops, innermost last.
		std::vector<LoopContext> loops;
		// Next virtual register to hand out.
		int next_register = 0;
	};

	IRFunction generate_function(const FunctionDecl& func);

	// Statement code generation
	void gen_stmt(const Stmt* stmt, FunctionContext& func);
	void gen_var_decl(const VarDeclStmt* stmt, FunctionContext& func);
	void gen_assign(const AssignStmt* stmt, FunctionContext& func);
	void gen_return(const ReturnStmt* stmt, FunctionContext& func);
	void gen_if(const IfStmt* stmt, FunctionContext& func);
	void gen_match(const MatchStmt* stmt, FunctionContext& func);
	void gen_while(const WhileStmt* stmt, FunctionContext& func);
	void gen_for(const ForStmt* stmt, FunctionContext& func);
	void gen_break(const BreakStmt* stmt, FunctionContext& func);
	void gen_continue(const ContinueStmt* stmt, FunctionContext& func);
	void gen_expr_stmt(const ExprStmt* stmt, FunctionContext& func);
	void emit_conditional_branch(IROpcode opcode, int cond_reg, const std::string& label, FunctionContext& func);

	// Expression code generation (returns register containing result)
	int gen_expr(const Expr* expr, FunctionContext& func);
	int gen_literal(const LiteralExpr* expr, FunctionContext& func);
	int gen_variable(const VariableExpr* expr, FunctionContext& func);
	int gen_binary(const BinaryExpr* expr, FunctionContext& func);
	int gen_logical(const BinaryExpr* expr, FunctionContext& func); // short-circuiting and/or
	int gen_unary(const UnaryExpr* expr, FunctionContext& func);
	int gen_ternary(const TernaryExpr* expr, FunctionContext& func);
	int gen_call(const CallExpr* expr, FunctionContext& func);
	int gen_member_call(const MemberCallExpr* expr, FunctionContext& func);
	int gen_index(const IndexExpr* expr, FunctionContext& func);
	int gen_array_literal(const ArrayLiteralExpr* expr, FunctionContext& func);
	int gen_dictionary_literal(const DictionaryLiteralExpr* expr, FunctionContext& func);

	// -= Diagnostics =-
	//
	// Every error names the source position it came from. An error that says
	// what is wrong but not where leaves the user to find the line themselves,
	// which for a generated diagnostic like a type mismatch on a global is most
	// of the work.
	[[noreturn]] void error_at(const std::string& message, int line, int column,
		const std::string& hint = "") const;
	[[noreturn]] void error_at(const std::string& message, const Expr* expr,
		const std::string& hint = "") const;
	[[noreturn]] void error_at(const std::string& message, const Stmt* stmt,
		const std::string& hint = "") const;

	// The function being generated, for the "in function:" line of a diagnostic.
	std::string m_current_function;

	// Utilities
	int alloc_register(FunctionContext& func);
	void free_register(FunctionContext& func, int reg);
	std::string make_label(const std::string& prefix);
	int add_string_constant(const std::string& str);

	// Program-wide state: labels are unique across the whole program, and the
	// string constant pool is shared by every function.
	int m_next_label = 0;
	std::vector<std::string> m_string_constants;

	// Scope management
	void push_scope(FunctionContext& func);
	void pop_scope(FunctionContext& func);
	Variable* find_variable(FunctionContext& func, const std::string& name);
	// `site` is the statement the declaration comes from, so that a redeclaration
	// can be reported at the line it happens on.
	void declare_variable(FunctionContext& func, const std::string& name, int register_num,
		bool is_const = false, const Stmt* site = nullptr);

	// Type tracking helpers
	void set_register_type(FunctionContext& func, int reg, IRInstruction::TypeHint type);
	IRInstruction::TypeHint get_register_type(const FunctionContext& func, int reg) const;
	bool is_inline_primitive_constructor(const std::string& name) const;
	bool is_inline_member_access(IRInstruction::TypeHint type, const std::string& member) const;
	int gen_inline_constructor(const std::string& name, const std::vector<int>& arg_regs,
		FunctionContext& func, const Expr* site);
	int gen_inline_member_get(int obj_reg, IRInstruction::TypeHint obj_type, const std::string& member, FunctionContext& func);

	// Global class detection
	bool is_global_class(const std::string& name) const;
	int gen_global_class_get(const std::string& class_name, FunctionContext& func);

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
	void apply_default_initializer(IRGlobalVar& global, FunctionContext& init_func,
		size_t global_index, bool& has_global_init);

	// Construction opcode for a packed array type, or IROpcode::LABEL when the
	// type is not a packed array.
	static IROpcode packed_array_opcode(IRInstruction::TypeHint type);

	// Make the value in `reg` match a declared type: performs GDScript's implicit
	// int -> float conversion and rejects mismatches that GDScript rejects.
	// Returns the register holding the coerced value.
	int coerce_to_declared_type(int reg, IRInstruction::TypeHint declared,
		FunctionContext& func, const std::string& what, const Stmt* site);

	// Make a folded constant initializer match the global's declared type, or
	// reject it when it cannot.
	void coerce_folded_initializer(IRGlobalVar& global, int line, int column) const;

	// Declared type of each global by index, for coercing stores.
	std::vector<IRInstruction::TypeHint> m_global_types;
};

} // namespace gdscript
