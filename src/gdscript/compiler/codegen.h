#pragma once
#include "ast.h"
#include "globals.h"
#include "ir.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace gdscript {

class CodeGenerator {
public:
	CodeGenerator();

	IRProgram generate(const Program& program);

private:
	// Per-function state. Value type: lives on the stack for one function's
	// lowering, so new fields are automatically fresh. Program-wide state
	// (string constants, globals, label counter) stays on CodeGenerator.

	struct Variable {
		std::string name;
		int register_num;
		IRInstruction::TypeHint type_hint = IRInstruction::TypeHint_NONE;
		bool is_const = false;
	};

	struct Scope {
		std::unordered_map<std::string, Variable> variables;
		size_t parent_scope_idx;
	};

	struct LoopContext {
		std::string break_label;
		std::string continue_label;
	};

	struct FunctionContext {
		IRFunction ir;
		std::vector<Scope> scopes;
		std::unordered_map<int, IRInstruction::TypeHint> register_types;
		// Struct known for a register: always DICTIONARY-typed, used for field-name checks.
		std::unordered_map<int, const StructDecl*> register_structs;
		std::vector<LoopContext> loops;
		std::string return_type;
		int next_register = 0;
	};

	IRFunction generate_function(const FunctionDecl& func);

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

	int gen_expr(const Expr* expr, FunctionContext& func);
	int gen_literal(const LiteralExpr* expr, FunctionContext& func);
	int gen_variable(const VariableExpr* expr, FunctionContext& func);
	int gen_binary(const BinaryExpr* expr, FunctionContext& func);
	int gen_logical(const BinaryExpr* expr, FunctionContext& func);
	int gen_unary(const UnaryExpr* expr, FunctionContext& func);
	int gen_type_test(const TypeTestExpr* expr, FunctionContext& func);
	// Replace Dictionary with its keys Array for position-based iteration.
	void gen_dictionary_keys_for_iteration(int iterable_reg, FunctionContext& func);


	int gen_assert(const CallExpr* expr, FunctionContext& func);
	int gen_get_node(const std::string& path, FunctionContext& func);
	int gen_range(const CallExpr* expr, FunctionContext& func);
	int gen_color8(const CallExpr* expr, FunctionContext& func);
	int gen_class_test(int value_reg, const std::string& class_name, FunctionContext& func);
	int gen_class_cast(const ClassCastExpr* expr, FunctionContext& func);
	int gen_int_immediate(int64_t value, FunctionContext& func);
	void gen_numeric_for(const ForStmt* stmt, int start_reg, int end_reg, int step_reg,
		FunctionContext& func);
	int gen_float_immediate(double value, FunctionContext& func);
	// Returns -1 if `type` has no constant `name`.
	int gen_builtin_constant(const std::string& type, const std::string& name, FunctionContext& func);
	int gen_ternary(const TernaryExpr* expr, FunctionContext& func);
	int gen_call(const CallExpr* expr, FunctionContext& func);
	int gen_member_call(const MemberCallExpr* expr, FunctionContext& func);
	int gen_index(const IndexExpr* expr, FunctionContext& func);
	int gen_array_literal(const ArrayLiteralExpr* expr, FunctionContext& func);
	int gen_dictionary_literal(const DictionaryLiteralExpr* expr, FunctionContext& func);

	// Every diagnostic names its source position.
	[[noreturn]] void error_at(const std::string& message, int line, int column,
		const std::string& hint = "") const;
	[[noreturn]] void error_at(const std::string& message, const Expr* expr,
		const std::string& hint = "") const;
	[[noreturn]] void error_at(const std::string& message, const Stmt* stmt,
		const std::string& hint = "") const;

	std::string m_current_function;

	int alloc_register(FunctionContext& func);
	void free_register(FunctionContext& func, int reg);
	std::string make_label(const std::string& prefix);
	int add_string_constant(const std::string& str);

	// Program-wide: labels unique across all functions, string pool shared.
	int m_next_label = 0;
	std::vector<std::string> m_string_constants;

	void push_scope(FunctionContext& func);
	void pop_scope(FunctionContext& func);
	Variable* find_variable(FunctionContext& func, const std::string& name);
	void declare_variable(FunctionContext& func, const std::string& name, int register_num,
		bool is_const = false, const Stmt* site = nullptr);

