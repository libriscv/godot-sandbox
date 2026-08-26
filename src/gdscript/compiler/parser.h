#pragma once
#include "token.h"
#include "ast.h"
#include "export_hints.h"
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace gdscript {

class Parser {
public:
	explicit Parser(std::vector<Token> tokens);

	// Optional; without doc comments every description is empty.
	void set_doc_comments(std::vector<std::pair<int, std::string>> comments);

	Program parse();

private:
	FunctionDecl parse_function();
	std::vector<Parameter> parse_parameters();

	StructDecl parse_struct();
	StructDecl parse_class();
	EnumDecl parse_enum();

	MatchPatternPtr parse_match_pattern();
	MatchPatternPtr parse_match_array_pattern();
	MatchPatternPtr parse_match_dictionary_pattern();
	bool parse_pattern_rest(const char* what);
	void parse_argument_list(std::vector<ExprPtr>& arguments, std::vector<std::string>& names);

	StmtPtr parse_statement();
	StmtPtr parse_statement_impl();
	StmtPtr parse_var_decl(bool is_const);
	bool at_property_accessor() const;
	void parse_property_accessors(VarDeclStmt& decl);
	void parse_one_property_accessor(VarDeclStmt& decl);
	StmtPtr parse_if_stmt();
	StmtPtr parse_while_stmt();
	StmtPtr parse_for_stmt();
	StmtPtr parse_match_stmt(bool is_switch);
	StmtPtr parse_return_stmt();
	StmtPtr parse_expr_or_assign_stmt();
	std::vector<StmtPtr> parse_block();
	std::vector<StmtPtr> parse_suite();
	std::vector<StmtPtr> parse_inline_suite();
	bool at_inline_suite_end() const;

	// Precedence climbing, loosest to tightest.
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
	ExprPtr parse_node_path();
	ExprPtr parse_lambda();

	// Node positioned at `token`.
	template <typename Node, typename... Args>
	static std::unique_ptr<Node> make_at(const Token& token, Args&&... args) {
		auto node = std::make_unique<Node>(std::forward<Args>(args)...);
		node->line = token.line;
		node->column = token.column;
		return node;
	}

	// Node positioned where an existing expression starts.
	template <typename Node, typename... Args>
	static std::unique_ptr<Node> make_like(const Expr& like, Args&&... args) {
		const int line = like.line;
		const int column = like.column;
		auto node = std::make_unique<Node>(std::forward<Args>(args)...);
		node->line = line;
		node->column = column;
		return node;
	}

	// Reads position before move; factored to avoid repeating at each call site.
	static ExprPtr make_binary(ExprPtr left, BinaryExpr::Op op, ExprPtr right);
	// Null when the target cannot be read twice without side effects.
	static ExprPtr clone_lvalue(const Expr* expr);

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
	// Reports at a position already consumed, rather than at peek().
	void error(const std::string& message, int line, int column);
	void skip_newlines();
	void consume_statement_end(const std::string& message);

	std::string parse_type_hint();
	std::string parse_type_name();
	std::string parse_return_type();
	void skip_type_arguments();
	bool parse_attribute(ExportHint& hint, bool* is_onready = nullptr);
	bool m_saw_tool = false;
	void hoist_onready_initializers(Program& program);
	std::vector<ExportArgument> parse_attribute_arguments();
	SignalDecl parse_signal();
	int64_t fold_enum_value(const Expr* expr, const EnumDecl& decl, const Token& start);
	static bool holds_engine_constant(const Expr* expr);
	std::string doc_comment_above(int p_line) const;

	std::vector<Token> m_tokens;
	std::unordered_map<int, std::string> m_doc_comments; // line -> ## text
	size_t m_current = 0;
	// Tracks `await` in the current function body.
	bool m_saw_await = false;
	int m_inline_suite_depth = 0;
};

} // namespace gdscript
