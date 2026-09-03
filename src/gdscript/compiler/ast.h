#pragma once
#include "export_hints.h"
#include "function_signature.h"
#include <map>
#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <variant>

namespace gdscript {

struct Expr;
struct Stmt;
struct FunctionDecl;
struct EnumDecl;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// A declaration type is either one ordinary name or a union of names and
// `null`.  `names` deliberately excludes null: nullable is useful often enough
// to deserve its own bit, and keeps existing single-name users simple.
struct TypeExpr {
	std::vector<std::string> names;
	// Generic arguments for a sole container name (Array[T], Dictionary[K,V]).
	std::vector<TypeExpr> arguments;
	bool nullable = false;
	// Diagnostics retain `T?` instead of normalizing it to `T | null`.
	bool spelled_nullable = false;

	bool empty() const { return names.empty() && !nullable; }
	bool is_union() const { return names.size() + (nullable ? 1u : 0u) > 1; }
	const std::string& single_name() const {
		static const std::string none;
		return names.size() == 1 && !nullable ? names.front() : none;
	}
	const std::string& sole_name() const {
		static const std::string none;
		return names.size() == 1 ? names.front() : none;
	}
	std::string to_string() const {
		if (spelled_nullable && names.size() == 1) {
			return names.front() + "?";
		}
		std::string result;
		for (const std::string& name : names) {
			if (!result.empty()) result += " | ";
			result += name;
			if (names.size() == 1 && !arguments.empty()) {
				result += "[";
				for (size_t i = 0; i < arguments.size(); i++) {
					if (i != 0) result += ", ";
					result += arguments[i].to_string();
				}
				result += "]";
			}
		}
		if (nullable) {
			if (!result.empty()) result += " | ";
			result += "null";
		}
		return result;
	}
	bool operator==(const std::string& name) const {
		return name.empty() ? empty() : single_name() == name;
	}
	bool operator==(const char* name) const { return *this == std::string(name); }
};

struct Expr {
	virtual ~Expr() = default;
	int line = 0;
	int column = 0;
};

struct LiteralExpr : Expr {
	enum class Type { INTEGER, FLOAT, STRING, BOOL, NULL_VAL };
	// Variant type for &"..." and ^"..." string literals.
	enum class StringType { PLAIN, STRING_NAME, NODE_PATH };
	Type lit_type;
	StringType string_type = StringType::PLAIN;
	std::variant<int64_t, double, std::string, bool> value;

	LiteralExpr(int64_t v) : lit_type(Type::INTEGER), value(v) {}
	LiteralExpr(double v) : lit_type(Type::FLOAT), value(v) {}
	LiteralExpr(std::string v) : lit_type(Type::STRING), value(std::move(v)) {}
	LiteralExpr(bool v) : lit_type(Type::BOOL), value(v) {}
	static std::unique_ptr<LiteralExpr> null() {
		auto expr = std::make_unique<LiteralExpr>(false);
		expr->lit_type = Type::NULL_VAL;
		return expr;
	}
};

struct VariableExpr : Expr {
	std::string name;
	explicit VariableExpr(std::string n) : name(std::move(n)) {}
};

struct ErrorExpr : Expr {};

struct BinaryExpr : Expr {
	enum class Op {
		ADD, SUB, MUL, DIV, MOD, POW,
		EQ, NEQ, LT, LTE, GT, GTE,
		AND, OR,
		BIT_AND, BIT_OR, BIT_XOR, SHL, SHR,
		IN,
		// `a ?? b`: b is evaluated only when a is null.
		COALESCE
	};

	ExprPtr left;
	Op op;
	ExprPtr right;

	BinaryExpr(ExprPtr l, Op o, ExprPtr r)
		: left(std::move(l)), op(o), right(std::move(r)) {}
};

// Suspension point; makes the enclosing function a coroutine.
struct AwaitExpr : Expr {
	ExprPtr operand;

	explicit AwaitExpr(ExprPtr e) : operand(std::move(e)) {}
};

// Forward-declared: FunctionDecl is only complete further down this file.
struct LambdaExpr : Expr {
	std::unique_ptr<FunctionDecl> decl;
	std::string lifted_name;

