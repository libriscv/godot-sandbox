#pragma once
#include <cstdint>
#include <string>
#include <variant>

namespace gdscript {

enum class TokenType {
	// Literals
	IDENTIFIER,
	INTEGER,
	FLOAT,
	STRING,
	STRING_NAME, // &"name"
	NODE_PATH,   // ^"a/b"

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
	BREAKPOINT,
	CONTINUE,
	PASS,
	EXTENDS,
	STRUCT,
	TRAIT,
	TRAIT_NAME,
	USES,
	TRUE,
	FALSE,
	NULL_VAL,
	MATCH,
	SWITCH,
	IS,
	AS,
	STATIC,
	ENUM,
	CLASS,
	CLASS_NAME,
	SIGNAL,
	AWAIT,

	// Operators
	PLUS,        // +
	MINUS,       // -
	MULTIPLY,    // *
	POWER,       // **
	DIVIDE,      // /
	MODULO,      // %
	ASSIGN,      // =
	PLUS_ASSIGN,    // +=
	MINUS_ASSIGN,   // -=
	MULTIPLY_ASSIGN, // *=
	DIVIDE_ASSIGN,  // /=
	MODULO_ASSIGN,  // %=
	POWER_ASSIGN,   // **=
	BIT_AND,        // &
	BIT_OR,         // |
	BIT_XOR,        // ^
	BIT_NOT,        // ~
	SHIFT_LEFT,     // <<
	SHIFT_RIGHT,    // >>
	BIT_AND_ASSIGN,     // &=
	BIT_OR_ASSIGN,      // |=
	BIT_XOR_ASSIGN,     // ^=
	SHIFT_LEFT_ASSIGN,  // <<=
	SHIFT_RIGHT_ASSIGN, // >>=
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
	LBRACE,      // {
	RBRACE,      // }
	COLON,       // :
	COMMA,       // ,
	SEMICOLON,   // ;
	DOT,         // .
	DOT_DOT,     // .. (the rest of an array or dictionary pattern)
	AT,          // @
	DOLLAR,      // $ (node path sugar)
	QUESTION,    // ? (nullable type suffix)
	QUESTION_QUESTION, // ?? (null-coalescing)
	QUESTION_DOT,      // ?. (safe navigation)
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
	// User-facing name for error messages; synthetic tokens get a description.
	std::string describe() const;
};

const char* token_type_name(TokenType type);

} // namespace gdscript
