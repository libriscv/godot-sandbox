#pragma once
#include "token.h"
#include "ast.h"
#include <vector>
#include <memory>

namespace gdscript {

class Parser {
public:
	explicit Parser(std::vector<Token> tokens);

	Program parse();

private:
	// Function parsing
	FunctionDecl parse_function();
	std::vector<Parameter> parse_parameters();

	// Struct parsing
	StructDecl parse_struct();

	// Enum parsing
	EnumDecl parse_enum();

	// Match patterns. A pattern is not an expression: `[a, 1]` destructures an
	// Array rather than building one, and `_` and `var name` mean nothing
	// outside a match.
	MatchPatternPtr parse_match_pattern();
	MatchPatternPtr parse_match_array_pattern();
	MatchPatternPtr parse_match_dictionary_pattern();
	// Consume a trailing `..` if present. `what` names the construct in the error
	// for a `..` in the middle.
	bool parse_pattern_rest(const char* what);

	// Argument list of a call, up to and including the closing ')'. Fills
	// `names` in step with `arguments`: the name an argument was passed under,
	// or an empty string when it was passed positionally.
	void parse_argument_list(std::vector<ExprPtr>& arguments, std::vector<std::string>& names);

	// Statement parsing
	StmtPtr parse_statement();
	StmtPtr parse_statement_impl();
	StmtPtr parse_var_decl(bool is_const);
	StmtPtr parse_if_stmt();
	StmtPtr parse_while_stmt();
	StmtPtr parse_for_stmt();
	StmtPtr parse_match_stmt();
	StmtPtr parse_return_stmt();
	StmtPtr parse_expr_or_assign_stmt();
	std::vector<StmtPtr> parse_block();

	// Expression parsing (precedence climbing)
	ExprPtr parse_expression();
	ExprPtr parse_ternary();
	ExprPtr parse_or_expression();
	ExprPtr parse_and_expression();
	ExprPtr parse_not();
	ExprPtr parse_inclusion();
	ExprPtr parse_equality();
	ExprPtr parse_comparison();
	ExprPtr parse_bit_or();
	ExprPtr parse_bit_xor();
	ExprPtr parse_bit_and();
	ExprPtr parse_shift();
	ExprPtr parse_term();
	ExprPtr parse_factor();
	ExprPtr parse_unary();
	ExprPtr parse_power();
	ExprPtr parse_type_test();
	ExprPtr parse_call();
	ExprPtr parse_primary();

	// -= Source positions =-
	//
	// Every AST node carries the position it starts at. A diagnostic that says
	// what is wrong but not where is only half a diagnostic, and every node
	// built here is built through one of these so that none of them is missed.

	// Build a node positioned at `token`.
	template <typename Node, typename... Args>
	static std::unique_ptr<Node> make_at(const Token& token, Args&&... args) {
		auto node = std::make_unique<Node>(std::forward<Args>(args)...);
		node->line = token.line;
		node->column = token.column;
		return node;
	}

	// Build a node positioned where an existing node is: an expression built
	// out of sub-expressions starts where its left-most operand starts.
	template <typename Node, typename... Args>
	static std::unique_ptr<Node> make_like(const Expr& like, Args&&... args) {
		const int line = like.line;
		const int column = like.column;
		auto node = std::make_unique<Node>(std::forward<Args>(args)...);
		node->line = line;
		node->column = column;
		return node;
	}

	// A binary expression starts where its left operand does. Reading the
	// position before the operand is moved into the node is why this is a
	// function rather than an expression at each of the twelve call sites.
	static ExprPtr make_binary(ExprPtr left, BinaryExpr::Op op, ExprPtr right);

	// A second copy of an assignment target, for rewriting `a op= b` into
	// `a = a op b`. Null when the target cannot be read twice without changing
	// the program's behaviour.
	static ExprPtr clone_lvalue(const Expr* expr);

	// Utilities
	bool match(TokenType type);
	bool match_one_of(std::initializer_list<TokenType> types);
	bool check(TokenType type) const;
	Token advance();
	Token peek() const;
	Token peek_ahead(size_t offset) const;
	Token previous() const;
	bool is_at_end() const;
	Token consume(TokenType type, const std::string& message);
	void synchronize();
	void error(const std::string& message);
	void skip_newlines();
	// Consume a statement end: a newline, or a ';' before the next statement on
	// the same line.
	void consume_statement_end(const std::string& message);

	// Type hint parsing
	std::string parse_type_hint();  // Parse optional type hint (e.g., ": int", ": String")
	// Optional return type, e.g. `-> void`.
	std::string parse_return_type();
	// Read past the element types of `Array[int]` / `Dictionary[K, V]`.
	void skip_type_arguments();

	// Attribute parsing
	bool parse_attribute();  // Parse attribute (e.g., @export), returns true if @export

	std::vector<Token> m_tokens;
	size_t m_current = 0;
};

} // namespace gdscript