	explicit LambdaExpr(std::unique_ptr<FunctionDecl> d) : decl(std::move(d)) {}
	~LambdaExpr();
};

struct UnaryExpr : Expr {
	enum class Op { NEG, NOT, BIT_NOT };

	Op op;
	ExprPtr operand;

	UnaryExpr(Op o, ExprPtr e) : op(o), operand(std::move(e)) {}
};

// Type kept as name; codegen resolves to Variant::Type or rejects class names.
struct TypeTestExpr : Expr {
	ExprPtr value;
	TypeExpr type;

	TypeTestExpr(ExprPtr v, TypeExpr t)
		: value(std::move(v)), type(std::move(t)) {}
};

struct CastExpr : Expr {
	ExprPtr value;
	std::string type_name;
	std::vector<TypeExpr> type_arguments;

	CastExpr(ExprPtr v, std::string t, std::vector<TypeExpr> arguments = {})
		: value(std::move(v)), type_name(std::move(t)),
		  type_arguments(std::move(arguments)) {}
};

struct TernaryExpr : Expr {
	ExprPtr condition;
	ExprPtr true_value;
	ExprPtr false_value;

	TernaryExpr(ExprPtr cond, ExprPtr t, ExprPtr f)
		: condition(std::move(cond)), true_value(std::move(t)), false_value(std::move(f)) {}
};

// Empty or same length as arguments; empty string for positional entries.
struct NamedArguments {
	std::vector<std::string> argument_names;

	const std::string& argument_name(size_t index) const {
		static const std::string unnamed;
		return index < argument_names.size() ? argument_names[index] : unnamed;
	}
	bool has_named_arguments() const {
		for (const auto& name : argument_names) {
			if (!name.empty()) {
				return true;
			}
		}
		return false;
	}
};

struct CallExpr : Expr, NamedArguments {
	std::string function_name;
	std::vector<ExprPtr> arguments;
	// Set only for `$Node` / `%Unique` sugar. Unlike an arbitrary call, this
	// lookup may be duplicated when a compound-assignment target is cloned.
	bool is_node_path_sugar = false;

	CallExpr(std::string name, std::vector<ExprPtr> args)
		: function_name(std::move(name)), arguments(std::move(args)) {}
};

struct MemberCallExpr : Expr, NamedArguments {
	ExprPtr object;
	std::string member_name;
	std::vector<ExprPtr> arguments;
	bool is_method_call = false;
	// `a?.b` / `a?.b()`: a null object skips the rest of the postfix chain.
	bool safe = false;
	// Outermost link of a chain holding a safe link. It owns the null result,
	// so `a?.b.c` is null rather than an access on null.
	bool safe_chain_root = false;

	MemberCallExpr(ExprPtr obj, std::string name, std::vector<ExprPtr> args = {}, bool is_method = false)
		: object(std::move(obj)), member_name(std::move(name)), arguments(std::move(args)), is_method_call(is_method) {}
};

struct IndexExpr : Expr {
	ExprPtr object;
	ExprPtr index;
	bool safe_chain_root = false;

	IndexExpr(ExprPtr obj, ExprPtr idx)
		: object(std::move(obj)), index(std::move(idx)) {}
};

struct ArrayLiteralExpr : Expr {
	std::vector<ExprPtr> elements;

	explicit ArrayLiteralExpr(std::vector<ExprPtr> elems)
		: elements(std::move(elems)) {}
};

struct DictionaryLiteralExpr : Expr {
	std::vector<std::pair<ExprPtr, ExprPtr>> elements;

	explicit DictionaryLiteralExpr(std::vector<std::pair<ExprPtr, ExprPtr>> elems)
		: elements(std::move(elems)) {}
};

struct Stmt {
	virtual ~Stmt() = default;
	int line = 0;
	int column = 0;
};

struct ExprStmt : Stmt {
	ExprPtr expression;
	explicit ExprStmt(ExprPtr e) : expression(std::move(e)) {}
};

struct VarDeclStmt : Stmt {
	std::string name;
	TypeExpr type_hint;
	ExprPtr initializer;
	bool is_const = false;
	bool is_property = false;
	bool is_static = false;
	bool is_onready = false;
	std::string trait_origin;
	std::string doc_comment;
	int chain_link = 0;
	ExportHint export_hint;
	ExportSection export_section;

