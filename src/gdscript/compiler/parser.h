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

	// Statement parsing
	StmtPtr parse_statement();
	StmtPtr parse_var_decl(bool is_const);
	StmtPtr parse_if_stmt();
	StmtPtr parse_while_stmt();
	StmtPtr parse_for_stmt();
	StmtPtr parse_return_stmt();
	StmtPtr parse_expr_or_assign_stmt();
	std::vector<StmtPtr> parse_block();

	// Expression parsing (precedence climbing)
	ExprPtr parse_expression();
	ExprPtr parse_or_expression();
	ExprPtr parse_and_expression();
	ExprPtr parse_equality();
	ExprPtr parse_comparison();
	ExprPtr parse_term();
	ExprPtr parse_factor();
	ExprPtr parse_unary();
	ExprPtr parse_call();
	ExprPtr parse_primary();

	// Utilities
	bool match(TokenType type);
	bool match_one_of(std::initializer_list<TokenType> types);
	bool check(TokenType type) const;
	Token advance();
	Token peek() const;
	Token previous() const;
	bool is_at_end() const;
	Token consume(TokenType type, const std::string& message);
	void synchronize();
	void error(const std::string& message);
	void skip_newlines();

	// Type hint parsing
	std::string parse_type_hint();  // Parse optional type hint (e.g., ": int", ": String")
	std::string parse_return_type();  // Parse optional return type (e.g., "-> void")

	// Attribute parsing
	bool parse_attribute();  // Parse attribute (e.g., @export), returns true if @export

	std::vector<Token> m_tokens;
	size_t m_current = 0;
};

} // namespace gdscript
