#pragma once
#include <string>
#include <variant>

namespace gdscript {

enum class TokenType {
	// Literals
	IDENTIFIER,
	INTEGER,
	FLOAT,
	STRING,

	// Keywords
	FUNC,
	VAR,
	CONST,
	RETURN,
	IF,
	ELSE,
	ELIF,
	FOR,
	IN,
	WHILE,
	BREAK,
	CONTINUE,
	PASS,
	TRUE,
	FALSE,
	NULL_VAL,

	// Operators
	PLUS,        // +
	MINUS,       // -
	MULTIPLY,    // *
	DIVIDE,      // /
	MODULO,      // %
	ASSIGN,      // =
	PLUS_ASSIGN,    // +=
	MINUS_ASSIGN,   // -=
	MULTIPLY_ASSIGN, // *=
	DIVIDE_ASSIGN,  // /=
	MODULO_ASSIGN,  // %=
	EQUAL,       // ==
	NOT_EQUAL,   // !=
	LESS,        // <
	LESS_EQUAL,  // <=
	GREATER,     // >
	GREATER_EQUAL, // >=
	AND,         // and
	OR,          // or
	NOT,         // not

	// Delimiters
	LPAREN,      // (
	RPAREN,      // )
	LBRACKET,    // [
	RBRACKET,    // ]
	COLON,       // :
	COMMA,       // ,
	DOT,         // .
	NEWLINE,
	INDENT,
	DEDENT,

	// Special
	EOF_TOKEN,
	INVALID
};

struct Token {
	TokenType type;
	std::string lexeme;
	std::variant<int64_t, double, std::string> value;
	int line;
	int column;

	Token() : type(TokenType::INVALID), line(0), column(0) {}
	Token(TokenType t, std::string lex, int l, int c)
		: type(t), lexeme(std::move(lex)), line(l), column(c) {}

	bool is_type(TokenType t) const { return type == t; }
	bool is_one_of(TokenType t1, TokenType t2) const { return type == t1 || type == t2; }

	template<typename... Types>
	bool is_one_of(TokenType first, Types... rest) const {
		return type == first || is_one_of(rest...);
	}

	std::string to_string() const;
};

const char* token_type_name(TokenType type);

} // namespace gdscript
