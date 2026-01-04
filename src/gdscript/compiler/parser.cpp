#include "parser.h"
#include "compiler_exception.h"
#include <stdexcept>
#include <sstream>

namespace gdscript {

Parser::Parser(std::vector<Token> tokens) : m_tokens(std::move(tokens)) {}

Program Parser::parse() {
	Program program;

	skip_newlines();

	while (!is_at_end()) {
		if (check(TokenType::EXTENDS)) {
			// Parse and ignore extends statement
			advance(); // consume 'extends'
			consume(TokenType::IDENTIFIER, "Expected class name after 'extends'");
			// Skip any newlines after extends
			skip_newlines();
		} else if (check(TokenType::AT)) {
			// Parse attribute (e.g., @export)
			bool is_export = parse_attribute();

			// Skip newlines after attribute
			skip_newlines();

			// After attribute, we expect a var declaration
			if (check(TokenType::VAR)) {
				advance(); // consume 'var'
				auto var_decl = parse_var_decl(false);
				if (auto* decl = dynamic_cast<VarDeclStmt*>(var_decl.get())) {
					VarDeclStmt global_decl(decl->name, std::move(decl->initializer), decl->is_const);
					global_decl.type_hint = decl->type_hint;
					global_decl.is_property = is_export;
					program.globals.push_back(std::move(global_decl));
				}
			} else {
				error("Expected variable declaration after attribute");
				synchronize();
			}
		} else if (check(TokenType::VAR)) {
			// Parse global var declaration
			advance(); // consume 'var'
			auto var_decl = parse_var_decl(false);
			if (auto* decl = dynamic_cast<VarDeclStmt*>(var_decl.get())) {
				VarDeclStmt global_decl(decl->name, std::move(decl->initializer), decl->is_const);
				global_decl.type_hint = decl->type_hint;
				program.globals.push_back(std::move(global_decl));
			}
		} else if (check(TokenType::CONST)) {
			// Parse global const declaration
			advance(); // consume 'const'
			auto const_decl = parse_var_decl(true);
			if (auto* decl = dynamic_cast<VarDeclStmt*>(const_decl.get())) {
				VarDeclStmt global_decl(decl->name, std::move(decl->initializer), decl->is_const);
				global_decl.type_hint = decl->type_hint;
				program.globals.push_back(std::move(global_decl));
			}
		} else if (check(TokenType::FUNC)) {
			program.functions.push_back(parse_function());
		} else {
			error("Expected function or variable declaration");
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

	// Parse optional return type (e.g., "-> void")
	func.return_type = parse_return_type();

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

		// Parse optional type hint (e.g., ": int")
		param.type_hint = parse_type_hint();

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

	skip_newlines();
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
		return parse_var_decl(false);
	}
	if (match(TokenType::CONST)) {
		return parse_var_decl(true);
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

StmtPtr Parser::parse_var_decl(bool is_const) {
	Token name = consume(TokenType::IDENTIFIER, "Expected variable name");

	// Parse optional type hint (e.g., ": int")
	std::string type_hint = parse_type_hint();

	ExprPtr initializer = nullptr;
	if (match(TokenType::ASSIGN)) {
		initializer = parse_expression();
	} else if (is_const) {
		error("Const variables must have an initializer");
	}

	consume(TokenType::NEWLINE, "Expected newline after variable declaration");
	auto stmt = std::make_unique<VarDeclStmt>(name.lexeme, std::move(initializer), is_const);
	stmt->type_hint = type_hint;
	return stmt;
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
	// Parse a postfix expression (which can be identifier or indexed expression)
	size_t saved_pos = m_current;
	ExprPtr lhs = parse_call();

	// Check if it's an assignment
	if (match(TokenType::ASSIGN)) {
		ExprPtr value = parse_expression();
		consume(TokenType::NEWLINE, "Expected newline after assignment");

		// Check if lhs is a simple variable, indexed expression, or property access
		if (auto* var_expr = dynamic_cast<VariableExpr*>(lhs.get())) {
			// Simple variable assignment: x = value
			return std::make_unique<AssignStmt>(var_expr->name, std::move(value));
		} else if (dynamic_cast<IndexExpr*>(lhs.get())) {
			// Indexed assignment: arr[0] = value
			return std::make_unique<AssignStmt>(std::move(lhs), std::move(value));
		} else if (auto* member_expr = dynamic_cast<MemberCallExpr*>(lhs.get())) {
			// Property assignment: obj.prop = value
			// Verify it's a property access (not a method call)
			if (member_expr->is_method_call) {
				throw CompilerException::parser_error("Cannot assign to method call", lhs->line, lhs->column);
			}
			return std::make_unique<AssignStmt>(std::move(lhs), std::move(value));
		} else {
			throw CompilerException::parser_error("Invalid assignment target", lhs->line, lhs->column);
		}
	}

	// Handle compound assignments for simple variables only
	if (auto* var_expr = dynamic_cast<VariableExpr*>(lhs.get())) {
		std::string name = var_expr->name;

		if (match(TokenType::PLUS_ASSIGN)) {
			ExprPtr var_ref = std::make_unique<VariableExpr>(name);
			ExprPtr rhs = parse_expression();
			ExprPtr add_expr = std::make_unique<BinaryExpr>(std::move(var_ref), BinaryExpr::Op::ADD, std::move(rhs));
			consume(TokenType::NEWLINE, "Expected newline after assignment");
			return std::make_unique<AssignStmt>(name, std::move(add_expr));
		}

		if (match(TokenType::MINUS_ASSIGN)) {
			ExprPtr var_ref = std::make_unique<VariableExpr>(name);
			ExprPtr rhs = parse_expression();
			ExprPtr sub_expr = std::make_unique<BinaryExpr>(std::move(var_ref), BinaryExpr::Op::SUB, std::move(rhs));
			consume(TokenType::NEWLINE, "Expected newline after assignment");
			return std::make_unique<AssignStmt>(name, std::move(sub_expr));
		}

		if (match(TokenType::MULTIPLY_ASSIGN)) {
			ExprPtr var_ref = std::make_unique<VariableExpr>(name);
			ExprPtr rhs = parse_expression();
			ExprPtr mul_expr = std::make_unique<BinaryExpr>(std::move(var_ref), BinaryExpr::Op::MUL, std::move(rhs));
			consume(TokenType::NEWLINE, "Expected newline after assignment");
			return std::make_unique<AssignStmt>(name, std::move(mul_expr));
		}

		if (match(TokenType::DIVIDE_ASSIGN)) {
			ExprPtr var_ref = std::make_unique<VariableExpr>(name);
			ExprPtr rhs = parse_expression();
			ExprPtr div_expr = std::make_unique<BinaryExpr>(std::move(var_ref), BinaryExpr::Op::DIV, std::move(rhs));
			consume(TokenType::NEWLINE, "Expected newline after assignment");
			return std::make_unique<AssignStmt>(name, std::move(div_expr));
		}

		if (match(TokenType::MODULO_ASSIGN)) {
			ExprPtr var_ref = std::make_unique<VariableExpr>(name);
			ExprPtr rhs = parse_expression();
			ExprPtr mod_expr = std::make_unique<BinaryExpr>(std::move(var_ref), BinaryExpr::Op::MOD, std::move(rhs));
			consume(TokenType::NEWLINE, "Expected newline after assignment");
			return std::make_unique<AssignStmt>(name, std::move(mod_expr));
		}
	}

	// Not an assignment, treat as expression statement
	consume(TokenType::NEWLINE, "Expected newline after expression");
	return std::make_unique<ExprStmt>(std::move(lhs));
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
			default: throw CompilerException::parser_error("Invalid comparison operator", op.line, op.column);
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
			default: throw CompilerException::parser_error("Invalid factor operator", op.line, op.column);
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
				expr = std::make_unique<CallExpr>(func_name, std::move(arguments));
			} else {
				error("Invalid call expression");
			}
		} else if (match(TokenType::DOT)) {
			// Member access
			Token member = consume(TokenType::IDENTIFIER, "Expected property or method name after '.'");

			if (match(TokenType::LPAREN)) {
				// Method call (including argument-less methods like obj.method())
				std::vector<ExprPtr> arguments;

				if (!check(TokenType::RPAREN)) {
					do {
						arguments.push_back(parse_expression());
					} while (match(TokenType::COMMA));
				}

				consume(TokenType::RPAREN, "Expected ')' after arguments");
				expr = std::make_unique<MemberCallExpr>(std::move(expr), member.lexeme, std::move(arguments), true);
			} else {
				// Property access (no parentheses)
				expr = std::unique_ptr<MemberCallExpr>(new MemberCallExpr(std::move(expr), member.lexeme, {}, false));
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

	if (match(TokenType::LBRACKET)) {
		// Array literal: [1, 2, 3]
		std::vector<ExprPtr> elements;

		if (!check(TokenType::RBRACKET)) {
			do {
				elements.push_back(parse_expression());
			} while (match(TokenType::COMMA));
		}

		consume(TokenType::RBRACKET, "Expected ']' after array elements");
		return std::make_unique<ArrayLiteralExpr>(std::move(elements));
	}

	if (match(TokenType::LBRACE)) {
		// Dictionary literal: {"key": "value", "num": 42} or {key: "value", num: 42}
		std::vector<std::pair<ExprPtr, ExprPtr>> elements;

		if (!check(TokenType::RBRACE)) {
			do {
				// Check if the key is an identifier (for shorthand {name: value} syntax)
				ExprPtr key;
				if (match(TokenType::IDENTIFIER)) {
					// Convert identifier to string literal
					Token identifier = previous();
					key = std::make_unique<LiteralExpr>(identifier.lexeme);
				} else {
					// Otherwise parse as a normal expression
					key = parse_expression();
				}

				consume(TokenType::COLON, "Expected ':' after dictionary key");
				ExprPtr value = parse_expression();
				elements.push_back({std::move(key), std::move(value)});
			} while (match(TokenType::COMMA));
		}

		consume(TokenType::RBRACE, "Expected '}' after dictionary elements");
		return std::make_unique<DictionaryLiteralExpr>(std::move(elements));
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
	throw CompilerException(ErrorType::PARSER_ERROR, message, token.line, token.column);
}

void Parser::skip_newlines() {
	while (match(TokenType::NEWLINE)) {
		// Skip
	}
}

std::string Parser::parse_type_hint() {
	// Type hints are optional and follow the pattern ": type"
	// We just consume the colon and the following identifier/type
	if (match(TokenType::COLON)) {
		// Look ahead to see if there's a type name
		// For now, we just capture whatever comes after the colon
		// This could be a simple identifier like "int" or a more complex type like "Array[int]"
		if (check(TokenType::IDENTIFIER)) {
			Token type_token = consume(TokenType::IDENTIFIER, "Expected type name");
			return type_token.lexeme;
		}
		// If there's no identifier immediately after, return empty
		// This allows for things like "var x:" which we'll just treat as no type hint
	}
	return "";
}

std::string Parser::parse_return_type() {
	// Return types are optional and follow the pattern "-> type"
	// We need to check for the arrow token (which would be MINUS + GREATER)
	// For now, let's check if we have MINUS followed by GREATER
	size_t saved_pos = m_current;

	if (match(TokenType::MINUS)) {
		if (match(TokenType::GREATER)) {
			// We found ->, now parse the type
			if (check(TokenType::IDENTIFIER)) {
				Token type_token = consume(TokenType::IDENTIFIER, "Expected return type");
				return type_token.lexeme;
			}
			return "";  // Found -> but no type
		}
		// Not a return type, rewind
		m_current = saved_pos;
	}

	return "";
}

bool Parser::parse_attribute() {
	// Parse attribute annotations like @export
	// Currently only @export is supported
	consume(TokenType::AT, "Expected '@' for attribute");

	if (match(TokenType::IDENTIFIER)) {
		Token attr_name = previous();
		if (attr_name.lexeme == "export") {
			return true; // This is an @export attribute
		} else {
			error("Unknown attribute: @" + attr_name.lexeme);
		}
	} else {
		error("Expected identifier after '@'");
	}

	return false;
}

} // namespace gdscript
