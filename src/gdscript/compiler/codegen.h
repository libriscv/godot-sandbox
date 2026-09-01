#pragma once
#include "ast.h"
#include "builtin_members.h"
#include "builtin_methods.h"
#include "globals.h"
#include "ir.h"
#include "type_set.h"
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

	void set_restricted(bool restricted) { m_restricted = restricted; }
	void set_struct_checks(bool enabled, bool deep = false) {
		m_struct_checks = enabled;
		m_struct_deep_checks = enabled && deep;
	}
	void set_trait_structural_fallback(bool enabled) { m_trait_structural_fallback = enabled; }
	void set_engine_ancestry(const std::vector<std::pair<std::string, std::string>>& pairs);
	void set_source_path(std::string path) { m_source_path = std::move(path); }
	void set_autoloads(const std::vector<std::string>& autoloads) {
		m_autoloads.clear();
		m_autoloads.insert(autoloads.begin(), autoloads.end());
	}
	void set_global_script_classes(const std::vector<std::pair<std::string, std::string>>& classes) {
		m_global_script_classes.clear();
		m_global_script_classes.insert(classes.begin(), classes.end());
	}

private:
	// Per-function state. Value type: lives on the stack for one function's
	// lowering, so new fields are automatically fresh. Program-wide state
	// (string constants, globals, label counter) stays on CodeGenerator.

	struct Variable {
		std::string name;
		int register_num;
		IRInstruction::TypeHint type_hint = IRInstruction::TypeHint_NONE;
		bool is_const = false;
		bool is_variant = false;
		size_t debug_index = SIZE_MAX;
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
		std::unordered_map<int, TypeSet> declared_sets;
		// A safe `is` branch may re-read a union member with one known tag.
		std::unordered_map<size_t, IRInstruction::TypeHint> narrowed_global_types;
		// Nullable struct slots keep their shape separately from the current tag.
		std::unordered_map<int, const StructDecl*> declared_structs;
		// Untyped locals inferred from an initializer may legally change Variant
		// type later; mixed numeric lowering must not bake their current tag into a
		// loop-carried CONVERT.
		std::unordered_set<int> reclassifiable_registers;
		// Struct known for a register: always DICTIONARY-typed, used for field-name checks.
		std::unordered_map<int, const StructDecl*> register_structs;
		std::unordered_map<int, std::unordered_set<const TraitDecl*>> register_traits;
		std::unordered_map<int, std::unordered_set<const TraitDecl*>> declared_traits;
		std::unordered_set<int> trait_only_registers;
		// Compiler-only generic container promises.
		std::unordered_map<int, const StructDecl*> array_element_structs;
		std::unordered_map<int, const StructDecl*> dictionary_value_structs;
		std::unordered_map<int, const TraitDecl*> array_element_traits;
		std::unordered_map<int, const TraitDecl*> dictionary_value_traits;
		// A String walk produces one Unicode code point per element. Godot stores
		// Strings as UTF-32, so length()/size() on that loop value is always one.
		std::unordered_set<int> string_character_registers;
		// Subset of the above holding the raw UTF-32 value, not a one-character
		// String. Only the code-point walk produces these; only these skip Godot
		// for ord().
		std::unordered_set<int> codepoint_value_registers;
		std::vector<LoopContext> loops;
		TypeExpr return_type;
		int next_register = 0;
		int next_scope_id = 0;
		int next_array_batch_id = 0;
		int next_codepoint_batch_id = 0;
	};

	IRFunction generate_function(const FunctionDecl& func, const StructDecl* owner = nullptr);
	IRFunction generate_lambda_function(const FunctionDecl& decl,
		const std::vector<std::string>& captures);

	// Stamps IRInstruction::line over everything the dispatch emits.
	void gen_stmt(const Stmt* stmt, FunctionContext& func);
	void gen_stmt_dispatch(const Stmt* stmt, FunctionContext& func);
	void gen_var_decl(const VarDeclStmt* stmt, FunctionContext& func,
		bool conditional_binding = false);
	void gen_assign(const AssignStmt* stmt, FunctionContext& func);
	void gen_return(const ReturnStmt* stmt, FunctionContext& func);
	void gen_if(const IfStmt* stmt, FunctionContext& func);
	void gen_if_binding(const IfStmt* stmt, FunctionContext& func);
	void gen_match(const MatchStmt* stmt, FunctionContext& func);
	struct NarrowingInfo {
		int reg = -1;
		size_t global_idx = SIZE_MAX;
		TypeSet original;
		TypeSet then_set;
		TypeSet else_set;
		IRInstruction::TypeHint saved_type = IRInstruction::TypeHint_NONE;
		const StructDecl* saved_struct = nullptr;
		const TraitDecl* narrowed_trait = nullptr;
		bool trait_then = true;
		std::unordered_set<const TraitDecl*> saved_traits;
		bool saved_trait_only = false;
		bool had_saved_global = false;
		IRInstruction::TypeHint saved_global = IRInstruction::TypeHint_NONE;
		bool valid() const { return reg >= 0 || global_idx != SIZE_MAX; }
		bool is_member() const { return global_idx != SIZE_MAX; }
	};
	NarrowingInfo condition_narrowing(const Expr* condition, FunctionContext& func);
	void apply_narrowing(const NarrowingInfo& narrowing, bool then_branch,
		FunctionContext& func);
	void restore_narrowing(const NarrowingInfo& narrowing, FunctionContext& func);
	static bool branch_returns(const std::vector<StmtPtr>& body);
	int open_scope(FunctionContext& func);
	void emit_scope_release(int scope_id, FunctionContext& func);
	int push_block_scope(FunctionContext& func);
	void pop_block_scope(int scope_id, FunctionContext& func);

	void gen_while(const WhileStmt* stmt, FunctionContext& func);
	void gen_for(const ForStmt* stmt, FunctionContext& func);
	void gen_break(const BreakStmt* stmt, FunctionContext& func);
	void gen_continue(const ContinueStmt* stmt, FunctionContext& func);
	void gen_expr_stmt(const ExprStmt* stmt, FunctionContext& func);
	void emit_conditional_branch(IROpcode opcode, int cond_reg, const std::string& label, FunctionContext& func);

	int gen_expr(const Expr* expr, FunctionContext& func);
	int gen_literal(const LiteralExpr* expr, FunctionContext& func);
	// Lvalue writeback: the read yields a copy; the caller needs the container.
	struct VariableOrigin {
		int container_reg = -1;
		bool borrowed = false;
	};
	int gen_variable(const VariableExpr* expr, FunctionContext& func,
		VariableOrigin* origin = nullptr);
	int gen_binary(const BinaryExpr* expr, FunctionContext& func);
	bool absorb_str_call(FunctionContext& func, int reg, size_t since, std::vector<int>& args);
	int gen_str_call(const std::vector<int>& args, FunctionContext& func);
	int gen_struct_string(int value_reg, const StructDecl& decl, FunctionContext& func,
		const Expr* site = nullptr);
	int gen_string_concat(const BinaryExpr* expr, FunctionContext& func,
		int& left_reg, int& right_reg);
	int gen_logical(const BinaryExpr* expr, FunctionContext& func);
	int gen_unary(const UnaryExpr* expr, FunctionContext& func);
	int gen_await(const AwaitExpr* expr, FunctionContext& func);
	int gen_type_test(const TypeTestExpr* expr, FunctionContext& func);
	int gen_trait_test(int value_reg, const TraitDecl& iface, FunctionContext& func);
	int require_trait_value(int value_reg, const TraitDecl& iface,
		const std::string& what, FunctionContext& func, int line, int column,
		bool nullable = false);
	// Replace Dictionary with its keys Array for position-based iteration.
	void gen_dictionary_keys_for_iteration(int iterable_reg, FunctionContext& func);


	int gen_assert(const CallExpr* expr, FunctionContext& func);
	int gen_get_node(const std::string& path, FunctionContext& func);
	const std::string* constant_string(const Expr* expr, FunctionContext& func);
	int gen_load_resource(const std::string& path, FunctionContext& func);
	std::string resolve_resource_path(const std::string& path) const;
	int gen_range(const CallExpr* expr, FunctionContext& func);
	int gen_color8(const CallExpr* expr, FunctionContext& func);
	int gen_class_test(int value_reg, const std::string& class_name, FunctionContext& func);
	int gen_instance_class_test(int value_reg, const std::string& class_name, int result_reg,
		FunctionContext& func);
	int gen_cast(const CastExpr* expr, FunctionContext& func);
	int gen_builtin_cast(const CastExpr* expr, IRInstruction::TypeHint target, FunctionContext& func);
	int gen_class_cast(const CastExpr* expr, FunctionContext& func);
	int gen_int_immediate(int64_t value, FunctionContext& func);
	int gen_enum_member(const EnumDecl::Member& member, FunctionContext& func);
	int gen_enum_dictionary(const EnumDecl& decl, FunctionContext& func);
	// A loop body invalidates the single-character property of everything it
	// assigns, for the whole loop rather than from the assignment onwards.
	void invalidate_loop_character_registers(const std::vector<StmtPtr>& body,
		FunctionContext& func);
	// `for c in <String>`: batched character walk, see codegen.cpp.
	void gen_string_walk(const ForStmt* stmt, int string_reg, FunctionContext& func);
	bool string_walk_uses_only_codepoints(const ForStmt* stmt) const;
	// `for v in <Array>`: ECALL_ARRAY_BATCH fills sixteen guest Variant slots.
	void gen_array_walk(const ForStmt* stmt, int array_reg, FunctionContext& func,
		const StructDecl* element_struct, const TraitDecl* element_trait);
	void gen_numeric_for(const ForStmt* stmt, int start_reg, int end_reg, int step_reg,
		FunctionContext& func);
	int gen_float_immediate(double value, FunctionContext& func);
	// Returns -1 if `type` has no constant `name`.
	int gen_builtin_constant(const std::string& type, const std::string& name, FunctionContext& func);
	int gen_ternary(const TernaryExpr* expr, FunctionContext& func);
	int gen_call(const CallExpr* expr, FunctionContext& func);
	int gen_lambda(const LambdaExpr* expr, FunctionContext& func);
	int gen_make_callable(const std::string& function_name, int bound_reg,
		FunctionContext& func);
	int gen_callable_constructor(const CallExpr* expr, FunctionContext& func);
	int gen_callable_variable_call(const CallExpr* expr, int callable_reg,
		std::vector<int>& arg_regs, FunctionContext& func);
	int gen_member_call(const MemberCallExpr* expr, FunctionContext& func);
	void gen_builtin_method(const BuiltinMethod& method, int result_reg, int obj_reg,
		const std::vector<int>& arg_regs, FunctionContext& func);
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

	// Queue grows while iterating (nested lambdas append).
	struct PendingLambda {
		const FunctionDecl* decl;
		std::string lifted_name;
		std::vector<std::string> captures;
		// m_current_class is cleared before lambda lowering; this preserves scope.
		const StructDecl* owner = nullptr;
		int chain_link = 0;
		std::string chain_function;
	};
	std::vector<PendingLambda> m_pending_lambdas;
	int m_next_lambda = 0;

	std::vector<std::string> collect_captures(const FunctionDecl& decl,
		FunctionContext& enclosing) const;

	int alloc_register(FunctionContext& func);
	void free_register(FunctionContext& func, int reg);
	std::string make_label(const std::string& prefix);
	int add_string_constant(const std::string& str);

	// Program-wide: labels unique across all functions, string pool shared.
	int m_next_label = 0;
	std::vector<std::string> m_string_constants;

	// Interned operand names; moved into the IRProgram at the end of generate().
	IRStringTable m_strings;
	IRValue ir_label(const std::string& name) { return IRValue::label(m_strings.intern(name)); }
	IRValue ir_var(const std::string& name) { return IRValue::var(m_strings.intern(name)); }
	IRValue ir_str(const std::string& text) { return IRValue::str(m_strings.intern(text)); }
	const std::string& operand_text(const IRValue& value) const { return m_strings[value.string_id]; }

	void push_scope(FunctionContext& func);
	void pop_scope(FunctionContext& func);
	Variable* find_variable(FunctionContext& func, const std::string& name);
	void declare_variable(FunctionContext& func, const std::string& name, int register_num,
		bool is_const = false, const Stmt* site = nullptr, bool is_variant = false,
		bool is_parameter = false);
	void reject_reclassification(const Variable& var, int value_reg,
		const FunctionContext& func, const Stmt* site);

	void set_register_type(FunctionContext& func, int reg, IRInstruction::TypeHint type);
	IRInstruction::TypeHint get_register_type(const FunctionContext& func, int reg) const;
	bool is_inline_primitive_constructor(const std::string& name) const;
	bool is_host_constructor(const std::string& name) const;
	int gen_host_constructor(const std::string& name, const std::vector<int>& arg_regs,
		FunctionContext& func, const Expr* site);
	int gen_host_constructor_typed(const std::string& name, IRInstruction::TypeHint variant_type,
		const std::vector<int>& arg_regs, FunctionContext& func, const Expr* site);
	static constexpr size_t MAX_HOST_CONSTRUCTOR_ARGS = 8;
	bool is_inline_member_access(IRInstruction::TypeHint type, const std::string& member) const;
	int gen_inline_constructor(const std::string& name, const std::vector<int>& arg_regs,
		FunctionContext& func, const Expr* site);
	int gen_inline_member_get(int obj_reg, IRInstruction::TypeHint obj_type, const std::string& member, FunctionContext& func);
	void gen_inline_member_set(int obj_reg, IRInstruction::TypeHint obj_type, const std::string& member,
		int value_reg, FunctionContext& func, bool stamp_type = true);

	// Types with `member` inline; non-empty means VGET/VSET (Object-only) is wrong.
	std::vector<IRInstruction::TypeHint> inline_member_types(const std::string& member) const;
	struct InlineMemberGroup {
		std::vector<IRInstruction::TypeHint> types;
	};
	std::vector<InlineMemberGroup> inline_member_groups(const std::string& member) const;
	void emit_group_type_test(int obj_reg, const InlineMemberGroup& group,
		const std::string& next_label, FunctionContext& func);
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
	void gen_element_store(int obj_reg, int idx_reg, int value_reg, FunctionContext& func,
		const Expr* site = nullptr);
	void gen_string_at(int dest, int obj_reg, int idx_reg, FunctionContext& func,
		const Expr* site);
	void gen_variant_get(int dest, int obj_reg, int idx_reg, FunctionContext& func);

	// Global functions: must not fall through to self-call (Godot drops the VCALL).
	bool is_global_function(const std::string& name) const;
	int gen_global_function(const CallExpr* expr, std::vector<int>& arg_regs, FunctionContext& func);

	// Emit GLOBAL_CALL; resolves NUMERIC to int/float form when types are known.
	int gen_global_call(const GlobalFunction& info, const std::vector<int>& arg_regs,
		FunctionContext& func, const Expr* site);

	// Structs: Dictionary with a fixed key set, field-checked at compile time.

	const StructDecl* find_struct(const std::string& name) const;
	const TraitDecl* find_trait(const std::string& name) const;
	const FunctionDecl* find_trait_method(const TraitDecl& trait, const std::string& name) const;
	const VarDeclStmt* find_trait_var(const TraitDecl& trait, const std::string& name) const;
	const StructField* find_trait_constant(const TraitDecl& trait, const std::string& name) const;
	const SignalDecl* find_trait_signal(const TraitDecl& trait, const std::string& name) const;
	std::string trait_required_base(const TraitDecl& trait) const;
	// True when engine class 'actual' has 'required' somewhere in its
	// ancestor chain, per the host-supplied ancestry.
	bool engine_class_derives_from(const std::string& actual,
		const std::string& required) const;
	bool declaration_uses(const StructDecl& decl, const TraitDecl& iface) const;
	std::vector<const TraitDecl*> used_traits(const StructDecl& decl) const;
	void validate_uses(const Program& program) const;
	void validate_trait_member(const std::string& kind, const std::string& name,
		const std::vector<FunctionDecl>& methods, const StructDecl* decl,
		const std::vector<std::string>& names, int line, int column) const;
	const StructDecl* class_base(const StructDecl& decl) const;
	const std::string* native_base(const StructDecl& decl) const;
	int gen_native_base_load(int self_reg, FunctionContext& func);
	// True when the script declares a class extending an engine class, and a
	// Dictionary reaching an untyped `.x` may therefore be one of its instances.
	bool has_engine_based_classes() const { return !m_native_bases.empty(); }
	int gen_dict_has(int obj_reg, const std::string& key, FunctionContext& func);
	// The object a bare name falls through to, or -1 when there is none.
	int gen_implicit_base_load(FunctionContext& func);
	std::vector<const StructField*> struct_fields(const StructDecl& decl) const;
	const StructField* find_struct_field(const StructDecl& decl, const std::string& name) const;
	int struct_field_index(const StructDecl& decl, const std::string& name) const;
	std::string struct_field_list(const StructDecl& decl) const;
	const FunctionDecl* find_class_method(const StructDecl& decl, const std::string& name,
		const StructDecl** owner = nullptr) const;
	// '@' prefix avoids collision with script-declared identifiers.
	static std::string lifted_method_name(const StructDecl& decl, const std::string& method);
	void register_classes(const Program& program);
	int gen_class_construct(const StructDecl& decl, const std::vector<ExprPtr>& arguments,
		const NamedArguments& names, FunctionContext& func, const Expr* site);
	int gen_class_method_call(const StructDecl& decl, const FunctionDecl& method,
		const StructDecl& owner, int self_reg, const std::vector<ExprPtr>& arguments,
		const NamedArguments& names, FunctionContext& func, const Expr* site);
	int class_field_self(const std::string& name, FunctionContext& func);
	bool is_super(const Expr* expr, FunctionContext& func);
	int gen_super_call(const MemberCallExpr* expr, FunctionContext& func);
	int gen_super_init(const CallExpr* expr, FunctionContext& func);
	int gen_chain_super_call(const std::string& name, const std::vector<ExprPtr>& arguments,
		const NamedArguments& names, FunctionContext& func, const Expr* site);

	const StructField& require_struct_field(const StructDecl& decl, const std::string& field_name,
		int line, int column) const;

	void check_struct_subscript(int obj_reg, const Expr* index, FunctionContext& func);

	int gen_struct_construct(const StructDecl& decl, const std::vector<ExprPtr>& arguments,
		const NamedArguments& names, FunctionContext& func, const Expr* site);

	int gen_field_default(const StructDecl& decl, const StructField& field, FunctionContext& func);
	// Returns -1 for types with no guest-constructible default.
	int gen_default_value(const TypeExpr& type_hint, FunctionContext& func);

	int gen_dict_get(int obj_reg, const std::string& key, FunctionContext& func);
	void gen_dict_set(int obj_reg, const std::string& key, int value_reg, FunctionContext& func);

	void apply_declared_type(int reg, const TypeExpr& type_hint, FunctionContext& func);
	void coerce_parameters(const std::vector<Parameter>& parameters, FunctionContext& func);
	FunctionSignature build_signature(const FunctionDecl& decl) const;
	ClassSignature build_class_signature(const StructDecl& decl,
		const std::string& engine_base) const;
	ClassSignature build_trait_signature(const TraitDecl& decl) const;

	void set_register_struct(FunctionContext& func, int reg, const StructDecl* decl);
	const StructDecl* get_register_struct(const FunctionContext& func, int reg) const;
	void add_register_trait(FunctionContext& func, int reg, const TraitDecl* decl);
	// 'proven_only' consults register_traits alone. A declaration is not proof:
	// a nullable slot may hold null until a check narrows it.
	const TraitDecl* get_register_trait(const FunctionContext& func, int reg,
		const std::string& method = {}, bool proven_only = false) const;

	void reject_named_arguments(const NamedArguments& names, const std::string& what,
		const Expr* site) const;

	const SignalDecl* find_signal(const std::string& name) const;
	int gen_signal_value(const std::string& name, FunctionContext& func, const Expr* site);
	std::string signal_name_of(const Expr* expr, FunctionContext& func);
	static const char* signal_owner_method(const std::string& member);
	int gen_signal_owner_call(const std::string& signal_name, const char* owner_method,
		const MemberCallExpr* expr, FunctionContext& func);
	FunctionSignature build_signal_signature(const SignalDecl& decl) const;
	void reject_signal_collision(const std::string& what, const std::string& name,
		int line, int column) const;

	std::unordered_map<std::string, const StructDecl*> m_structs;
	std::unordered_map<std::string, const TraitDecl*> m_traits;
	std::unordered_map<const TraitDecl*, size_t> m_trait_indices;
	std::unordered_map<const StructDecl*, std::string> m_native_bases;
	const StructDecl* m_current_class = nullptr;
	std::string m_script_base_class;
	ChainInfo m_chain;
	int m_current_chain_link = 0;
	std::string m_current_chain_function;
	bool m_restricted = false;
	bool m_struct_checks = true;
	bool m_struct_deep_checks = false;
	bool m_trait_structural_fallback = true;
	std::string m_source_path;
	std::unordered_set<std::string> m_autoloads;
	std::unordered_map<std::string, std::string> m_global_script_classes;
	// Class name -> comma-separated ancestor chain, exactly as the host sends
	// it. Kept unparsed: over a thousand entries arrive with every compile,
	// and only trait base-class checks ever read one.
	std::vector<std::pair<std::string, std::string>> m_engine_ancestry;
	std::unordered_map<std::string, const EnumDecl*> m_enums;
	std::unordered_map<std::string, const EnumDecl::Member*> m_enum_members;
	std::unordered_map<std::string, const SignalDecl*> m_signals;

	// Recursion guard for struct defaults that contain themselves.
	std::vector<const StructDecl*> m_struct_default_stack;


	bool is_global_class(const std::string& name) const;
	bool is_autoload(const std::string& name) const;
	bool names_a_chain_class(const std::string& name, FunctionContext& func);
	const VariableExpr* engine_enum_qualifier(const Expr* expr, FunctionContext& func);
	const std::string* chain_qualified_member(const Expr* expr, FunctionContext& func);
	int emit_local_call(const std::string& name, std::vector<int> arg_regs,
		FunctionContext& func, const Expr* site);
	bool names_an_engine_type(const std::string& name, FunctionContext& func);
	Variant::Type names_a_builtin_type(const std::string& name, FunctionContext& func);
	std::string script_level_super_hint() const;
	const std::string* global_script_class_path(const std::string& name) const;
	int gen_global_class_get(const std::string& class_name, FunctionContext& func);
	int gen_string_value(const std::string& text, FunctionContext& func);
	int gen_engine_class_static_call(const std::string& class_name,
		const MemberCallExpr* expr, FunctionContext& func);
	int gen_engine_class_constant(const std::string& class_name,
		const std::string& constant_name, FunctionContext& func);
	// @GlobalScope enum member (Side.SIDE_LEFT): an immediate, no engine call.
	int gen_global_enum_value(const std::string& enum_name,
		const std::string& member_name, FunctionContext& func, const Expr* site);
	int gen_script_class_new(const std::string& class_name, const std::string& path,
		const MemberCallExpr* expr, FunctionContext& func);
	int gen_engine_class_new(const std::string& class_name, const MemberCallExpr* expr,
		FunctionContext& func);

	bool is_local_function(const std::string& name) const;
	static std::unordered_set<std::string> get_global_classes();
	std::unordered_set<std::string> m_local_functions;
	std::unordered_map<std::string, const FunctionDecl*> m_local_signatures;
	std::unordered_map<std::string, size_t> m_global_variables;
	std::unordered_set<std::string> m_global_consts;
	bool is_global_variable(const std::string& name) const;
	bool is_global_const(const std::string& name) const;

	std::unordered_map<std::string, IRGlobalVar> m_global_const_values;
	// 'Class.NAME' -> its folded value; a class constant never reaches the IR.
	std::unordered_map<std::string, IRGlobalVar> m_class_constants;
	// Forward references read NIL; rejected at this boundary.
	size_t m_globals_lowered = 0;
	bool m_members_in_scope = true;
	std::vector<bool> m_global_is_member;

	bool fold_global_initializer(const Expr* expr, IRGlobalVar& out) const;

	// Decline reason; match ignores it, switch promotes it to a compile error.
	struct JumpTableReject {
		std::string reason;
		std::string hint;
		int line = 0;
		int column = 0;
	};

	// SWITCH for dense integer-constant arms; false if declined, reason in reject.
	bool gen_match_jump_table(const MatchStmt* stmt, int subject_reg,
	                          const std::vector<std::string>& body_labels,
	                          const std::string& default_label, FunctionContext& func,
	                          JumpTableReject* reject = nullptr);

	// Match patterns: fall through on match, jump to fail_label otherwise.
	void gen_branch_test(const MatchStmt::Branch& branch, int subject_reg,
	                     const std::string& fail_label, FunctionContext& func);
	void gen_pattern_test(const MatchPattern& pattern, int subject_reg,
	                      const std::string& fail_label, FunctionContext& func);
	void gen_array_pattern_test(const MatchPattern& pattern, int subject_reg,
	                            const std::string& fail_label, FunctionContext& func);
	void gen_dictionary_pattern_test(const MatchPattern& pattern, int subject_reg,
	                                 const std::string& fail_label, FunctionContext& func);
	void gen_struct_pattern_test(const MatchPattern& pattern, int subject_reg,
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
	int gen_struct_shape_test(int value_reg, const StructDecl& decl, FunctionContext& func);
	int require_struct_value(int value_reg, const StructDecl& decl, const std::string& what,
		FunctionContext& func, int line, int column);
	int gen_compare(IROpcode opcode, int left_reg, int right_reg, FunctionContext& func);
	// TypeHint_NONE forces Variant::evaluate() fallback.
	static IRInstruction::TypeHint fused_compare_type(IRInstruction::TypeHint left,
	                                                  IRInstruction::TypeHint right);

	// Returns -1 for non-const or container globals (those stay on LOAD_GLOBAL).
	int gen_const_global_value(const std::string& name, FunctionContext& func);
	int gen_folded_const(const IRGlobalVar& global, FunctionContext& func);
	void register_class_constants(const Program& program);
	int gen_class_constant(const StructDecl& decl, const std::string& name,
		FunctionContext& func);

	static IRInstruction::TypeHint derive_global_value_type(const IRGlobalVar& global);

	void apply_default_initializer(IRGlobalVar& global, FunctionContext& init_func,
		size_t global_index, bool& has_global_init);

	static IROpcode packed_array_opcode(IRInstruction::TypeHint type);
	// Implicit int->float widening; rejects type mismatches.
	int coerce_to_declared_type(int reg, IRInstruction::TypeHint declared,
		FunctionContext& func, const std::string& what, const Stmt* site);
	int coerce_to_declared_type(int reg, IRInstruction::TypeHint declared,
		FunctionContext& func, const std::string& what, int line, int column,
		const std::string& display = {});
	int coerce_to_declared_type(int reg, TypeSet declared, FunctionContext& func,
		const std::string& what, int line, int column, const std::string& display = {});
	int coerce_to_declared_type(int reg, TypeSet declared, FunctionContext& func,
		const std::string& what, const Stmt* site, const std::string& display = {});
	TypeSet type_set_from(const TypeExpr& type, int line = 0, int column = 0) const;
	IRInstruction::TypeHint single_type_from(const TypeExpr& type) const;
	int32_t published_type_from(const TypeExpr& type) const;

	void coerce_folded_initializer(IRGlobalVar& global, const TypeExpr& declared,
		int line, int column) const;
	std::vector<IRInstruction::TypeHint> m_global_types;
	std::vector<TypeSet> m_global_sets;
	std::vector<std::string> m_global_type_names;
	// Struct per global, for field-name checking on load.
	std::vector<const StructDecl*> m_global_structs;
	std::vector<const TraitDecl*> m_global_traits;
	std::vector<const StructDecl*> m_global_array_element_structs;
	std::vector<const StructDecl*> m_global_dictionary_value_structs;
	std::vector<const TraitDecl*> m_global_array_element_traits;
	std::vector<const TraitDecl*> m_global_dictionary_value_traits;
	std::vector<bool> m_global_holds_object;

	bool type_hint_names_a_class(const std::string& type_hint) const;
	void mark_global_holds_object(int64_t global_idx);

	std::vector<std::string> m_global_setters;
	std::vector<std::string> m_global_getters;
	// function name -> global indices it is an accessor for
	std::unordered_map<std::string, std::vector<size_t>> m_accessor_properties;
	// Globals accessed as raw storage (inside their own accessor).
	std::unordered_set<size_t> m_direct_globals;

	bool m_saw_breakpoint_statement = false;

	void collect_property_accessors(const Program& program);
	void emit_missing_export_accessors(IRProgram& ir_program);
	void enter_accessor_scope(const std::string& function_name);
	const std::string& global_setter(size_t index) const;
	const std::string& global_getter(size_t index) const;
	int gen_property_get(size_t index, FunctionContext& func);
	void gen_property_set(size_t index, int value_reg, FunctionContext& func);
};

} // namespace gdscript
