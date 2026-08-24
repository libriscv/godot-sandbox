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
	{"breakpoint", TokenType::BREAKPOINT},
	{"continue", TokenType::CONTINUE},
	{"pass", TokenType::PASS},
	{"extends", TokenType::EXTENDS},
	{"struct", TokenType::STRUCT},
	{"true", TokenType::TRUE},
	{"false", TokenType::FALSE},
	{"null", TokenType::NULL_VAL},
	{"and", TokenType::AND},
	{"or", TokenType::OR},
	{"not", TokenType::NOT},
	{"match", TokenType::MATCH},
	{"switch", TokenType::SWITCH},
	{"is", TokenType::IS},
	{"static", TokenType::STATIC},
	{"enum", TokenType::ENUM},
	{"class", TokenType::CLASS},
	{"class_name", TokenType::CLASS_NAME},
	{"signal", TokenType::SIGNAL},
	{"await", TokenType::AWAIT},
	{"as", TokenType::AS},
};

Lexer::Lexer(std::string source) : m_source(std::move(source)) {
	m_indent_stack.push_back(0);
}

std::vector<Token> Lexer::tokenize() {
	while (!is_at_end()) {
		m_start = m_current;
		scan_token();
	}

	// Report unclosed bracket at the opener, not at EOF.
	if (!m_open_brackets.empty()) {
		const OpenBracket& open = m_open_brackets.back();
		error_at(std::string("Unclosed '") + opener_for(open.closer) + "', expected '" +
			open.closer + "' before the end of the file", open.line, open.column);
	}

	while (m_indent_stack.size() > 1) {
		m_indent_stack.pop_back();
		add_token(TokenType::DEDENT);
	}

	add_token(TokenType::EOF_TOKEN);
	return m_tokens;
}

char Lexer::opener_for(char closer) {
	switch (closer) {
		case ')': return '(';
		case ']': return '[';
		default:  return '{';
	}
}

void Lexer::push_bracket(char closer) {
	m_open_brackets.push_back({closer, m_line, m_column - 1});
}

void Lexer::pop_bracket(char closer) {
	if (!m_open_brackets.empty() && m_open_brackets.back().closer == closer) {
		m_open_brackets.pop_back();
	}
}

