#include "token.h"
#include <sstream>

namespace gdscript {

const char* token_type_name(TokenType type) {
	switch (type) {
		case TokenType::IDENTIFIER: return "IDENTIFIER";
		case TokenType::INTEGER: return "INTEGER";
		case TokenType::FLOAT: return "FLOAT";
		case TokenType::STRING: return "STRING";
		case TokenType::FUNC: return "FUNC";
		case TokenType::VAR: return "VAR";
		case TokenType::RETURN: return "RETURN";
		case TokenType::IF: return "IF";
		case TokenType::ELSE: return "ELSE";
		case TokenType::ELIF: return "ELIF";
		case TokenType::FOR: return "FOR";
		case TokenType::IN: return "IN";
		case TokenType::WHILE: return "WHILE";
		case TokenType::BREAK: return "BREAK";
		case TokenType::CONTINUE: return "CONTINUE";
		case TokenType::PASS: return "PASS";
		case TokenType::TRUE: return "TRUE";
		case TokenType::FALSE: return "FALSE";
		case TokenType::NULL_VAL: return "NULL";
		case TokenType::PLUS: return "PLUS";
		case TokenType::MINUS: return "MINUS";
		case TokenType::MULTIPLY: return "MULTIPLY";
		case TokenType::DIVIDE: return "DIVIDE";
		case TokenType::MODULO: return "MODULO";
		case TokenType::ASSIGN: return "ASSIGN";
		case TokenType::PLUS_ASSIGN: return "PLUS_ASSIGN";
		case TokenType::MINUS_ASSIGN: return "MINUS_ASSIGN";
		case TokenType::MULTIPLY_ASSIGN: return "MULTIPLY_ASSIGN";
		case TokenType::DIVIDE_ASSIGN: return "DIVIDE_ASSIGN";
		case TokenType::MODULO_ASSIGN: return "MODULO_ASSIGN";
		case TokenType::EQUAL: return "EQUAL";
		case TokenType::NOT_EQUAL: return "NOT_EQUAL";
		case TokenType::LESS: return "LESS";
		case TokenType::LESS_EQUAL: return "LESS_EQUAL";
		case TokenType::GREATER: return "GREATER";
		case TokenType::GREATER_EQUAL: return "GREATER_EQUAL";
		case TokenType::AND: return "AND";
		case TokenType::OR: return "OR";
		case TokenType::NOT: return "NOT";
		case TokenType::LPAREN: return "LPAREN";
		case TokenType::RPAREN: return "RPAREN";
		case TokenType::LBRACKET: return "LBRACKET";
		case TokenType::RBRACKET: return "RBRACKET";
		case TokenType::COLON: return "COLON";
		case TokenType::COMMA: return "COMMA";
		case TokenType::DOT: return "DOT";
		case TokenType::NEWLINE: return "NEWLINE";
		case TokenType::INDENT: return "INDENT";
		case TokenType::DEDENT: return "DEDENT";
		case TokenType::EOF_TOKEN: return "EOF";
		case TokenType::INVALID: return "INVALID";
		default: return "UNKNOWN";
	}
}

std::string Token::to_string() const {
	std::ostringstream oss;
	oss << token_type_name(type) << " '" << lexeme << "' at " << line << ":" << column;
	return oss.str();
}

} // namespace gdscript
