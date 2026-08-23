#include "token.h"
#include <sstream>

namespace gdscript {

const char* token_type_name(TokenType type) {
	switch (type) {
		case TokenType::IDENTIFIER: return "IDENTIFIER";
		case TokenType::INTEGER: return "INTEGER";
		case TokenType::FLOAT: return "FLOAT";
		case TokenType::STRING: return "STRING";
		case TokenType::STRING_NAME: return "STRING_NAME";
		case TokenType::NODE_PATH: return "NODE_PATH";
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
		case TokenType::EXTENDS: return "EXTENDS";
		case TokenType::STRUCT: return "STRUCT";
		case TokenType::TRUE: return "TRUE";
		case TokenType::FALSE: return "FALSE";
		case TokenType::NULL_VAL: return "NULL";
		case TokenType::MATCH: return "MATCH";
		case TokenType::SWITCH: return "SWITCH";
		case TokenType::IS: return "IS";
		case TokenType::STATIC: return "STATIC";
		case TokenType::ENUM: return "ENUM";
		case TokenType::CLASS_NAME: return "CLASS_NAME";
		case TokenType::SIGNAL: return "SIGNAL";
		case TokenType::AWAIT: return "AWAIT";
		case TokenType::AS: return "AS";
		case TokenType::PLUS: return "PLUS";
		case TokenType::MINUS: return "MINUS";
		case TokenType::MULTIPLY: return "MULTIPLY";
		case TokenType::POWER: return "POWER";
		case TokenType::DIVIDE: return "DIVIDE";
		case TokenType::MODULO: return "MODULO";
		case TokenType::ASSIGN: return "ASSIGN";
		case TokenType::PLUS_ASSIGN: return "PLUS_ASSIGN";
		case TokenType::MINUS_ASSIGN: return "MINUS_ASSIGN";
		case TokenType::MULTIPLY_ASSIGN: return "MULTIPLY_ASSIGN";
		case TokenType::DIVIDE_ASSIGN: return "DIVIDE_ASSIGN";
		case TokenType::MODULO_ASSIGN: return "MODULO_ASSIGN";
		case TokenType::POWER_ASSIGN: return "POWER_ASSIGN";
		case TokenType::BIT_AND: return "BIT_AND";
		case TokenType::BIT_OR: return "BIT_OR";
		case TokenType::BIT_XOR: return "BIT_XOR";
		case TokenType::BIT_NOT: return "BIT_NOT";
		case TokenType::SHIFT_LEFT: return "SHIFT_LEFT";
		case TokenType::SHIFT_RIGHT: return "SHIFT_RIGHT";
		case TokenType::BIT_AND_ASSIGN: return "BIT_AND_ASSIGN";
		case TokenType::BIT_OR_ASSIGN: return "BIT_OR_ASSIGN";
		case TokenType::BIT_XOR_ASSIGN: return "BIT_XOR_ASSIGN";
		case TokenType::SHIFT_LEFT_ASSIGN: return "SHIFT_LEFT_ASSIGN";
		case TokenType::SHIFT_RIGHT_ASSIGN: return "SHIFT_RIGHT_ASSIGN";
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
		case TokenType::LBRACE: return "LBRACE";
		case TokenType::RBRACE: return "RBRACE";
		case TokenType::COLON: return "COLON";
		case TokenType::COMMA: return "COMMA";
		case TokenType::SEMICOLON: return "SEMICOLON";
		case TokenType::DOT: return "DOT";
		case TokenType::DOT_DOT: return "DOT_DOT";
		case TokenType::AT: return "AT";
		case TokenType::DOLLAR: return "DOLLAR";
		case TokenType::NEWLINE: return "NEWLINE";
		case TokenType::INDENT: return "INDENT";
		case TokenType::DEDENT: return "DEDENT";
		case TokenType::EOF_TOKEN: return "EOF";
		case TokenType::INVALID: return "INVALID";
		default: return "UNKNOWN";
	}
}

std::string Token::describe() const {
	switch (type) {
		case TokenType::NEWLINE:   return "end of line";
		case TokenType::INDENT:    return "an indented block";
		case TokenType::DEDENT:    return "the end of a block";
		case TokenType::EOF_TOKEN: return "end of file";
		case TokenType::STRING:    return "a string";
		case TokenType::STRING_NAME: return "a StringName";
		case TokenType::NODE_PATH: return "a NodePath";
		case TokenType::INVALID:   return "an unrecognized token";
		default: break;
	}
	if (lexeme.empty()) {
		return token_type_name(type);
	}
	return "'" + lexeme + "'";
}

std::string Token::to_string() const {
	std::ostringstream oss;
	oss << token_type_name(type) << " '" << lexeme << "' at " << line << ":" << column;
	return oss.str();
}

} // namespace gdscript
