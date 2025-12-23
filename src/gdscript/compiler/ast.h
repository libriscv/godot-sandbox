#pragma once
#include <memory>
#include <string>
#include <variant>
#include <vector>

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
	enum class Type { INTEGER,
		FLOAT,
		STRING,
		BOOL,
		NULL_VAL };
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
		ADD,
		SUB,
		MUL,
		DIV,
		MOD,
		EQ,
		NEQ,
		LT,
		LTE,
		GT,
		GTE,
		AND,
		OR
	};

	ExprPtr left;
	Op op;
	ExprPtr right;

	BinaryExpr(ExprPtr l, Op o, ExprPtr r) : left(std::move(l)), op(o), right(std::move(r)) {}
};

// Unary operation: -x, not y
struct UnaryExpr : Expr {
	enum class Op { NEG,
		NOT };

	Op op;
	ExprPtr operand;

	UnaryExpr(Op o, ExprPtr e) : op(o), operand(std::move(e)) {}
};

// Function call: foo(1, 2, 3)
struct CallExpr : Expr {
	std::string function_name;
	std::vector<ExprPtr> arguments;

	CallExpr(std::string name, std::vector<ExprPtr> args) : function_name(std::move(name)), arguments(std::move(args)) {}
};

// Member access: obj.method(args) or obj.property
struct MemberCallExpr : Expr {
	ExprPtr object;
	std::string member_name;
	std::vector<ExprPtr> arguments; // Empty if property access

	MemberCallExpr(ExprPtr obj, std::string name, std::vector<ExprPtr> args = {}) : object(std::move(obj)), member_name(std::move(name)), arguments(std::move(args)) {}
};

// Array index: arr[0]
struct IndexExpr : Expr {
	ExprPtr object;
	ExprPtr index;

	IndexExpr(ExprPtr obj, ExprPtr idx) : object(std::move(obj)), index(std::move(idx)) {}
};

// Array literal: [1, 2, 3]
struct ArrayExpr : Expr {
	std::vector<ExprPtr> elements;

	ArrayExpr(std::vector<ExprPtr> elems) : elements(std::move(elems)) {}
};

// Dictionary literal: {"key1": value1, "key2": value2}
struct DictionaryExpr : Expr {
	struct KeyValue {
		ExprPtr key;
		ExprPtr value;
	};
	std::vector<KeyValue> pairs;

	DictionaryExpr(std::vector<KeyValue> p) : pairs(std::move(p)) {}
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

// Variable declaration: var x = 10
struct VarDeclStmt : Stmt {
	std::string name;
	ExprPtr initializer; // Can be null

	VarDeclStmt(std::string n, ExprPtr init = nullptr) : name(std::move(n)), initializer(std::move(init)) {}
};

// Assignment: x = 42 or arr[0] = value
struct AssignStmt : Stmt {
	ExprPtr target; // VariableExpr for x = value, IndexExpr for arr[0] = value
	ExprPtr value;
	// Future: std::optional<std::string> type_hint; // For: arr[0]: int = 5

	AssignStmt(ExprPtr t, ExprPtr v) : target(std::move(t)), value(std::move(v)) {}
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

	IfStmt(ExprPtr cond, std::vector<StmtPtr> then_b, std::vector<StmtPtr> else_b = {}) : condition(std::move(cond)), then_branch(std::move(then_b)), else_branch(std::move(else_b)) {}
};

// While statement
struct WhileStmt : Stmt {
	ExprPtr condition;
	std::vector<StmtPtr> body;

	WhileStmt(ExprPtr cond, std::vector<StmtPtr> b) : condition(std::move(cond)), body(std::move(b)) {}
};

// For statement: for variable in iterable:
struct ForStmt : Stmt {
	std::string variable; // Loop variable name
	ExprPtr iterable; // Expression to iterate over (e.g., range(10))
	std::vector<StmtPtr> body;

	ForStmt(std::string var, ExprPtr iter, std::vector<StmtPtr> b) : variable(std::move(var)), iterable(std::move(iter)), body(std::move(b)) {}
};

// Break statement
struct BreakStmt : Stmt {};

// Continue statement
struct ContinueStmt : Stmt {};

// Pass statement (no-op)
struct PassStmt : Stmt {};

// Function parameter
struct Parameter {
	std::string name;
	// For simple version, no type annotations
};

// Function declaration
struct FunctionDecl {
	std::string name;
	std::vector<Parameter> parameters;
	std::vector<StmtPtr> body;
	int line = 0;
	int column = 0;
};

// Top-level program
struct Program {
	std::vector<FunctionDecl> functions;
};

} // namespace gdscript