void Lexer::scan_token() {
	if (m_at_line_start) {
		handle_indent();
		return;
	}

	char c = advance();

	switch (c) {
		case ' ':
		case '\r':
		case '\t':
			break;

		case '\n':
			// Inside brackets, newlines are layout, not statement ends.
			if (m_open_brackets.empty()) {
				add_token(TokenType::NEWLINE);
				m_at_line_start = true;
			}
			m_line++;
			m_column = 1;
			break;

		case '\\':
			while (peek() == ' ' || peek() == '\r' || peek() == '\t') advance();
			if (peek() != '\n') {
				error("Expected end of line after '\\' line continuation");
			}
			advance();
			m_line++;
			m_column = 1;
			break;

		case '#': {
			// '##' is a doc comment retained for editor descriptions.
			const int comment_line = m_line;
			const bool is_doc = peek() == '#';
			if (is_doc) {
				advance();
			}
			const size_t text_start = m_current;
			while (peek() != '\n' && !is_at_end()) advance();
			if (is_doc) {
				std::string text = m_source.substr(text_start, m_current - text_start);
				// Strip one leading space (marker spelling, not content).
				if (!text.empty() && text.front() == ' ') {
					text.erase(text.begin());
				}
				while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
					text.pop_back();
				}
				m_doc_comments.emplace_back(comment_line, std::move(text));
			}
			break;
		}

		case '(': push_bracket(')'); add_token(TokenType::LPAREN); break;
		case '[': push_bracket(']'); add_token(TokenType::LBRACKET); break;
		case '{': push_bracket('}'); add_token(TokenType::LBRACE); break;
		// Stray closers passed through; the parser reports them.
		case ')': pop_bracket(')'); add_token(TokenType::RPAREN); break;
		case ']': pop_bracket(']'); add_token(TokenType::RBRACKET); break;
		case '}': pop_bracket('}'); add_token(TokenType::RBRACE); break;
		case ':': add_token(TokenType::COLON); break;
		case ',': add_token(TokenType::COMMA); break;
		case ';': add_token(TokenType::SEMICOLON); break;
		case '.': add_token(match('.') ? TokenType::DOT_DOT : TokenType::DOT); break;
		case '@': add_token(TokenType::AT); break;
		case '$': add_token(TokenType::DOLLAR); break;
		case '+': add_token(match('=') ? TokenType::PLUS_ASSIGN : TokenType::PLUS); break;
		case '-': add_token(match('=') ? TokenType::MINUS_ASSIGN : TokenType::MINUS); break;
		case '*':
			// '**' before '*=': `a **= b` is POWER_ASSIGN, not MULTIPLY + stray.
			if (match('*')) {
				add_token(match('=') ? TokenType::POWER_ASSIGN : TokenType::POWER);
			} else {
				add_token(match('=') ? TokenType::MULTIPLY_ASSIGN : TokenType::MULTIPLY);
			}
			break;
		case '/': add_token(match('=') ? TokenType::DIVIDE_ASSIGN : TokenType::DIVIDE); break;
		case '%': add_token(match('=') ? TokenType::MODULO_ASSIGN : TokenType::MODULO); break;

		case '=':
			add_token(match('=') ? TokenType::EQUAL : TokenType::ASSIGN);
			break;

		case '!':
			add_token(match('=') ? TokenType::NOT_EQUAL : TokenType::NOT);
			break;

		case '<':
			if (match('<')) {
				add_token(match('=') ? TokenType::SHIFT_LEFT_ASSIGN : TokenType::SHIFT_LEFT);
			} else {
				add_token(match('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
			}
			break;

		case '>':
			if (match('>')) {
				add_token(match('=') ? TokenType::SHIFT_RIGHT_ASSIGN : TokenType::SHIFT_RIGHT);
			} else {
				add_token(match('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER);
			}
			break;

		case '&':
			if (peek() == '"' || peek() == '\'') {
				advance();
				scan_string(TokenType::STRING_NAME);
			} else if (match('&')) {
				add_token(TokenType::AND);
			} else {
				add_token(match('=') ? TokenType::BIT_AND_ASSIGN : TokenType::BIT_AND);
			}
			break;

		case '|':
			if (match('|')) {
				add_token(TokenType::OR);
			} else {
				add_token(match('=') ? TokenType::BIT_OR_ASSIGN : TokenType::BIT_OR);
			}
			break;

		case '^':
			if (peek() == '"' || peek() == '\'') {
				advance();
				scan_string(TokenType::NODE_PATH);
			} else {
				add_token(match('=') ? TokenType::BIT_XOR_ASSIGN : TokenType::BIT_XOR);
			}
			break;

		case '~':
			add_token(TokenType::BIT_NOT);
			break;

		case '"':
		case '\'':
			scan_string();
			break;

		default:
			if (is_digit(c)) {
				scan_number();
			} else if (c == 'r' && (peek() == '"' || peek() == '\'')) {
				// Raw string literal: no escape processing.
				advance();
				scan_string(TokenType::STRING, true);
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
			indent_level += 4;
		} else {
			indent_level += 1;
		}
		advance();
	}

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

static void append_utf8(std::string& out, uint32_t cp) {
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

// Caller has consumed the backslash and letter.
uint32_t Lexer::scan_hex_escape(int hex_len) {
	uint32_t value = 0;
	for (int i = 0; i < hex_len; i++) {
		if (is_at_end()) {
			error("Unterminated string");
		}
		const char c = peek();
		if (!is_hex_digit(c)) {
			error("Invalid hexadecimal digit in unicode escape sequence");
		}
		advance();
		const uint32_t digit = (c >= '0' && c <= '9')   ? uint32_t(c - '0')
		                       : (c >= 'a' && c <= 'f') ? uint32_t(c - 'a' + 10)
		                                                : uint32_t(c - 'A' + 10);
		value = (value << 4) | digit;
	}
	return value;
}

void Lexer::append_unicode_escape(std::string& value, int hex_len) {
	uint32_t cp = scan_hex_escape(hex_len);

	if (cp >= 0xD800 && cp <= 0xDBFF) {
		if (peek() != '\\' || (peek_next() != 'u' && peek_next() != 'U')) {
			error("Invalid UTF-16 sequence in string, unpaired lead surrogate");
		}
		advance();
		const char letter = advance();
		const uint32_t trail = scan_hex_escape(letter == 'u' ? 4 : 6);
		if (trail < 0xDC00 || trail > 0xDFFF) {
			error("Invalid UTF-16 sequence in string, unpaired lead surrogate");
		}
		cp = 0x10000 + ((cp - 0xD800) << 10) + (trail - 0xDC00);
	} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
		error("Invalid UTF-16 sequence in string, unpaired trail surrogate");
	}

	// Godot silently substitutes U+FFFD; reject at compile time instead.
	if (cp > 0x10FFFF) {
		error("Invalid unicode codepoint in escape sequence");
	}
	append_utf8(value, cp);
}

// `type`: STRING, STRING_NAME (&"..."), or NODE_PATH (^"...").
// `raw`: r"..." — backslash literal, still escapes the quote terminator.
void Lexer::scan_string(TokenType type, bool raw) {
	const char quote = m_source[m_current - 1];
	// Report unterminated strings at the opening quote, not at EOF.
	const int open_line = m_line;
	const int open_column = m_column - 1;

	// Triple-quoted strings may span lines; single-quoted ones cannot.
	bool triple = false;
	if (peek() == quote && peek_next() == quote) {
		advance();
		advance();
		triple = true;
	}

	std::string value;
	bool terminated = false;
	while (!is_at_end()) {
		const char c = peek();
		if (c == quote) {
			if (!triple) {
				terminated = true;
				break;
			}
			if (peek_next() == quote && peek_at(2) == quote) {
				terminated = true;
				break;
			}
		} else if (c == '\n') {
			if (!triple) {
				break;
			}
			m_line++;
			m_column = 0;
		} else if (c == '\\') {
			advance();
			if (is_at_end()) {
				break;
			}
			if (raw) {
				value += '\\';
				value += advance();
				continue;
			}
			const char escaped = advance();
			switch (escaped) {
				case 'a': value += '\a'; break;
				case 'b': value += '\b'; break;
				case 'f': value += '\f'; break;
				case 'n': value += '\n'; break;
				case 'r': value += '\r'; break;
				case 't': value += '\t'; break;
				case 'v': value += '\v'; break;
				case '\\': value += '\\'; break;
				case '"': value += '"'; break;
				case '\'': value += '\''; break;
				case 'u':
				case 'U':
					append_unicode_escape(value, escaped == 'u' ? 4 : 6);
					break;
				case '\r':
					// CRLF continuation; a lone CR is not one.
					if (peek() != '\n') {
						error("Invalid escape in string");
					}
					advance();
					m_line++;
					m_column = 0;
					break;
				case '\n':
					// Line continuation; nothing emitted.
					m_line++;
					m_column = 0;
					break;
				default: error("Invalid escape in string"); break;
			}
			continue;
		}
		value += advance();
	}

	if (!terminated) {
		error_at("Unterminated string", open_line, open_column);
	}

	advance();
	if (triple) {
		advance();
		advance();
	}
	add_token(type, value);
}

void Lexer::scan_number() {
	if (m_source[m_start] == '0' && (peek() == 'x' || peek() == 'X' ||
	                                 peek() == 'b' || peek() == 'B')) {
		const bool hex = (peek() == 'x' || peek() == 'X');
		advance();

		std::string digits;
		while (!is_at_end()) {
			const char c = peek();
			if (c == '_') {
				advance();
				continue;
			}
			if (hex ? is_hex_digit(c) : (c == '0' || c == '1')) {
				digits += advance();
				continue;
			}
			break;
		}

		if (digits.empty()) {
			error(hex ? "Expected hexadecimal digits after '0x'"
			          : "Expected binary digits after '0b'");
			return;
		}

		// Unsigned parse so top-bit-set literals wrap instead of throwing.
		int64_t value = 0;
		try {
			value = static_cast<int64_t>(std::stoull(digits, nullptr, hex ? 16 : 2));
		} catch (const std::exception&) {
			error("Integer literal out of range");
			return;
		}
		add_token(TokenType::INTEGER, value);
		return;
	}

	std::string num_str(1, m_source[m_start]);
	bool is_float = false;

	auto consume_digits = [&]() {
		while (!is_at_end()) {
			if (peek() == '_') {
				advance();
				continue;
			}
			if (!is_digit(peek())) break;
			num_str += advance();
		}
	};

	consume_digits();

	// Only when a digit follows '.', so `1.method()` stays a method call.
	if (peek() == '.' && is_digit(peek_next())) {
		is_float = true;
		num_str += advance();
		consume_digits();
	}

	if ((peek() == 'e' || peek() == 'E') &&
	    (is_digit(peek_next()) ||
	     ((peek_next() == '+' || peek_next() == '-') && is_digit(peek_at(2))))) {
		is_float = true;
		num_str += advance();
		if (peek() == '+' || peek() == '-') {
			num_str += advance();
		}
		consume_digits();
	}

	try {
		if (is_float) {
			add_token(TokenType::FLOAT, std::stod(num_str));
		} else {
			add_token(TokenType::INTEGER, static_cast<int64_t>(std::stoull(num_str)));
		}
	} catch (const std::exception&) {
		error("Numeric literal out of range");
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
	return peek_at(1);
}

char Lexer::peek_at(size_t ahead) const {
	if (m_current + ahead >= m_source.length()) return '\0';
	return m_source[m_current + ahead];
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

bool Lexer::is_hex_digit(char c) const {
	return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
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
	error_at(message, m_line, m_column);
}

void Lexer::error_at(const std::string& message, int line, int column) {
	throw CompilerException(ErrorType::LEXER_ERROR, message, line, column);
}

} // namespace gdscript
