#pragma once
#include <memory>
#include <vector>
#include <string>
#include <variant>

namespace gdscript {

// Forward declarations
struct Expr;
struct Stmt;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// Expression base class
struct Expr {
	virtual ~Expr() = default;
	int line = 0;
	int column = 0;
};

// Literal expression: 42, 3.14, "hello", true, false, null
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

// Variable reference: x
struct VariableExpr : Expr {
	std::string name;
	explicit VariableExpr(std::string n) : name(std::move(n)) {}
};

// Binary operation: a + b, x * y
struct BinaryExpr : Expr {
	enum class Op {
		ADD, SUB, MUL, DIV, MOD,
		EQ, NEQ, LT, LTE, GT, GTE,
		AND, OR,
		BIT_AND, BIT_OR, BIT_XOR, SHL, SHR
	};

	ExprPtr left;
	Op op;
	ExprPtr right;

	BinaryExpr(ExprPtr l, Op o, ExprPtr r)
		: left(std::move(l)), op(o), right(std::move(r)) {}
};

// Unary operation: -x, not y
struct UnaryExpr : Expr {
	enum class Op { NEG, NOT, BIT_NOT };

	Op op;
	ExprPtr operand;

	UnaryExpr(Op o, ExprPtr e) : op(o), operand(std::move(e)) {}
};

// Ternary conditional: true_value if condition else false_value
struct TernaryExpr : Expr {
	ExprPtr condition;
	ExprPtr true_value;
	ExprPtr false_value;

	TernaryExpr(ExprPtr cond, ExprPtr t, ExprPtr f)
		: condition(std::move(cond)), true_value(std::move(t)), false_value(std::move(f)) {}
};

// -= Named call arguments =-
//
// `BankAccount.new(balance = 10)` names the argument it passes. The name lives
// beside the argument rather than inside it, so that everything which walks an
// argument list keeps walking expressions. `argument_names` is either empty --
// the call used no names at all -- or exactly as long as `arguments`, with an
// empty string where an argument was passed positionally.
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

// Function call: foo(1, 2, 3)
struct CallExpr : Expr, NamedArguments {
	std::string function_name;
	std::vector<ExprPtr> arguments;

	CallExpr(std::string name, std::vector<ExprPtr> args)
		: function_name(std::move(name)), arguments(std::move(args)) {}
};

// Member access: obj.method(args) or obj.property
struct MemberCallExpr : Expr, NamedArguments {
	ExprPtr object;
	std::string member_name;
	std::vector<ExprPtr> arguments; // Empty if property access
	bool is_method_call = false;    // true if this is obj.method(), false if obj.property

	MemberCallExpr(ExprPtr obj, std::string name, std::vector<ExprPtr> args = {}, bool is_method = false)
		: object(std::move(obj)), member_name(std::move(name)), arguments(std::move(args)), is_method_call(is_method) {}
};

// Array index: arr[0]
struct IndexExpr : Expr {
	ExprPtr object;
	ExprPtr index;

	IndexExpr(ExprPtr obj, ExprPtr idx)
		: object(std::move(obj)), index(std::move(idx)) {}
};

// Array literal: [1, 2, 3]
struct ArrayLiteralExpr : Expr {
	std::vector<ExprPtr> elements;

	explicit ArrayLiteralExpr(std::vector<ExprPtr> elems)
		: elements(std::move(elems)) {}
};

// Dictionary literal: {"key": "value", "num": 42}
struct DictionaryLiteralExpr : Expr {
	// Key-value pairs
	std::vector<std::pair<ExprPtr, ExprPtr>> elements;

	explicit DictionaryLiteralExpr(std::vector<std::pair<ExprPtr, ExprPtr>> elems)
		: elements(std::move(elems)) {}
};

// Statement base class
struct Stmt {
	virtual ~Stmt() = default;
	int line = 0;
	int column = 0;
};

// Expression statement: print("hello")
struct ExprStmt : Stmt {
	ExprPtr expression;
	explicit ExprStmt(ExprPtr e) : expression(std::move(e)) {}
};

// Variable declaration: var x = 10 or const x = 10
struct VarDeclStmt : Stmt {
	std::string name;
	std::string type_hint;  // Type annotation if present (e.g., "int", "float", "String")
	ExprPtr initializer; // Can be null
	bool is_const = false; // Whether this is a const declaration
	bool is_property = false; // Whether this is an exported property (@export)