	std::unique_ptr<FunctionDecl> setter_body;
	std::unique_ptr<FunctionDecl> getter_body;
	std::string setter_name;
	std::string getter_name;

	VarDeclStmt(std::string n, ExprPtr init = nullptr, bool const_flag = false)
		: name(std::move(n)), initializer(std::move(init)), is_const(const_flag) {}
	VarDeclStmt(VarDeclStmt&&) noexcept;
	VarDeclStmt& operator=(VarDeclStmt&&) noexcept;
	~VarDeclStmt();

	bool has_accessors() const {
		return setter_body || getter_body || !setter_name.empty() || !getter_name.empty();
	}
};

struct BreakpointStmt : Stmt {};

struct AssignStmt : Stmt {
	std::string name;
	ExprPtr target;
	ExprPtr value;

	AssignStmt(std::string n, ExprPtr v)
		: name(std::move(n)), target(nullptr), value(std::move(v)) {}

	AssignStmt(ExprPtr t, ExprPtr v)
		: name(""), target(std::move(t)), value(std::move(v)) {}
};

struct ReturnStmt : Stmt {
	ExprPtr value;

	explicit ReturnStmt(ExprPtr v = nullptr) : value(std::move(v)) {}
};

struct IfStmt : Stmt {
	ExprPtr condition;
	// SafeGDScript extension: `if var value = expression:` evaluates the
	// initializer once and enters only when it is not NIL.  The declaration is
	// visible in the then branch, not in `else` or after the statement.
	std::unique_ptr<VarDeclStmt> binding;
	std::vector<StmtPtr> then_branch;
	std::vector<StmtPtr> else_branch;

	IfStmt(ExprPtr cond, std::vector<StmtPtr> then_b, std::vector<StmtPtr> else_b = {})
		: condition(std::move(cond)), then_branch(std::move(then_b)), else_branch(std::move(else_b)) {}
	IfStmt(std::unique_ptr<VarDeclStmt> bind, std::vector<StmtPtr> then_b,
		std::vector<StmtPtr> else_b = {})
		: binding(std::move(bind)), then_branch(std::move(then_b)),
		  else_branch(std::move(else_b)) {}
};

struct WhileStmt : Stmt {
	ExprPtr condition;
	std::vector<StmtPtr> body;

	WhileStmt(ExprPtr cond, std::vector<StmtPtr> b)
		: condition(std::move(cond)), body(std::move(b)) {}
};

struct ForStmt : Stmt {
	std::string variable;
	ExprPtr iterable;
	std::vector<StmtPtr> body;

	ForStmt(std::string var, ExprPtr iter, std::vector<StmtPtr> b)
		: variable(std::move(var)), iterable(std::move(iter)), body(std::move(b)) {}
};

struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};
struct PassStmt : Stmt {};

struct MatchPattern;
using MatchPatternPtr = std::unique_ptr<MatchPattern>;

struct MatchPattern {
	enum class Kind {
		VALUE,      // 1, "text", OP_ADD -- compared with ==
		WILDCARD,   // _ -- matches anything, binds nothing
		BIND,       // var name -- matches anything, and names it
		ARRAY,      // [p, p, ..] -- an Array of that length, elementwise
		DICTIONARY, // {"k": p, "k2", ..} -- a Dictionary with those keys
		STRUCT,     // Point(x = p, y = p) / Point(p, p)
	};

	Kind kind = Kind::VALUE;
	ExprPtr value;
	std::string name;
	std::vector<MatchPatternPtr> elements;
	// Entry with no value pattern: key must be present, value unconstrained.
	struct Entry {
		ExprPtr key;
		MatchPatternPtr value;
	};
	std::vector<Entry> entries;
	struct StructEntry {
		// Empty for a positional field pattern.
		std::string name;
		MatchPatternPtr value;
	};
	std::string struct_name;
	std::vector<StructEntry> struct_entries;
	bool open = false; // trailing `..`
	int line = 0;
	int column = 0;

