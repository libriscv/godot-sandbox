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

	char advance();
	char peek() const;
	char peek_next() const;
	bool match(char expected);
	bool is_at_end() const;
	bool is_digit(char c) const;
	bool is_alpha(char c) const;
	bool is_alphanumeric(char c) const;

	void add_token(TokenType type);
	void add_token(TokenType type, int64_t value);
	void add_token(TokenType type, double value);
	void add_token(TokenType type, const std::string& value);

	void error(const std::string& message);

	std::string m_source;
	std::vector<Token> m_tokens;
	std::vector<int> m_indent_stack; // Track indentation levels

	size_t m_start = 0;
	size_t m_current = 0;
	int m_line = 1;
	int m_column = 1;
	bool m_at_line_start = true;

	static const std::unordered_map<std::string, TokenType> keywords;
};

} // namespace gdscript
