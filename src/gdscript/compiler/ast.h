#pragma once
#include <memory>
#include <vector>
#include <string>
#include <variant>

namespace gdscript {

struct Expr;
struct Stmt;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

struct Expr {
	virtual ~Expr() = default;
	int line = 0;
	int column = 0;
};

struct LiteralExpr : Expr {
	enum class Type { INTEGER, FLOAT, STRING, BOOL, NULL_VAL };
	Type lit_type;
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

struct BinaryExpr : Expr {
	enum class Op {
		ADD, SUB, MUL, DIV, MOD, POW,
		EQ, NEQ, LT, LTE, GT, GTE,
		AND, OR,
		BIT_AND, BIT_OR, BIT_XOR, SHL, SHR,
		IN
	};

	ExprPtr left;
	Op op;
	ExprPtr right;

	BinaryExpr(ExprPtr l, Op o, ExprPtr r)
		: left(std::move(l)), op(o), right(std::move(r)) {}
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
	std::string type_name;

	TypeTestExpr(ExprPtr v, std::string t)
		: value(std::move(v)), type_name(std::move(t)) {}
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

	CallExpr(std::string name, std::vector<ExprPtr> args)
		: function_name(std::move(name)), arguments(std::move(args)) {}
};

struct MemberCallExpr : Expr, NamedArguments {
	ExprPtr object;
	std::string member_name;
	std::vector<ExprPtr> arguments;
	bool is_method_call = false;

	MemberCallExpr(ExprPtr obj, std::string name, std::vector<ExprPtr> args = {}, bool is_method = false)
		: object(std::move(obj)), member_name(std::move(name)), arguments(std::move(args)), is_method_call(is_method) {}
};

struct IndexExpr : Expr {
	ExprPtr object;
	ExprPtr index;

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
	std::string type_hint;
	ExprPtr initializer;
	bool is_const = false;
	bool is_property = false;

	VarDeclStmt(std::string n, ExprPtr init = nullptr, bool const_flag = false)
		: name(std::move(n)), initializer(std::move(init)), is_const(const_flag) {}
};

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
	std::vector<StmtPtr> then_branch;
	std::vector<StmtPtr> else_branch;

	IfStmt(ExprPtr cond, std::vector<StmtPtr> then_b, std::vector<StmtPtr> else_b = {})
		: condition(std::move(cond)), then_branch(std::move(then_b)), else_branch(std::move(else_b)) {}
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

	MatchStmt(ExprPtr subj, std::vector<Branch> b)
		: subject(std::move(subj)), branches(std::move(b)) {}
};

struct Parameter {
	std::string name;
	std::string type_hint;
	ExprPtr default_value;
};

struct FunctionDecl {
	std::string name;
	std::vector<Parameter> parameters;
	std::string return_type;
	std::vector<StmtPtr> body;
	int line = 0;
	int column = 0;
	// '##' block above declaration; published in the signature for the editor.
	std::string doc_comment;
};

// Sugar for a Dictionary with a fixed key set; nothing survives into IR.
struct StructField {
	std::string name;
	std::string type_hint;
	ExprPtr default_value;
	int line = 0;
	int column = 0;
};

struct StructDecl {
	std::string name;
	std::vector<StructField> fields;
	int line = 0;
	int column = 0;

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
		int line = 0;
		int column = 0;
	};

	std::string name;
	std::vector<Member> members;
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

struct Program {
	std::vector<VarDeclStmt> globals;
	std::vector<StructDecl> structs;
	std::vector<EnumDecl> enums;
	std::vector<FunctionDecl> functions;
};

} // namespace gdscript
