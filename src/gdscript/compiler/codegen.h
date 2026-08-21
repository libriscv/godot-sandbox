#pragma once
#include "ast.h"
#include "globals.h"
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
		// The struct each virtual register is known to hold an instance of.
		// A register in here always has the DICTIONARY type as well: the struct
		// is what the compiler knows on top of that, and the only thing it is
		// used for is checking field names and typing what a field read yields.
		std::unordered_map<int, const StructDecl*> register_structs;
		// Enclosing loops, innermost last.
		std::vector<LoopContext> loops;
		// Declared return type, coerced to by gen_return() so that a caller may
		// trust the same type read off the signature.
		std::string return_type;
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
	int gen_type_test(const TypeTestExpr* expr, FunctionContext& func);
	// Replace a Dictionary in `iterable_reg` with the Array of its keys, so a for
	// loop can walk it by position. Emits nothing for a known non-Dictionary.
	void gen_dictionary_keys_for_iteration(int iterable_reg, FunctionContext& func);

	// A compile-time integer in a register typed INT.
	int gen_int_immediate(int64_t value, FunctionContext& func);
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

	// GDScript global functions. These are not methods on the owner node, so
	// they must not fall through to the self-call path in gen_call(), which
	// would produce a VCALL that Godot silently drops.
	// Which globals exist, and what each one lowers to, is globals.h's table.
	bool is_global_function(const std::string& name) const;
	int gen_global_function(const CallExpr* expr, std::vector<int>& arg_regs, FunctionContext& func);

	// One GLOBAL_CALL for `info`, over the registers in `arg_regs`. Resolves a
	// NUMERIC entry to its integer or floating-point form when the argument
	// types say which, and leaves it to the backend when they do not.
	int gen_global_call(const GlobalFunction& info, const std::vector<int>& arg_regs,
		FunctionContext& func, const Expr* site);

	// -= Structs =-
	//
	// A struct is a Dictionary with a fixed set of keys. `BankAccount.new()`
	// builds that Dictionary from the declared defaults, and a field access on
	// a value the compiler knows to be a BankAccount becomes a Dictionary get
	// or set, checked against the declared field names.

	// The struct of that name, or null when there is none.
	const StructDecl* find_struct(const std::string& name) const;

	// The named field of a struct. A name the struct does not declare is the
	// mistake structs exist to catch, so it is an error rather than a new key.
	const StructField& require_struct_field(const StructDecl& decl, const std::string& field_name,
		int line, int column) const;

	// Build an instance: a Dictionary holding every declared field, taking each
	// value from the call site when it supplies one and from the declaration
	// otherwise.
	int gen_struct_construct(const StructDecl& decl, const std::vector<ExprPtr>& arguments,
		const NamedArguments& names, FunctionContext& func, const Expr* site);

	// The value a field takes when an instance does not supply one.
	int gen_field_default(const StructDecl& decl, const StructField& field, FunctionContext& func);

	// The default value of a declared type, the way GDScript gives one to a
	// typed variable with no initializer. Returns -1 for a type that has no
	// default the guest can construct.
	int gen_default_value(const std::string& type_hint, FunctionContext& func);

	// Dictionary element access, which is what a struct field access lowers to.
	int gen_dict_get(int obj_reg, const std::string& key, FunctionContext& func);
	void gen_dict_set(int obj_reg, const std::string& key, int value_reg, FunctionContext& func);

	// Give a register the type, and the struct, that a declared type names.
	void apply_declared_type(int reg, const std::string& type_hint, FunctionContext& func);

	// The signature the host sees for one function: parameter names, declared
	// types, and the defaults that folded to constants. Built after the globals
	// are lowered, so a default naming a global const folds too.
	FunctionSignature build_signature(const FunctionDecl& decl) const;

	void set_register_struct(FunctionContext& func, int reg, const StructDecl* decl);
	const StructDecl* get_register_struct(const FunctionContext& func, int reg) const;

	// Named arguments are a struct-constructor feature. Anything else that
	// reaches a call with them has to say so rather than drop the names.
	void reject_named_arguments(const NamedArguments& names, const std::string& what,
		const Expr* site) const;

	// Declared structs by name. The Program outlives generate(), so borrowing
	// pointers into it is safe.
	std::unordered_map<std::string, const StructDecl*> m_structs;
	// Named enums by name; members of unnamed enums, reachable unqualified.
	std::unordered_map<std::string, const EnumDecl*> m_enums;
	std::unordered_map<std::string, int64_t> m_enum_members;

	// Structs currently having their defaults built, so that a struct holding
	// itself by value is reported instead of recursing forever.
	std::vector<const StructDecl*> m_struct_default_stack;

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

	// Emit a SWITCH for a match with dense integer constant patterns. Returns
	// false if it does not qualify, leaving the compare chain to do the dispatch.
	bool gen_match_jump_table(const MatchStmt* stmt, int subject_reg,
	                          const std::vector<std::string>& body_labels,
	                          const std::string& default_label, FunctionContext& func);

	// -= Match patterns =-
	//
	// Each emits a test that falls through on a match and jumps to `fail_label`
	// otherwise. A binding pattern declares its name in the open scope, which is
	// the arm's, so the guard and the body both see it.
	void gen_branch_test(const MatchStmt::Branch& branch, int subject_reg,
	                     const std::string& fail_label, FunctionContext& func);
	void gen_pattern_test(const MatchPattern& pattern, int subject_reg,
	                      const std::string& fail_label, FunctionContext& func);
	void gen_array_pattern_test(const MatchPattern& pattern, int subject_reg,
	                            const std::string& fail_label, FunctionContext& func);
	void gen_dictionary_pattern_test(const MatchPattern& pattern, int subject_reg,
	                                 const std::string& fail_label, FunctionContext& func);

	// Jump to `fail_label` unless the value has that Variant type. Emits nothing
	// when the type is already known to match. Returns false when it is known
	// not to, after emitting the unconditional jump that is then the whole test:
	// the caller emits no destructuring, which could never run.
	bool emit_type_guard(int value_reg, IRInstruction::TypeHint type,
	                     const std::string& fail_label, FunctionContext& func);
	// Whether `a[i]` may lower to ARRAY_GET/ARRAY_SET rather than a VCALL.
	bool is_array_element_access(int obj_reg, int idx_reg, FunctionContext& func);
	// Array length, via ECALL_ARRAY_SIZE.
	int gen_array_size(int array_reg, FunctionContext& func);
	// Array element by position, via ECALL_ARRAY_AT.
	int gen_array_element(int array_reg, int index_reg, FunctionContext& func);
	// One ECALL_DICTIONARY_OPS. `key_reg` is -1 for keyless operations;
	// `result_type` is the operation's known result type.
	int gen_dictionary_op(int64_t op, int dict_reg, int key_reg,
	                      IRInstruction::TypeHint result_type, FunctionContext& func);
	// A comparison feeding a branch, carrying the operand type hint that keeps
	// the backend off Variant::evaluate().
	int gen_compare(IROpcode opcode, int left_reg, int right_reg, FunctionContext& func);

	// The type hint a comparison of these operands may carry, or TypeHint_NONE if
	// the backend must fall back to Variant::evaluate().
	static IRInstruction::TypeHint fused_compare_type(IRInstruction::TypeHint left,
	                                                  IRInstruction::TypeHint right);

	// Materialise a global `const` as the immediate it folded to, typing the
	// register accordingly. Returns -1 when `name` is not such a const, or holds
	// a container, which stays a single shared handle on LOAD_GLOBAL.
	int gen_const_global_value(const std::string& name, FunctionContext& func);

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
	// The same, for a value whose source position is not a statement: a struct
	// field's declaration, or one argument of a constructor call.
	int coerce_to_declared_type(int reg, IRInstruction::TypeHint declared,
		FunctionContext& func, const std::string& what, int line, int column);

	// Make a folded constant initializer match the global's declared type, or
	// reject it when it cannot.
	void coerce_folded_initializer(IRGlobalVar& global, int line, int column) const;

	// Declared type of each global by index, for coercing stores.
	std::vector<IRInstruction::TypeHint> m_global_types;

	// Struct each global was declared as, by index, or null. Read back when a
	// global is loaded, so that a field access on it is checked the same way a
	// local is.
	std::vector<const StructDecl*> m_global_structs;
};

} // namespace gdscript