	// Binding disqualifies jump table entries and multi-pattern arms.
	bool binds() const {
		if (kind == Kind::BIND) {
			return true;
		}
		for (const auto& element : elements) {
			if (element->binds()) {
				return true;
			}
		}
		for (const auto& entry : entries) {
			if (entry.value && entry.value->binds()) {
				return true;
			}
		}
		for (const auto& entry : struct_entries) {
			if (entry.value && entry.value->binds()) {
				return true;
			}
		}
		return false;
	}
};

struct MatchStmt : Stmt {
	struct Branch {
		std::vector<MatchPatternPtr> patterns;
		ExprPtr guard;
		std::vector<StmtPtr> body;

		// Guard or multiple patterns disqualify catch-all.
		bool is_catch_all() const {
			return !guard && patterns.size() == 1 &&
				patterns[0]->kind == MatchPattern::Kind::WILDCARD;
		}
	};

	ExprPtr subject;
	std::vector<Branch> branches;
	// When true, the jump table is mandatory; decline is a compile error.
	bool is_switch = false;

	MatchStmt(ExprPtr subj, std::vector<Branch> b, bool sw = false)
		: subject(std::move(subj)), branches(std::move(b)), is_switch(sw) {}
};

struct Parameter {
	std::string name;
	TypeExpr type_hint;
	ExprPtr default_value;
	int line = 0;
	int column = 0;
};

struct FunctionDecl {
	std::string name;
	std::vector<Parameter> parameters;
	TypeExpr return_type;
	std::vector<StmtPtr> body;
	int line = 0;
	int column = 0;
	// '##' block above declaration; published in the signature for the editor.
	std::string doc_comment;
	// Has AWAIT; gets a resume entry and returns an awaitable.
	bool is_coroutine = false;
	// `static func` in a class body: lifted without the instance parameter.
	bool is_static = false;
	bool is_abstract = false;
	// `@test`: also published as a test case the host runner can call.
	bool is_test = false;
	std::string trait_origin;
	std::optional<RPCConfig> rpc_config;
	int chain_link = 0;
	// Non-empty when an override displaced this copy onto a mangled symbol.
	std::string chain_name;

	const std::string& declared_name() const {
		return chain_name.empty() ? name : chain_name;
	}
};

// Published beside the ELF; nothing reaches the IR.
struct SignalDecl {
	std::string name;
	std::vector<Parameter> parameters;
	int line = 0;
	int column = 0;
	std::string doc_comment;
	std::string trait_origin;
};

inline LambdaExpr::~LambdaExpr() = default;
inline VarDeclStmt::VarDeclStmt(VarDeclStmt&&) noexcept = default;
inline VarDeclStmt& VarDeclStmt::operator=(VarDeclStmt&&) noexcept = default;
inline VarDeclStmt::~VarDeclStmt() = default;

// Sugar for a Dictionary with a fixed key set; nothing survives into IR.
struct StructField {
	std::string name;
	TypeExpr type_hint;
	ExprPtr default_value;
	int line = 0;
	int column = 0;
	std::string doc_comment;
	std::string trait_origin;
};

struct StructDecl {
	std::string name;
	std::vector<StructField> fields;
	int line = 0;
	int column = 0;
	std::string doc_comment;

	bool is_class = false;
	std::string base_name;
	std::vector<std::string> uses;
	std::vector<FunctionDecl> methods;
	// `const` in a class body: compile-time only, like the file's own consts.
	std::vector<StructField> constants;
	size_t inherited_fields = 0;

	const StructField* find_constant(const std::string& constant_name) const {
		for (const StructField& constant : constants) {
			if (constant.name == constant_name) {
				return &constant;
			}
		}
		return nullptr;
	}