	VarDeclStmt(std::string n, ExprPtr init = nullptr, bool const_flag = false)
		: name(std::move(n)), initializer(std::move(init)), is_const(const_flag) {}
};

// Assignment: x = 42 or arr[0] = 42
struct AssignStmt : Stmt {
	std::string name;        // For simple variable assignment
	ExprPtr target;          // For indexed assignment (IndexExpr)
	ExprPtr value;

	// Simple variable assignment
	AssignStmt(std::string n, ExprPtr v)
		: name(std::move(n)), target(nullptr), value(std::move(v)) {}

	// Indexed assignment (e.g., arr[0] = value)
	AssignStmt(ExprPtr t, ExprPtr v)
		: name(""), target(std::move(t)), value(std::move(v)) {}
};

// Return statement: return x
struct ReturnStmt : Stmt {
	ExprPtr value; // Can be null for bare return

	explicit ReturnStmt(ExprPtr v = nullptr) : value(std::move(v)) {}
};

// If statement
struct IfStmt : Stmt {
	ExprPtr condition;
	std::vector<StmtPtr> then_branch;
	std::vector<StmtPtr> else_branch; // Can be empty

	IfStmt(ExprPtr cond, std::vector<StmtPtr> then_b, std::vector<StmtPtr> else_b = {})
		: condition(std::move(cond)), then_branch(std::move(then_b)), else_branch(std::move(else_b)) {}
};

// While statement
struct WhileStmt : Stmt {
	ExprPtr condition;
	std::vector<StmtPtr> body;

	WhileStmt(ExprPtr cond, std::vector<StmtPtr> b)
		: condition(std::move(cond)), body(std::move(b)) {}
};

// For statement: for variable in iterable:
struct ForStmt : Stmt {
	std::string variable;  // Loop variable name
	ExprPtr iterable;      // Expression to iterate over (e.g., range(10))
	std::vector<StmtPtr> body;

	ForStmt(std::string var, ExprPtr iter, std::vector<StmtPtr> b)
		: variable(std::move(var)), iterable(std::move(iter)), body(std::move(b)) {}
};

// Break statement
struct BreakStmt : Stmt {};

// Continue statement
struct ContinueStmt : Stmt {};

// Pass statement (no-op)
struct PassStmt : Stmt {};

// Match statement: match value: <patterns>
struct MatchStmt : Stmt {
	struct Branch {
		// Values this branch matches. Empty means the wildcard pattern '_'.
		std::vector<ExprPtr> patterns;
		std::vector<StmtPtr> body;
	};

	ExprPtr subject;
	std::vector<Branch> branches;

	MatchStmt(ExprPtr subj, std::vector<Branch> b)
		: subject(std::move(subj)), branches(std::move(b)) {}
};

// Function parameter
struct Parameter {
	std::string name;
	std::string type_hint;  // Type annotation if present (e.g., "int", "float", "String")
	ExprPtr default_value;  // Default value if present (e.g., "b = 5"), else nullptr
};

// Function declaration
struct FunctionDecl {
	std::string name;
	std::vector<Parameter> parameters;
	std::string return_type;  // Return type annotation if present (e.g., "void", "int")
	std::vector<StmtPtr> body;
	int line = 0;
	int column = 0;
};

// -= Structs =-
//
// A struct is sugar for a Dictionary with a fixed set of keys: the declaration
// names the keys and their defaults, and every instance is an ordinary
// Dictionary Variant. The compiler keeps the declaration only so that it can
// build that Dictionary and reject a field name the struct does not have --
// nothing about a struct survives into the IR.

// One field of a struct: var balance = 0
struct StructField {
	std::string name;
	std::string type_hint;   // Type annotation if present (e.g. "int")
	ExprPtr default_value;   // Value when the instance does not supply one, or null
	int line = 0;
	int column = 0;
};

// Struct declaration: struct BankAccount: <fields>
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

	// Position of a field in the declaration, which is the position it takes in
	// a positional constructor call. -1 when the struct has no such field.
	int field_index(const std::string& field_name) const {
		for (size_t i = 0; i < fields.size(); i++) {
			if (fields[i].name == field_name) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	// The field names, for the "fields are: ..." line of a diagnostic.
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

// Top-level program
struct Program {
	std::vector<VarDeclStmt> globals; // Global variable declarations
	std::vector<StructDecl> structs;  // Struct declarations
	std::vector<FunctionDecl> functions;
};

} // namespace gdscript
