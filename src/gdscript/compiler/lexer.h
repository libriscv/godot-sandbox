#pragma once
#include "token.h"
#include <vector>
#include <string>
#include <cstdint>
#include <utility>
#include <unordered_map>

namespace gdscript {

class Lexer {
public:
	explicit Lexer(std::string source);

	std::vector<Token> tokenize();

	// ## doc comments paired with line numbers. Valid after tokenize().
	const std::vector<std::pair<int, std::string>> &doc_comments() const { return m_doc_comments; }

private:
	void scan_token();
	void scan_string(TokenType type = TokenType::STRING, bool raw = false);
	uint32_t scan_hex_escape(int hex_len);
	void append_unicode_escape(std::string& value, int hex_len);
	void scan_number();
	void scan_identifier();
	void handle_indent();

	void push_bracket(char closer);
	void pop_bracket(char closer);
	static char opener_for(char closer);
	bool lambda_layout_active() const;

	char advance();
	char peek() const;
	char peek_next() const;
	char peek_at(size_t ahead) const;
	bool match(char expected);
	bool is_at_end() const;
	bool is_digit(char c) const;
	bool is_hex_digit(char c) const;
	bool is_alpha(char c) const;
	bool is_alphanumeric(char c) const;

	void add_token(TokenType type);
	void add_token(TokenType type, int64_t value);
	void add_token(TokenType type, double value);
	void add_token(TokenType type, const std::string& value);

	[[noreturn]] void error(const std::string& message);
	[[noreturn]] void error_at(const std::string& message, int line, int column);

	std::string m_source;
	std::vector<Token> m_tokens;
	std::vector<int> m_indent_stack;
	std::vector<std::pair<int, std::string>> m_doc_comments;

	size_t m_start = 0;
	size_t m_current = 0;
	int m_line = 1;
	int m_column = 1;
	bool m_at_line_start = true;
	// Unclosed brackets; while non-empty, newlines are swallowed.
	struct OpenBracket {
		char closer;
		int line;
		int column;
	};
	std::vector<OpenBracket> m_open_brackets;
	// A multiline lambda can open an indented suite even while an enclosing
	// call/Array/Dictionary keeps brackets open. Each entry is the indentation
	// of the line containing `func()` and the enclosing bracket depth.
	struct LambdaLayout {
		int base_indent;
		size_t bracket_depth;
	};
	std::vector<LambdaLayout> m_lambda_layouts;
	bool m_lambda_signature = false;
	bool m_lambda_parameters_closed = false;
	bool m_lambda_suite_may_start = false;
	size_t m_lambda_parameter_depth = 0;
	int m_lambda_base_indent = 0;

	static const std::unordered_map<std::string, TokenType> keywords;
};

} // namespace gdscript