	void set_register_type(FunctionContext& func, int reg, IRInstruction::TypeHint type);
	IRInstruction::TypeHint get_register_type(const FunctionContext& func, int reg) const;
	bool is_inline_primitive_constructor(const std::string& name) const;
	bool is_inline_member_access(IRInstruction::TypeHint type, const std::string& member) const;
	int gen_inline_constructor(const std::string& name, const std::vector<int>& arg_regs,
		FunctionContext& func, const Expr* site);
	int gen_inline_member_get(int obj_reg, IRInstruction::TypeHint obj_type, const std::string& member, FunctionContext& func);
	void gen_inline_member_set(int obj_reg, IRInstruction::TypeHint obj_type, const std::string& member,
		int value_reg, FunctionContext& func);

	// Types with `member` inline; non-empty means VGET/VSET (Object-only) is wrong.
	std::vector<IRInstruction::TypeHint> inline_member_types(const std::string& member) const;
	// Has own size()/get(); ECALL_ARRAY_SIZE/AT throw on these.
	static bool is_packed_array_type(IRInstruction::TypeHint type);
	int gen_vget(int obj_reg, const std::string& member, FunctionContext& func);
	void gen_vset(int obj_reg, const std::string& member, int value_reg, FunctionContext& func);
	// Untyped: branch on tag at run time.
	int gen_dynamic_member_get(int obj_reg, const std::string& member, FunctionContext& func);
	void gen_dynamic_member_set(int obj_reg, const std::string& member, int value_reg, FunctionContext& func);

	// Assignment target with write-back chain. Value types (Vector2/3, Color, ...)
	// need the mutated copy carried back; handles (Array, Dictionary, Object) do not.
	struct LValue {
		enum class Kind { LOCAL, GLOBAL, MEMBER, INDEX, VALUE };
		Kind kind = Kind::VALUE;
		int reg = -1;
		std::shared_ptr<LValue> container; // MEMBER/INDEX: source of `reg`
		std::string name;                  // LOCAL/GLOBAL/MEMBER
		int index_reg = -1;                // INDEX
		bool borrowed = false;             // LOCAL: `reg` owns the variable, skip free
	};

	LValue resolve_lvalue(const Expr* expr, FunctionContext& func);
	void store_lvalue(const LValue& target, int value_reg, FunctionContext& func, const Stmt* site);
	void free_lvalue(const LValue& lvalue, FunctionContext& func);
	void gen_store_to(const Expr* target, int value_reg, FunctionContext& func, const Stmt* site);
	void gen_store_to_variable(const std::string& name, int value_reg, FunctionContext& func, const Stmt* site);
	int gen_member_read(int obj_reg, const std::string& member, FunctionContext& func,
		const Expr* site = nullptr);
	int gen_element_read(int obj_reg, int idx_reg, FunctionContext& func,
		const Expr* site = nullptr);
	// Returns true when the store mutated a value-type copy (caller must write back).
	bool gen_member_store(int obj_reg, const std::string& member, int value_reg, FunctionContext& func);
	void gen_element_store(int obj_reg, int idx_reg, int value_reg, FunctionContext& func);

	// Global functions: must not fall through to self-call (Godot drops the VCALL).
	bool is_global_function(const std::string& name) const;
	int gen_global_function(const CallExpr* expr, std::vector<int>& arg_regs, FunctionContext& func);

	// Emit GLOBAL_CALL; resolves NUMERIC to int/float form when types are known.
	int gen_global_call(const GlobalFunction& info, const std::vector<int>& arg_regs,
		FunctionContext& func, const Expr* site);

	// Structs: Dictionary with a fixed key set, field-checked at compile time.

	const StructDecl* find_struct(const std::string& name) const;

	const StructField& require_struct_field(const StructDecl& decl, const std::string& field_name,
		int line, int column) const;

	int gen_struct_construct(const StructDecl& decl, const std::vector<ExprPtr>& arguments,
		const NamedArguments& names, FunctionContext& func, const Expr* site);

	int gen_field_default(const StructDecl& decl, const StructField& field, FunctionContext& func);
	// Returns -1 for types with no guest-constructible default.
	int gen_default_value(const std::string& type_hint, FunctionContext& func);

	int gen_dict_get(int obj_reg, const std::string& key, FunctionContext& func);
	void gen_dict_set(int obj_reg, const std::string& key, int value_reg, FunctionContext& func);

	void apply_declared_type(int reg, const std::string& type_hint, FunctionContext& func);
	FunctionSignature build_signature(const FunctionDecl& decl) const;