	const FunctionDecl* find_method(const std::string& method_name) const {
		for (const FunctionDecl& method : methods) {
			if (method.name == method_name) {
				return &method;
			}
		}
		return nullptr;
	}

	const StructField* find_field(const std::string& field_name) const {
		for (const auto& field : fields) {
			if (field.name == field_name) {
				return &field;
			}
		}
		return nullptr;
	}

	// Position in the declaration; -1 if absent.
	int field_index(const std::string& field_name) const {
		for (size_t i = 0; i < fields.size(); i++) {
			if (fields[i].name == field_name) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	std::string field_list() const {
		std::string list;
		for (const auto& field : fields) {
			if (!list.empty()) {
				list += ", ";
			}
			list += field.name;
		}
		return list.empty() ? "(none)" : list;
	}
};

// Members are compile-time integers; nothing reaches IR. Unnamed enums export to file scope.
struct EnumDecl {
	struct Member {
		std::string name;
		int64_t value = 0;
		const Expr* value_expr = nullptr; // non-foldable engine constant; value is the auto-increment offset
		int line = 0;
		int column = 0;
	};

	std::string name;
	std::vector<Member> members;
	std::vector<ExprPtr> owned_values;
	int line = 0;
	int column = 0;

	const Member* find_member(const std::string& member_name) const {
		for (const auto& member : members) {
			if (member.name == member_name) {
				return &member;
			}
		}
		return nullptr;
	}
};

struct TraitDecl {
	std::string name;
	std::string base_name;
	std::vector<std::string> uses;
	std::vector<VarDeclStmt> vars;
	std::vector<StructField> constants;
	std::vector<EnumDecl> enums;
	std::vector<SignalDecl> signals;
	std::vector<FunctionDecl> methods;
	int line = 0;
	int column = 0;
	std::string doc_comment;
	bool is_file_level = false;

	const FunctionDecl* find_method(const std::string& member) const {
		for (const FunctionDecl& method : methods) if (method.name == member) return &method;
		return nullptr;
	}
	const VarDeclStmt* find_var(const std::string& member) const {
		for (const VarDeclStmt& var : vars) if (var.name == member) return &var;
		return nullptr;
	}
	const StructField* find_constant(const std::string& member) const {
		for (const StructField& value : constants) if (value.name == member) return &value;
		return nullptr;
	}
	const SignalDecl* find_signal(const std::string& member) const {
		for (const SignalDecl& signal : signals) if (signal.name == member) return &signal;
		return nullptr;
	}
};

struct ChainInfo {
	std::vector<std::string> class_names; // root first
	std::vector<std::string> paths;       // same order, diagnostics only

	struct Origin {
		int link = 0;
		std::string symbol; // mangled '@' name when an override displaced this copy
	};
	std::map<std::string, std::vector<Origin>> functions;

	bool merged() const { return class_names.size() > 1; }

	bool names_a_link(const std::string& name) const {
		for (const std::string& link : class_names) {
			if (!link.empty() && link == name) {
				return true;
			}
		}
		return false;
	}

	const Origin* super_of(const std::string& name, int link) const {
		const auto it = functions.find(name);
		if (it == functions.end()) {
			return nullptr;
		}
		const Origin* found = nullptr;
		for (const Origin& origin : it->second) {
			if (origin.link < link) {
				found = &origin;
			}
		}
		return found;
	}
};

struct Program {
	bool is_tool = false;
	std::string class_name;
	int class_name_line = 0;
	int class_name_column = 0;
	std::string base_class;
	bool base_is_path = false;
	int base_class_line = 0;
	int base_class_column = 0;
	std::string native_base_class;
	bool native_base_is_path = false;
	std::vector<std::string> uses;
	std::string trait_name;
	int trait_name_line = 0;
	int trait_name_column = 0;
	ChainInfo chain;
	std::vector<VarDeclStmt> globals;
	std::vector<StructDecl> structs;
	std::vector<TraitDecl> traits;
	std::vector<EnumDecl> enums;
	std::vector<SignalDecl> signals;
	std::vector<FunctionDecl> functions;
};

} // namespace gdscript
