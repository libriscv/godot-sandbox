#pragma once
#include "token.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace gdscript {

class Lexer {
public:
	explicit Lexer(std::string source);

	std::vector<Token> tokenize();

private:
	void scan_token();
	void scan_string();
	void scan_number();
	void scan_identifier();
	void handle_indent();

	// Bracket nesting, which decides whether a newline ends a statement.
	void push_bracket(char closer);
	void pop_bracket(char closer);
	static char opener_for(char closer);

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
	std::vector<int> m_indent_stack; // Track indentation levels

	size_t m_start = 0;
	size_t m_current = 0;
	int m_line = 1;
	int m_column = 1;
	bool m_at_line_start = true;
	// Unclosed (), [] and {}, innermost last. While non-empty, newlines are
	// swallowed, which is what lets an argument list or literal span lines -- and
	// what makes an unclosed bracket run to EOF, so each records where it opened.
	struct OpenBracket {
		char closer;
		int line;
		int column;
	};
	std::vector<OpenBracket> m_open_brackets;

	static const std::unordered_map<std::string, TokenType> keywords;
};

} // namespace gdscript
