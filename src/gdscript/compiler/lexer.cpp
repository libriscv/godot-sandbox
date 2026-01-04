#include "lexer.h"
#include "compiler_exception.h"
#include <cctype>
#include <stdexcept>

namespace gdscript {

const std::unordered_map<std::string, TokenType> Lexer::keywords = {
	{"func", TokenType::FUNC},
	{"var", TokenType::VAR},
	{"const", TokenType::CONST},
	{"return", TokenType::RETURN},
	{"if", TokenType::IF},
	{"else", TokenType::ELSE},
	{"elif", TokenType::ELIF},
	{"for", TokenType::FOR},
	{"in", TokenType::IN},
	{"while", TokenType::WHILE},
	{"break", TokenType::BREAK},
	{"continue", TokenType::CONTINUE},
	{"pass", TokenType::PASS},
	{"extends", TokenType::EXTENDS},
	{"true", TokenType::TRUE},
	{"false", TokenType::FALSE},
	{"null", TokenType::NULL_VAL},
	{"and", TokenType::AND},
	{"or", TokenType::OR},
	{"not", TokenType::NOT},
};

Lexer::Lexer(std::string source) : m_source(std::move(source)) {
	m_indent_stack.push_back(0); // Start with zero indentation
}

std::vector<Token> Lexer::tokenize() {
	while (!is_at_end()) {
		m_start = m_current;
		scan_token();
	}

	// Add remaining dedents at end of file
	while (m_indent_stack.size() > 1) {
		m_indent_stack.pop_back();
		add_token(TokenType::DEDENT);
	}

	add_token(TokenType::EOF_TOKEN);
	return m_tokens;
}

void Lexer::scan_token() {
	// Handle indentation at line start
	if (m_at_line_start) {
		handle_indent();
		return;
	}

	char c = advance();

	switch (c) {
		case ' ':
		case '\r':
		case '\t':
			// Skip whitespace (except at line start)
			break;

		case '\n':
			add_token(TokenType::NEWLINE);
			m_line++;
			m_column = 1;
			m_at_line_start = true;
			break;

		case '#':
			// Comment - skip to end of line
			while (peek() != '\n' && !is_at_end()) advance();
			break;

		case '(': add_token(TokenType::LPAREN); break;
		case ')': add_token(TokenType::RPAREN); break;
		case '[': add_token(TokenType::LBRACKET); break;
		case ']': add_token(TokenType::RBRACKET); break;
		case '{': add_token(TokenType::LBRACE); break;
		case '}': add_token(TokenType::RBRACE); break;
		case ':': add_token(TokenType::COLON); break;
		case ',': add_token(TokenType::COMMA); break;
		case '.': add_token(TokenType::DOT); break;
		case '@': add_token(TokenType::AT); break;
		case '+': add_token(match('=') ? TokenType::PLUS_ASSIGN : TokenType::PLUS); break;
		case '-': add_token(match('=') ? TokenType::MINUS_ASSIGN : TokenType::MINUS); break;
		case '*': add_token(match('=') ? TokenType::MULTIPLY_ASSIGN : TokenType::MULTIPLY); break;
		case '/': add_token(match('=') ? TokenType::DIVIDE_ASSIGN : TokenType::DIVIDE); break;
		case '%': add_token(match('=') ? TokenType::MODULO_ASSIGN : TokenType::MODULO); break;

		case '=':
			add_token(match('=') ? TokenType::EQUAL : TokenType::ASSIGN);
			break;

		case '!':
			if (match('=')) {
				add_token(TokenType::NOT_EQUAL);
			} else {
				error("Unexpected character '!'");
			}
			break;

		case '<':
			add_token(match('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
			break;

		case '>':
			add_token(match('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER);
			break;

		case '"':
		case '\'':
			scan_string();
			break;

		default:
			if (is_digit(c)) {
				scan_number();
			} else if (is_alpha(c)) {
				scan_identifier();
			} else {
				error("Unexpected character");
			}
			break;
	}
}

void Lexer::handle_indent() {
	int indent_level = 0;

	while (!is_at_end() && (peek() == ' ' || peek() == '\t')) {
		if (peek() == '\t') {
			indent_level += 4; // Tab counts as 4 spaces
		} else {
			indent_level += 1;
		}
		advance();
	}

	// Skip blank lines and comments
	if (is_at_end() || peek() == '\n' || peek() == '#') {
		m_at_line_start = false;
		return;
	}

	m_at_line_start = false;

	int current_indent = m_indent_stack.back();

	if (indent_level > current_indent) {
		m_indent_stack.push_back(indent_level);
		add_token(TokenType::INDENT);
	} else if (indent_level < current_indent) {
		while (m_indent_stack.size() > 1 && m_indent_stack.back() > indent_level) {
			m_indent_stack.pop_back();
			add_token(TokenType::DEDENT);
		}

		if (m_indent_stack.back() != indent_level) {
			error("Inconsistent indentation");
		}
	}
}

void Lexer::scan_string() {
	char quote = m_source[m_current - 1];
	std::string value;

	while (!is_at_end() && peek() != quote) {
		if (peek() == '\n') {
			m_line++;
			m_column = 0;
		} else if (peek() == '\\') {
			advance();
			if (!is_at_end()) {
				char escaped = advance();
				switch (escaped) {
					case 'n': value += '\n'; break;
					case 't': value += '\t'; break;
					case 'r': value += '\r'; break;
					case '\\': value += '\\'; break;
					case '"': value += '"'; break;
					case '\'': value += '\''; break;
					default: value += escaped; break;
				}
				continue;
			}
		}
		value += advance();
	}

	if (is_at_end()) {
		error("Unterminated string");
		return;
	}

	advance(); // Closing quote
	add_token(TokenType::STRING, value);
}

void Lexer::scan_number() {
	while (is_digit(peek())) advance();

	bool is_float = false;

	// Look for decimal point
	if (peek() == '.' && is_digit(peek_next())) {
		is_float = true;
		advance(); // Consume '.'
		while (is_digit(peek())) advance();
	}

	std::string num_str = m_source.substr(m_start, m_current - m_start);

	if (is_float) {
		double d = std::stod(num_str);
		add_token(TokenType::FLOAT, d);
	} else {
		int64_t i = std::stoll(num_str);
		add_token(TokenType::INTEGER, i);
	}
}

void Lexer::scan_identifier() {
	while (is_alphanumeric(peek())) advance();

	std::string text = m_source.substr(m_start, m_current - m_start);

	auto it = keywords.find(text);
	TokenType type = (it != keywords.end()) ? it->second : TokenType::IDENTIFIER;

	add_token(type);
}

char Lexer::advance() {
	m_column++;
	return m_source[m_current++];
}

char Lexer::peek() const {
	if (is_at_end()) return '\0';
	return m_source[m_current];
}

char Lexer::peek_next() const {
	if (m_current + 1 >= m_source.length()) return '\0';
	return m_source[m_current + 1];
}

bool Lexer::match(char expected) {
	if (is_at_end()) return false;
	if (m_source[m_current] != expected) return false;

	m_current++;
	m_column++;
	return true;
}

bool Lexer::is_at_end() const {
	return m_current >= m_source.length();
}

bool Lexer::is_digit(char c) const {
	return c >= '0' && c <= '9';
}

bool Lexer::is_alpha(char c) const {
	return (c >= 'a' && c <= 'z') ||
	       (c >= 'A' && c <= 'Z') ||
	       c == '_';
}

bool Lexer::is_alphanumeric(char c) const {
	return is_alpha(c) || is_digit(c);
}

void Lexer::add_token(TokenType type) {
	std::string text = m_source.substr(m_start, m_current - m_start);
	m_tokens.emplace_back(type, text, m_line, m_column);
}

void Lexer::add_token(TokenType type, int64_t value) {
	std::string text = m_source.substr(m_start, m_current - m_start);
	Token token(type, text, m_line, m_column);
	token.value = value;
	m_tokens.push_back(token);
}

void Lexer::add_token(TokenType type, double value) {
	std::string text = m_source.substr(m_start, m_current - m_start);
	Token token(type, text, m_line, m_column);
	token.value = value;
	m_tokens.push_back(token);
}

void Lexer::add_token(TokenType type, const std::string& value) {
	std::string text = m_source.substr(m_start, m_current - m_start);
	Token token(type, text, m_line, m_column);
	token.value = value;
	m_tokens.push_back(token);
}

void Lexer::error(const std::string& message) {
	throw CompilerException(ErrorType::LEXER_ERROR, message, m_line, m_column);
}

} // namespace gdscript
