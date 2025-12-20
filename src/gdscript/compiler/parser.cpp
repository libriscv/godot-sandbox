#include "parser.h"
#include <stdexcept>
#include <sstream>

namespace gdscript {

Parser::Parser(std::vector<Token> tokens) : m_tokens(std::move(tokens)) {}

Program Parser::parse() {
	Program program;

	skip_newlines();

	while (!is_at_end()) {
		if (check(TokenType::FUNC)) {
			program.functions.push_back(parse_function());
		} else {
			error("Expected function declaration");
			synchronize();
		}
		skip_newlines();
	}

	return program;
}

FunctionDecl Parser::parse_function() {
	FunctionDecl func;
	Token func_token = consume(TokenType::FUNC, "Expected 'func'");
	func.line = func_token.line;
	func.column = func_token.column;

	Token name = consume(TokenType::IDENTIFIER, "Expected function name");
	func.name = name.lexeme;

	consume(TokenType::LPAREN, "Expected '(' after function name");
	func.parameters = parse_parameters();
	consume(TokenType::RPAREN, "Expected ')' after parameters");
	consume(TokenType::COLON, "Expected ':' after function signature");
	consume(TokenType::NEWLINE, "Expected newline after function signature");

	func.body = parse_block();

	return func;
}

std::vector<Parameter> Parser::parse_parameters() {
	std::vector<Parameter> params;

	if (check(TokenType::RPAREN)) {
		return params;
	}

	do {
		Token param_name = consume(TokenType::IDENTIFIER, "Expected parameter name");
		Parameter param;
		param.name = param_name.lexeme;
		params.push_back(param);

		if (!check(TokenType::RPAREN)) {
			if (!match(TokenType::COMMA)) {
				break;
			}
		}
	} while (!check(TokenType::RPAREN));

	return params;
}

std::vector<StmtPtr> Parser::parse_block() {
	std::vector<StmtPtr> statements;

	consume(TokenType::INDENT, "Expected indented block");

	while (!check(TokenType::DEDENT) && !is_at_end()) {
		skip_newlines();
		if (!check(TokenType::DEDENT) && !is_at_end()) {
			statements.push_back(parse_statement());
		}
	}

	consume(TokenType::DEDENT, "Expected dedent after block");

	return statements;
}

StmtPtr Parser::parse_statement() {
	skip_newlines();

	if (match(TokenType::VAR)) {
		return parse_var_decl();
	}
	if (match(TokenType::IF)) {
		return parse_if_stmt();
	}
	if (match(TokenType::WHILE)) {
		return parse_while_stmt();
	}
	if (match(TokenType::FOR)) {
		return parse_for_stmt();
	}
	if (match(TokenType::RETURN)) {
		return parse_return_stmt();
	}
	if (match(TokenType::BREAK)) {
		auto stmt = std::make_unique<BreakStmt>();
		consume(TokenType::NEWLINE, "Expected newline after 'break'");
		return stmt;
	}
	if (match(TokenType::CONTINUE)) {
		auto stmt = std::make_unique<ContinueStmt>();
		consume(TokenType::NEWLINE, "Expected newline after 'continue'");
		return stmt;
	}
	if (match(TokenType::PASS)) {
		auto stmt = std::make_unique<PassStmt>();
		consume(TokenType::NEWLINE, "Expected newline after 'pass'");
		return stmt;
	}

	return parse_expr_or_assign_stmt();
}

StmtPtr Parser::parse_var_decl() {
	Token name = consume(TokenType::IDENTIFIER, "Expected variable name");

	ExprPtr initializer = nullptr;
	if (match(TokenType::ASSIGN)) {
		initializer = parse_expression();
	}

	consume(TokenType::NEWLINE, "Expected newline after variable declaration");
	return std::make_unique<VarDeclStmt>(name.lexeme, std::move(initializer));
}

StmtPtr Parser::parse_if_stmt() {
	ExprPtr condition = parse_expression();
	consume(TokenType::COLON, "Expected ':' after if condition");
	consume(TokenType::NEWLINE, "Expected newline after ':'");

	std::vector<StmtPtr> then_branch = parse_block();
	std::vector<StmtPtr> else_branch;

	skip_newlines();

	// Handle elif as else { if }
	if (match(TokenType::ELIF)) {
		auto elif_stmt = parse_if_stmt();
		else_branch.push_back(std::move(elif_stmt));
	} else if (match(TokenType::ELSE)) {
		consume(TokenType::COLON, "Expected ':' after else");
		consume(TokenType::NEWLINE, "Expected newline after ':'");
		else_branch = parse_block();
	}

	return std::make_unique<IfStmt>(std::move(condition), std::move(then_branch), std::move(else_branch));
}

StmtPtr Parser::parse_while_stmt() {
	ExprPtr condition = parse_expression();
	consume(TokenType::COLON, "Expected ':' after while condition");
	consume(TokenType::NEWLINE, "Expected newline after ':'");

	std::vector<StmtPtr> body = parse_block();

	return std::make_unique<WhileStmt>(std::move(condition), std::move(body));
}

StmtPtr Parser::parse_for_stmt() {
	// for variable in iterable:
	Token var_name = consume(TokenType::IDENTIFIER, "Expected variable name in for loop");
	consume(TokenType::IN, "Expected 'in' after for loop variable");
	ExprPtr iterable = parse_expression();
	consume(TokenType::COLON, "Expected ':' after for loop iterable");
	consume(TokenType::NEWLINE, "Expected newline after ':'");

	std::vector<StmtPtr> body = parse_block();

	return std::make_unique<ForStmt>(var_name.lexeme, std::move(iterable), std::move(body));
}

StmtPtr Parser::parse_return_stmt() {
	ExprPtr value = nullptr;

	if (!check(TokenType::NEWLINE)) {
		value = parse_expression();
	}

	consume(TokenType::NEWLINE, "Expected newline after return statement");
	return std::make_unique<ReturnStmt>(std::move(value));
}

StmtPtr Parser::parse_expr_or_assign_stmt() {
	// Try to parse an identifier followed by '='
	if (check(TokenType::IDENTIFIER)) {
		size_t saved_pos = m_current;
		Token name = advance();

		if (match(TokenType::ASSIGN)) {
			ExprPtr value = parse_expression();
			consume(TokenType::NEWLINE, "Expected newline after assignment");
			return std::make_unique<AssignStmt>(name.lexeme, std::move(value));
		}

		// Handle compound assignments: +=, -=, *=, /=, %=
		// Convert them to: name = name op value
		if (match(TokenType::PLUS_ASSIGN)) {
			ExprPtr var_expr = std::make_unique<VariableExpr>(name.lexeme);
			ExprPtr value = parse_expression();
			ExprPtr add_expr = std::make_unique<BinaryExpr>(std::move(var_expr), BinaryExpr::Op::ADD, std::move(value));
			consume(TokenType::NEWLINE, "Expected newline after assignment");
			return std::make_unique<AssignStmt>(name.lexeme, std::move(add_expr));
		}

		if (match(TokenType::MINUS_ASSIGN)) {
			ExprPtr var_expr = std::make_unique<VariableExpr>(name.lexeme);
			ExprPtr value = parse_expression();
			ExprPtr sub_expr = std::make_unique<BinaryExpr>(std::move(var_expr), BinaryExpr::Op::SUB, std::move(value));
			consume(TokenType::NEWLINE, "Expected newline after assignment");
			return std::make_unique<AssignStmt>(name.lexeme, std::move(sub_expr));
		}

		if (match(TokenType::MULTIPLY_ASSIGN)) {
			ExprPtr var_expr = std::make_unique<VariableExpr>(name.lexeme);
			ExprPtr value = parse_expression();
			ExprPtr mul_expr = std::make_unique<BinaryExpr>(std::move(var_expr), BinaryExpr::Op::MUL, std::move(value));
			consume(TokenType::NEWLINE, "Expected newline after assignment");
			return std::make_unique<AssignStmt>(name.lexeme, std::move(mul_expr));
		}

		if (match(TokenType::DIVIDE_ASSIGN)) {
			ExprPtr var_expr = std::make_unique<VariableExpr>(name.lexeme);
			ExprPtr value = parse_expression();
			ExprPtr div_expr = std::make_unique<BinaryExpr>(std::move(var_expr), BinaryExpr::Op::DIV, std::move(value));
			consume(TokenType::NEWLINE, "Expected newline after assignment");
			return std::make_unique<AssignStmt>(name.lexeme, std::move(div_expr));
		}

		if (match(TokenType::MODULO_ASSIGN)) {
			ExprPtr var_expr = std::make_unique<VariableExpr>(name.lexeme);
			ExprPtr value = parse_expression();
			ExprPtr mod_expr = std::make_unique<BinaryExpr>(std::move(var_expr), BinaryExpr::Op::MOD, std::move(value));
			consume(TokenType::NEWLINE, "Expected newline after assignment");
			return std::make_unique<AssignStmt>(name.lexeme, std::move(mod_expr));
		}

		// Not an assignment, backtrack
		m_current = saved_pos;
	}

	// Parse as expression statement
	ExprPtr expr = parse_expression();
	consume(TokenType::NEWLINE, "Expected newline after expression");
	return std::make_unique<ExprStmt>(std::move(expr));
}

ExprPtr Parser::parse_expression() {
	return parse_or_expression();
}

ExprPtr Parser::parse_or_expression() {
	ExprPtr left = parse_and_expression();

	while (match(TokenType::OR)) {
		ExprPtr right = parse_and_expression();
		left = std::make_unique<BinaryExpr>(std::move(left), BinaryExpr::Op::OR, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_and_expression() {
	ExprPtr left = parse_equality();

	while (match(TokenType::AND)) {
		ExprPtr right = parse_equality();
		left = std::make_unique<BinaryExpr>(std::move(left), BinaryExpr::Op::AND, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_equality() {
	ExprPtr left = parse_comparison();

	while (match_one_of({TokenType::EQUAL, TokenType::NOT_EQUAL})) {
		Token op = previous();
		ExprPtr right = parse_comparison();

		BinaryExpr::Op bin_op = (op.type == TokenType::EQUAL) ? BinaryExpr::Op::EQ : BinaryExpr::Op::NEQ;
		left = std::make_unique<BinaryExpr>(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_comparison() {
	ExprPtr left = parse_term();

	while (match_one_of({TokenType::LESS, TokenType::LESS_EQUAL, TokenType::GREATER, TokenType::GREATER_EQUAL})) {
		Token op = previous();
		ExprPtr right = parse_term();

		BinaryExpr::Op bin_op;
		switch (op.type) {
			case TokenType::LESS: bin_op = BinaryExpr::Op::LT; break;
			case TokenType::LESS_EQUAL: bin_op = BinaryExpr::Op::LTE; break;
			case TokenType::GREATER: bin_op = BinaryExpr::Op::GT; break;
			case TokenType::GREATER_EQUAL: bin_op = BinaryExpr::Op::GTE; break;
			default: throw std::runtime_error("Invalid comparison operator");
		}

		left = std::make_unique<BinaryExpr>(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_term() {
	ExprPtr left = parse_factor();

	while (match_one_of({TokenType::PLUS, TokenType::MINUS})) {
		Token op = previous();
		ExprPtr right = parse_factor();

		BinaryExpr::Op bin_op = (op.type == TokenType::PLUS) ? BinaryExpr::Op::ADD : BinaryExpr::Op::SUB;
		left = std::make_unique<BinaryExpr>(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_factor() {
	ExprPtr left = parse_unary();

	while (match_one_of({TokenType::MULTIPLY, TokenType::DIVIDE, TokenType::MODULO})) {
		Token op = previous();
		ExprPtr right = parse_unary();

		BinaryExpr::Op bin_op;
		switch (op.type) {
			case TokenType::MULTIPLY: bin_op = BinaryExpr::Op::MUL; break;
			case TokenType::DIVIDE: bin_op = BinaryExpr::Op::DIV; break;
			case TokenType::MODULO: bin_op = BinaryExpr::Op::MOD; break;
			default: throw std::runtime_error("Invalid factor operator");
		}

		left = std::make_unique<BinaryExpr>(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_unary() {
	if (match_one_of({TokenType::MINUS, TokenType::NOT})) {
		Token op = previous();
		ExprPtr operand = parse_unary();

		UnaryExpr::Op un_op = (op.type == TokenType::MINUS) ? UnaryExpr::Op::NEG : UnaryExpr::Op::NOT;
		return std::make_unique<UnaryExpr>(un_op, std::move(operand));
	}

	return parse_call();
}

ExprPtr Parser::parse_call() {
	ExprPtr expr = parse_primary();

	while (true) {
		if (match(TokenType::LPAREN)) {
			// Function call
			std::vector<ExprPtr> arguments;

			if (!check(TokenType::RPAREN)) {
				do {
					arguments.push_back(parse_expression());
				} while (match(TokenType::COMMA));
			}

			consume(TokenType::RPAREN, "Expected ')' after arguments");

			// Check if this is a method call
			if (auto* var_expr = dynamic_cast<VariableExpr*>(expr.get())) {
				// Local function call
				std::string func_name = var_expr->name;
				expr.release(); // We're replacing it
				expr = std::make_unique<CallExpr>(func_name, std::move(arguments));
			} else {
				error("Invalid call expression");
			}
		} else if (match(TokenType::DOT)) {
			// Member access
			Token member = consume(TokenType::IDENTIFIER, "Expected property or method name after '.'");

			if (match(TokenType::LPAREN)) {
				// Method call
				std::vector<ExprPtr> arguments;

				if (!check(TokenType::RPAREN)) {
					do {
						arguments.push_back(parse_expression());
					} while (match(TokenType::COMMA));
				}

				consume(TokenType::RPAREN, "Expected ')' after arguments");
				expr = std::make_unique<MemberCallExpr>(std::move(expr), member.lexeme, std::move(arguments));
			} else {
				// Property access
				expr = std::make_unique<MemberCallExpr>(std::move(expr), member.lexeme);
			}
		} else if (match(TokenType::LBRACKET)) {
			// Array indexing
			ExprPtr index = parse_expression();
			consume(TokenType::RBRACKET, "Expected ']' after index");
			expr = std::make_unique<IndexExpr>(std::move(expr), std::move(index));
		} else {
			break;
		}
	}

	return expr;
}

ExprPtr Parser::parse_primary() {
	if (match(TokenType::TRUE)) {
		return std::make_unique<LiteralExpr>(true);
	}
	if (match(TokenType::FALSE)) {
		return std::make_unique<LiteralExpr>(false);
	}
	if (match(TokenType::NULL_VAL)) {
		return LiteralExpr::null();
	}

	if (match(TokenType::INTEGER)) {
		Token num = previous();
		return std::make_unique<LiteralExpr>(std::get<int64_t>(num.value));
	}

	if (match(TokenType::FLOAT)) {
		Token num = previous();
		return std::make_unique<LiteralExpr>(std::get<double>(num.value));
	}

	if (match(TokenType::STRING)) {
		Token str = previous();
		return std::make_unique<LiteralExpr>(std::get<std::string>(str.value));
	}

	if (match(TokenType::IDENTIFIER)) {
		Token name = previous();
		return std::make_unique<VariableExpr>(name.lexeme);
	}

	if (match(TokenType::LPAREN)) {
		ExprPtr expr = parse_expression();
		consume(TokenType::RPAREN, "Expected ')' after expression");
		return expr;
	}

	error("Expected expression");
	return nullptr;
}

bool Parser::match(TokenType type) {
	if (check(type)) {
		advance();
		return true;
	}
	return false;
}

bool Parser::match_one_of(std::initializer_list<TokenType> types) {
	for (TokenType type : types) {
		if (match(type)) {
			return true;
		}
	}
	return false;
}

bool Parser::check(TokenType type) const {
	if (is_at_end()) return false;
	return peek().type == type;
}

Token Parser::advance() {
	if (!is_at_end()) m_current++;
	return previous();
}

Token Parser::peek() const {
	return m_tokens[m_current];
}

Token Parser::previous() const {
	return m_tokens[m_current - 1];
}

bool Parser::is_at_end() const {
	return peek().type == TokenType::EOF_TOKEN;
}

Token Parser::consume(TokenType type, const std::string& message) {
	if (check(type)) return advance();

	error(message + " but got " + peek().to_string());
	return peek();
}

void Parser::synchronize() {
	advance();

	while (!is_at_end()) {
		if (previous().type == TokenType::NEWLINE) return;

		switch (peek().type) {
			case TokenType::FUNC:
			case TokenType::VAR:
			case TokenType::IF:
			case TokenType::WHILE:
			case TokenType::RETURN:
				return;
			default:
				advance();
		}
	}
}

void Parser::error(const std::string& message) {
	Token token = peek();
	std::ostringstream oss;
	oss << "Parse error at line " << token.line << ", column " << token.column << ": " << message;
	throw std::runtime_error(oss.str());
}

void Parser::skip_newlines() {
	while (match(TokenType::NEWLINE)) {
		// Skip
	}
}

} // namespace gdscript