	void set_register_struct(FunctionContext& func, int reg, const StructDecl* decl);
	const StructDecl* get_register_struct(const FunctionContext& func, int reg) const;

	void reject_named_arguments(const NamedArguments& names, const std::string& what,
		const Expr* site) const;

	std::unordered_map<std::string, const StructDecl*> m_structs;
	std::unordered_map<std::string, const EnumDecl*> m_enums;
	std::unordered_map<std::string, int64_t> m_enum_members;

	// Recursion guard for struct defaults that contain themselves.
	std::vector<const StructDecl*> m_struct_default_stack;


	bool is_global_class(const std::string& name) const;
	int gen_global_class_get(const std::string& class_name, FunctionContext& func);

	bool is_local_function(const std::string& name) const;
	static std::unordered_set<std::string> get_global_classes();
	std::unordered_set<std::string> m_local_functions;
	std::unordered_map<std::string, const FunctionDecl*> m_local_signatures;
	std::unordered_map<std::string, size_t> m_global_variables;
	std::unordered_set<std::string> m_global_consts;
	bool is_global_variable(const std::string& name) const;
	bool is_global_const(const std::string& name) const;

	std::unordered_map<std::string, IRGlobalVar> m_global_const_values;
	// Forward references read NIL; rejected at this boundary.
	size_t m_globals_lowered = 0;

	bool fold_global_initializer(const Expr* expr, IRGlobalVar& out) const;

	// SWITCH for dense integer-constant match arms; returns false if not dense enough.
	bool gen_match_jump_table(const MatchStmt* stmt, int subject_reg,
	                          const std::vector<std::string>& body_labels,
	                          const std::string& default_label, FunctionContext& func);

	// Match patterns: fall through on match, jump to fail_label otherwise.
	void gen_branch_test(const MatchStmt::Branch& branch, int subject_reg,
	                     const std::string& fail_label, FunctionContext& func);
	void gen_pattern_test(const MatchPattern& pattern, int subject_reg,
	                      const std::string& fail_label, FunctionContext& func);
	void gen_array_pattern_test(const MatchPattern& pattern, int subject_reg,
	                            const std::string& fail_label, FunctionContext& func);
	void gen_dictionary_pattern_test(const MatchPattern& pattern, int subject_reg,
	                                 const std::string& fail_label, FunctionContext& func);

	// Branch to fail_label on type mismatch; returns false if statically impossible.
	bool emit_type_guard(int value_reg, IRInstruction::TypeHint type,
	                     const std::string& fail_label, FunctionContext& func);
	bool is_array_element_access(int obj_reg, int idx_reg, FunctionContext& func);
	int gen_array_size(int array_reg, FunctionContext& func);
	int gen_array_element(int array_reg, int index_reg, FunctionContext& func);
	// key_reg -1 for keyless operations.
	int gen_dictionary_op(int64_t op, int dict_reg, int key_reg,
	                      IRInstruction::TypeHint result_type, FunctionContext& func);
	int gen_compare(IROpcode opcode, int left_reg, int right_reg, FunctionContext& func);
	// TypeHint_NONE forces Variant::evaluate() fallback.
	static IRInstruction::TypeHint fused_compare_type(IRInstruction::TypeHint left,
	                                                  IRInstruction::TypeHint right);

	// Returns -1 for non-const or container globals (those stay on LOAD_GLOBAL).
	int gen_const_global_value(const std::string& name, FunctionContext& func);

	static IRInstruction::TypeHint derive_global_value_type(const IRGlobalVar& global);

	void apply_default_initializer(IRGlobalVar& global, FunctionContext& init_func,
		size_t global_index, bool& has_global_init);

	static IROpcode packed_array_opcode(IRInstruction::TypeHint type);
	// Implicit int->float widening; rejects type mismatches.
	int coerce_to_declared_type(int reg, IRInstruction::TypeHint declared,
		FunctionContext& func, const std::string& what, const Stmt* site);
	int coerce_to_declared_type(int reg, IRInstruction::TypeHint declared,
		FunctionContext& func, const std::string& what, int line, int column);

	void coerce_folded_initializer(IRGlobalVar& global, int line, int column) const;
	std::vector<IRInstruction::TypeHint> m_global_types;
	// Struct per global, for field-name checking on load.
	std::vector<const StructDecl*> m_global_structs;
};

} // namespace gdscript
