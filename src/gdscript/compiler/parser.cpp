#include "parser.h"
#include "compiler_exception.h"
#include <stdexcept>
#include <sstream>

namespace gdscript {

Parser::Parser(std::vector<Token> tokens) : m_tokens(std::move(tokens)) {}

void Parser::set_doc_comments(std::vector<std::pair<int, std::string>> comments) {
	m_doc_comments.clear();
	m_doc_comments.reserve(comments.size());
	for (auto &comment : comments) {
		m_doc_comments.emplace(comment.first, std::move(comment.second));
	}
}

std::string Parser::doc_comment_above(int p_line) const {
	// The block is the contiguous run of doc-comment lines directly above the
	// declaration; any other line, blank included, ends it. Collected bottom-up,
	// then reversed into source order.
	std::vector<const std::string *> lines;
	for (int line = p_line - 1; line > 0; line--) {
		auto it = m_doc_comments.find(line);
		if (it == m_doc_comments.end()) {
			break;
		}
		lines.push_back(&it->second);
	}

	std::string text;
	for (size_t i = lines.size(); i-- > 0;) {
		if (!text.empty()) {
			text += '\n';
		}
		text += *lines[i];
	}
	return text;
}

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
					global_decl.line = decl->line;
					global_decl.column = decl->column;
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
				global_decl.line = decl->line;
				global_decl.column = decl->column;
				program.globals.push_back(std::move(global_decl));
			}
		} else if (check(TokenType::CONST)) {
			// Parse global const declaration
			advance(); // consume 'const'
			auto const_decl = parse_var_decl(true);
			if (auto* decl = dynamic_cast<VarDeclStmt*>(const_decl.get())) {
				VarDeclStmt global_decl(decl->name, std::move(decl->initializer), decl->is_const);
				global_decl.type_hint = decl->type_hint;
				global_decl.line = decl->line;
				global_decl.column = decl->column;
				program.globals.push_back(std::move(global_decl));
			}
		} else if (check(TokenType::STRUCT)) {
			program.structs.push_back(parse_struct());
		} else if (check(TokenType::ENUM)) {
			program.enums.push_back(parse_enum());
		} else if (check(TokenType::CLASS_NAME)) {
			// The registered script name is a project fact, not code: read and
			// dropped, like `extends`.
			advance();
			consume(TokenType::IDENTIFIER, "Expected a class name after 'class_name'");
			skip_newlines();
		} else if (check(TokenType::STATIC) || check(TokenType::FUNC)) {
			// `static` is accepted and ignored: with no class instance, every
			// emitted function is already a plain function.
			match(TokenType::STATIC);
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
	func.doc_comment = doc_comment_above(func_token.line);

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

EnumDecl Parser::parse_enum() {
	EnumDecl decl;
	Token enum_token = consume(TokenType::ENUM, "Expected 'enum'");
	decl.line = enum_token.line;
	decl.column = enum_token.column;

	// `enum Name { ... }` names the set; `enum { ... }` puts its members in file
	// scope.
	if (check(TokenType::IDENTIFIER)) {
		decl.name = advance().lexeme;
	}

	consume(TokenType::LBRACE, "Expected '{' after 'enum'");

	// The lexer swallows newlines inside the braces, so a member list may span
	// lines with no handling here.
	int64_t next_value = 0;
	while (!check(TokenType::RBRACE) && !is_at_end()) {
		Token member_name = consume(TokenType::IDENTIFIER, "Expected an enum member name");

		EnumDecl::Member member;
		member.name = member_name.lexeme;
		member.line = member_name.line;
		member.column = member_name.column;

		if (match(TokenType::ASSIGN)) {
			// Only a signed literal integer. GDScript allows a constant
			// expression; anything not reducible to a number now would have to
			// exist at run time, which an enum member cannot.
			const bool negative = match(TokenType::MINUS);
			if (!negative) {
				match(TokenType::PLUS);
			}
			Token value_token = consume(TokenType::INTEGER,
				"An enum member's value has to be an integer literal");
			const int64_t magnitude = std::get<int64_t>(value_token.value);
			next_value = negative ? -magnitude : magnitude;
		}

		member.value = next_value;
		next_value++;

		if (decl.find_member(member.name) != nullptr) {
			throw CompilerException::parser_error(
				"Enum member '" + member.name + "' is declared more than once",
				member.line, member.column);
		}
		decl.members.push_back(std::move(member));

		if (!match(TokenType::COMMA)) {
			break;
		}
	}

	consume(TokenType::RBRACE, "Expected '}' after enum members");
	return decl;
}

StructDecl Parser::parse_struct() {
	StructDecl decl;
	Token struct_token = consume(TokenType::STRUCT, "Expected 'struct'");
	decl.line = struct_token.line;
	decl.column = struct_token.column;

	Token name = consume(TokenType::IDENTIFIER, "Expected struct name");
	decl.name = name.lexeme;

	consume(TokenType::COLON, "Expected ':' after struct name");
	consume(TokenType::NEWLINE, "Expected newline after struct declaration");

	skip_newlines();
	consume(TokenType::INDENT, "Expected indented block after 'struct " + decl.name + ":'");

	while (!check(TokenType::DEDENT) && !is_at_end()) {
		skip_newlines();
		if (check(TokenType::DEDENT) || is_at_end()) {
			break;
		}

		// 'pass' stands in for a body, which for a struct means no fields.
		if (match(TokenType::PASS)) {
			consume_statement_end("Expected newline after 'pass'");
			continue;
		}

		// A struct body is a list of fields and nothing else. Anything else --
		// a method, a nested struct -- would silently not be part of the
		// Dictionary the struct lowers to, so it is rejected rather than
		// skipped.
		Token var_token = consume(TokenType::VAR,
			"A struct body holds only field declarations");

		StructField field;
		field.line = var_token.line;
		field.column = var_token.column;

		Token field_name = consume(TokenType::IDENTIFIER, "Expected field name");
		field.name = field_name.lexeme;
		field.type_hint = parse_type_hint();

		if (match(TokenType::ASSIGN)) {
			field.default_value = parse_expression();
		}
		consume(TokenType::NEWLINE, "Expected newline after field declaration");

		if (decl.find_field(field.name) != nullptr) {
			throw CompilerException::parser_error(
				"Struct '" + decl.name + "' declares field '" + field.name + "' more than once",
				field.line, field.column);
		}
		decl.fields.push_back(std::move(field));
	}

	consume(TokenType::DEDENT, "Expected dedent after struct body");
	return decl;
}

void Parser::parse_argument_list(std::vector<ExprPtr>& arguments, std::vector<std::string>& names) {
	if (!check(TokenType::RPAREN)) {
		bool seen_named = false;
		do {
			std::string name;
			// `name = value` is a named argument. Two tokens of lookahead
			// separate it from an expression starting with an identifier: only
			// '=' can follow, and '==' lexes as its own token, so a comparison
			// is never mistaken for one.
			if (check(TokenType::IDENTIFIER) && peek_ahead(1).type == TokenType::ASSIGN) {
				name = advance().lexeme;
				advance(); // consume '='
				seen_named = true;
			} else if (seen_named) {
				error("A positional argument cannot follow a named argument");
			}
			arguments.push_back(parse_expression());
			names.push_back(std::move(name));
			if (!match(TokenType::COMMA)) {
				break;
			}
		} while (!check(TokenType::RPAREN)); // a trailing ',' before ')' is allowed
	}

	consume(TokenType::RPAREN, "Expected ')' after arguments");
}

std::vector<Parameter> Parser::parse_parameters() {
	std::vector<Parameter> params;

	if (check(TokenType::RPAREN)) {
		return params;
	}

	bool seen_default = false;
	do {
		Token param_name = consume(TokenType::IDENTIFIER, "Expected parameter name");
		Parameter param;
		param.name = param_name.lexeme;

		// Parse optional type hint (e.g., ": int")
		param.type_hint = parse_type_hint();

		// Parse optional default value (e.g., "= 5")
		if (match(TokenType::ASSIGN)) {
			param.default_value = parse_expression();
			seen_default = true;
		} else if (seen_default) {
			error("Parameter '" + param.name + "' without a default value cannot follow one with a default value");
		}

		params.push_back(std::move(param));

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
	// Statements are positioned here rather than at each of the eleven places
	// one is constructed, so that adding a statement kind cannot forget to.
	const Token start = peek();
	StmtPtr stmt = parse_statement_impl();
	if (stmt && stmt->line == 0) {
		stmt->line = start.line;
		stmt->column = start.column;
	}
	return stmt;
}

StmtPtr Parser::parse_statement_impl() {
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
	if (match(TokenType::MATCH)) {
		return parse_match_stmt();
	}
	if (match(TokenType::RETURN)) {
		return parse_return_stmt();
	}
	if (match(TokenType::BREAK)) {
		auto stmt = std::make_unique<BreakStmt>();
		consume_statement_end("Expected newline after 'break'");
		return stmt;
	}
	if (match(TokenType::CONTINUE)) {
		auto stmt = std::make_unique<ContinueStmt>();
		consume_statement_end("Expected newline after 'continue'");
		return stmt;
	}
	if (match(TokenType::PASS)) {
		auto stmt = std::make_unique<PassStmt>();
		consume_statement_end("Expected newline after 'pass'");
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

	consume_statement_end("Expected newline after variable declaration");
	auto stmt = make_at<VarDeclStmt>(name, name.lexeme, std::move(initializer), is_const);
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

StmtPtr Parser::parse_match_stmt() {
	// match <subject>:
	//     <pattern>[, <pattern>...] [when <condition>]:
	//         <body>
	//     _:
	//         <body>
	ExprPtr subject = parse_expression();
	consume(TokenType::COLON, "Expected ':' after match subject");
	consume(TokenType::NEWLINE, "Expected newline after ':'");

	skip_newlines();
	consume(TokenType::INDENT, "Expected indented block after 'match'");

	std::vector<MatchStmt::Branch> branches;

	while (!check(TokenType::DEDENT) && !is_at_end()) {
		skip_newlines();
		if (check(TokenType::DEDENT) || is_at_end()) {
			break;
		}

		MatchStmt::Branch branch;

		// A branch is a comma-separated pattern list; any one match takes it.
		do {
			branch.patterns.push_back(parse_match_pattern());
		} while (match(TokenType::COMMA) && !check(TokenType::COLON));

		// A binding must exist by the time the body runs. Next to another pattern
		// it would not: the other pattern may be the one that matched, leaving
		// nothing to name. GDScript rejects this for the same reason.
		if (branch.patterns.size() > 1) {
			for (const auto& pattern : branch.patterns) {
				if (pattern->binds()) {
					throw CompilerException::parser_error(
						"A pattern that binds a name cannot share an arm with other patterns",
						pattern->line, pattern->column);
				}
			}
		}

		// `when` is a contextual keyword -- an ordinary identifier everywhere
		// else -- so it is matched by lexeme, not by token type.
		if (check(TokenType::IDENTIFIER) && peek().lexeme == "when") {
			advance();
			branch.guard = parse_expression();
		}

		consume(TokenType::COLON, "Expected ':' after match pattern");
		consume(TokenType::NEWLINE, "Expected newline after ':'");
		branch.body = parse_block();

		branches.push_back(std::move(branch));
	}

	skip_newlines();
	consume(TokenType::DEDENT, "Expected dedent after match block");

	if (branches.empty()) {
		error("'match' requires at least one pattern");
	}

	return std::make_unique<MatchStmt>(std::move(subject), std::move(branches));
}

MatchPatternPtr Parser::parse_match_pattern() {
	auto pattern = std::make_unique<MatchPattern>();
	pattern->line = peek().line;
	pattern->column = peek().column;

	// '_' matches anything and binds nothing. An identifier everywhere else, so
	// it is matched by lexeme.
	if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
		advance();
		pattern->kind = MatchPattern::Kind::WILDCARD;
		return pattern;
	}

	// `var name` matches anything and binds it for the guard and the body.
	if (match(TokenType::VAR)) {
		Token name = consume(TokenType::IDENTIFIER, "Expected a name after 'var' in a pattern");
		pattern->kind = MatchPattern::Kind::BIND;
		pattern->name = name.lexeme;
		return pattern;
	}

	if (check(TokenType::LBRACKET)) {
		return parse_match_array_pattern();
	}
	if (check(TokenType::LBRACE)) {
		return parse_match_dictionary_pattern();
	}

	// Anything else is a value compared against the subject. The expression stops
	// at the ',' between patterns and the ':' ending the arm, both of which end
	// an expression anyway.
	pattern->kind = MatchPattern::Kind::VALUE;
	pattern->value = parse_expression();
	pattern->line = pattern->value->line;
	pattern->column = pattern->value->column;
	return pattern;
}

bool Parser::parse_pattern_rest(const char* what) {
	if (!match(TokenType::DOT_DOT)) {
		return false;
	}
	// `..` means "and the rest", so it is last: a second one, or anything after
	// it, has nothing left to describe.
	match(TokenType::COMMA);
	if (!check(TokenType::RBRACKET) && !check(TokenType::RBRACE)) {
		Token at = peek();
		throw CompilerException::parser_error(
			std::string("'..' has to be the last entry of ") + what, at.line, at.column);
	}
	return true;
}

MatchPatternPtr Parser::parse_match_array_pattern() {
	auto pattern = std::make_unique<MatchPattern>();
	Token open_bracket = consume(TokenType::LBRACKET, "Expected '[' to start an array pattern");
	pattern->kind = MatchPattern::Kind::ARRAY;
	pattern->line = open_bracket.line;
	pattern->column = open_bracket.column;

	while (!check(TokenType::RBRACKET) && !is_at_end()) {
		if (parse_pattern_rest("an array pattern")) {
			pattern->open = true;
			break;
		}
		pattern->elements.push_back(parse_match_pattern());
		if (!match(TokenType::COMMA)) {
			break;
		}
	}

	consume(TokenType::RBRACKET, "Expected ']' after an array pattern");
	return pattern;
}

MatchPatternPtr Parser::parse_match_dictionary_pattern() {
	auto pattern = std::make_unique<MatchPattern>();
	Token open_brace = consume(TokenType::LBRACE, "Expected '{' to start a dictionary pattern");
	pattern->kind = MatchPattern::Kind::DICTIONARY;
	pattern->line = open_brace.line;
	pattern->column = open_brace.column;

	while (!check(TokenType::RBRACE) && !is_at_end()) {
		if (parse_pattern_rest("a dictionary pattern")) {
			pattern->open = true;
			break;
		}

		MatchPattern::Entry entry;
		entry.key = parse_expression();
		// `{"key": <pattern>}` constrains the value; `{"key"}` only presence.
		if (match(TokenType::COLON)) {
			entry.value = parse_match_pattern();
		}
		pattern->entries.push_back(std::move(entry));

		if (!match(TokenType::COMMA)) {
			break;
		}
	}

	consume(TokenType::RBRACE, "Expected '}' after a dictionary pattern");
	return pattern;
}

StmtPtr Parser::parse_return_stmt() {
	ExprPtr value = nullptr;

	if (!check(TokenType::NEWLINE)) {
		value = parse_expression();
	}

	consume_statement_end("Expected newline after return statement");
	return std::make_unique<ReturnStmt>(std::move(value));
}

StmtPtr Parser::parse_expr_or_assign_stmt() {
	// Parse a postfix expression (which can be identifier or indexed expression)
	ExprPtr lhs = parse_call();

	// Check if it's an assignment
	if (match(TokenType::ASSIGN)) {
		ExprPtr value = parse_expression();
		consume_statement_end("Expected newline after assignment");

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

	// Handle compound assignments (x += 1, obj.field <<= 2, arr[i] **= 2, ...).
	//
	// `a op= b` becomes `a = a op b`, which needs a second copy of the target.
	// AST nodes are unique_ptr and do not clone, so clone_lvalue() rebuilds the
	// targets that can be rebuilt without evaluating anything twice. `f().x += 1`
	// is not one of them -- it would call f() twice -- so it stays an error.
	static const struct { TokenType token; BinaryExpr::Op op; } compound_ops[] = {
		{TokenType::PLUS_ASSIGN,        BinaryExpr::Op::ADD},
		{TokenType::MINUS_ASSIGN,       BinaryExpr::Op::SUB},
		{TokenType::MULTIPLY_ASSIGN,    BinaryExpr::Op::MUL},
		{TokenType::DIVIDE_ASSIGN,      BinaryExpr::Op::DIV},
		{TokenType::MODULO_ASSIGN,      BinaryExpr::Op::MOD},
		{TokenType::POWER_ASSIGN,       BinaryExpr::Op::POW},
		{TokenType::BIT_AND_ASSIGN,     BinaryExpr::Op::BIT_AND},
		{TokenType::BIT_OR_ASSIGN,      BinaryExpr::Op::BIT_OR},
		{TokenType::BIT_XOR_ASSIGN,     BinaryExpr::Op::BIT_XOR},
		{TokenType::SHIFT_LEFT_ASSIGN,  BinaryExpr::Op::SHL},
		{TokenType::SHIFT_RIGHT_ASSIGN, BinaryExpr::Op::SHR},
	};

	for (const auto& entry : compound_ops) {
		if (!check(entry.token)) {
			continue;
		}
		// Build the read before consuming the operator, so a target that cannot
		// be read twice is reported as itself, not as whatever follows.
		ExprPtr read = clone_lvalue(lhs.get());
		if (!read) {
			throw CompilerException::parser_error(
				"Invalid target for compound assignment", lhs->line, lhs->column);
		}
		advance(); // the 'op=' token

		ExprPtr rhs = parse_expression();
		ExprPtr combined = make_binary(std::move(read), entry.op, std::move(rhs));
		consume_statement_end("Expected newline after assignment");

		if (auto* var_expr = dynamic_cast<VariableExpr*>(lhs.get())) {
			return std::make_unique<AssignStmt>(var_expr->name, std::move(combined));
		}
		return std::make_unique<AssignStmt>(std::move(lhs), std::move(combined));
	}

	// Not an assignment, treat as expression statement
	consume_statement_end("Expected newline after expression");
	return std::make_unique<ExprStmt>(std::move(lhs));
}

ExprPtr Parser::clone_lvalue(const Expr* expr) {
	// Rebuild an assignment target so `a op= b` can read and write it. Only pure
	// lookups are rebuilt: re-evaluating a call, or an index that is itself a
	// call, would run it twice and give `a[f()] += 1` two different elements.
	if (auto* var = dynamic_cast<const VariableExpr*>(expr)) {
		return make_like<VariableExpr>(*expr, var->name);
	}
	if (auto* lit = dynamic_cast<const LiteralExpr*>(expr)) {
		auto copy = std::make_unique<LiteralExpr>(*lit);
		copy->line = expr->line;
		copy->column = expr->column;
		return copy;
	}
	if (auto* member = dynamic_cast<const MemberCallExpr*>(expr)) {
		if (member->is_method_call) {
			return nullptr;
		}
		ExprPtr object = clone_lvalue(member->object.get());
		if (!object) {
			return nullptr;
		}
		return make_like<MemberCallExpr>(*expr, std::move(object), member->member_name,
			std::vector<ExprPtr>{}, false);
	}
	if (auto* index = dynamic_cast<const IndexExpr*>(expr)) {
		ExprPtr object = clone_lvalue(index->object.get());
		ExprPtr subscript = clone_lvalue(index->index.get());
		if (!object || !subscript) {
			return nullptr;
		}
		return make_like<IndexExpr>(*expr, std::move(object), std::move(subscript));
	}
	return nullptr;
}

ExprPtr Parser::make_binary(ExprPtr left, BinaryExpr::Op op, ExprPtr right) {
	const int line = left->line;
	const int column = left->column;
	auto node = std::make_unique<BinaryExpr>(std::move(left), op, std::move(right));
	node->line = line;
	node->column = column;
	return node;
}

ExprPtr Parser::parse_expression() {
	ExprPtr expr = parse_ternary();

	// `as` is the loosest operator, so `a if c else b as int` casts the whole
	// conditional.
	while (match(TokenType::AS)) {
		const Token type_token = consume(TokenType::IDENTIFIER, "Expected a type name after 'as'");
		// For the built-in scalar types cast and constructor agree, so `x as int`
		// compiles as `int(x)`. For a class name they do not: a failed cast
		// yields null, which only the engine can decide. Rejecting here beats
		// reporting a call to an unknown function later.
		if (type_token.lexeme != "int" && type_token.lexeme != "float" &&
		    type_token.lexeme != "bool" && type_token.lexeme != "String") {
			throw CompilerException::parser_error(
				"'as " + type_token.lexeme + "' is not supported: 'as' is only available for"
				" int, float, bool and String", type_token.line, type_token.column);
		}
		std::vector<ExprPtr> arguments;
		arguments.push_back(std::move(expr));
		const Token& at = type_token;
		auto call = make_at<CallExpr>(at, type_token.lexeme, std::move(arguments));
		call->argument_names.resize(1);
		expr = std::move(call);
	}

	return expr;
}

ExprPtr Parser::parse_ternary() {
	// GDScript conditional expression: <true_value> if <condition> else <false_value>
	ExprPtr true_value = parse_or_expression();

	if (!match(TokenType::IF)) {
		return true_value;
	}

	ExprPtr condition = parse_or_expression();
	consume(TokenType::ELSE, "Expected 'else' in conditional expression");
	ExprPtr false_value = parse_ternary(); // Right-associative

	// "<true> if <cond> else <false>" starts at the true value.
	const Expr& start = *true_value;
	return make_like<TernaryExpr>(start, std::move(condition), std::move(true_value), std::move(false_value));
}

ExprPtr Parser::parse_or_expression() {
	ExprPtr left = parse_and_expression();

	while (match(TokenType::OR)) {
		ExprPtr right = parse_and_expression();
		left = make_binary(std::move(left), BinaryExpr::Op::OR, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_and_expression() {
	ExprPtr left = parse_not();

	while (match(TokenType::AND)) {
		ExprPtr right = parse_not();
		left = make_binary(std::move(left), BinaryExpr::Op::AND, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_not() {
	// `not` binds looser than everything below it, as in GDScript and Python:
	// `not a == b` is `not (a == b)`. It is the one unary operator that does not
	// sit with `-` and `~`, which bind tighter than the arithmetic.
	if (match(TokenType::NOT)) {
		Token op = previous();
		return make_at<UnaryExpr>(op, UnaryExpr::Op::NOT, parse_not());
	}
	return parse_inclusion();
}

ExprPtr Parser::parse_inclusion() {
	ExprPtr left = parse_equality();

	while (true) {
		// `not in` is two tokens. Only `in` may follow `not` here; anything else
		// is `not` applied to what comes next, which parses one level up. Decide
		// by lookahead, without consuming.
		bool negated = false;
		if (check(TokenType::NOT) && peek_ahead(1).type == TokenType::IN) {
			advance();
			advance();
			negated = true;
		} else if (!match(TokenType::IN)) {
			break;
		}

		ExprPtr right = parse_equality();
		left = make_binary(std::move(left), BinaryExpr::Op::IN, std::move(right));
		if (negated) {
			const Expr& start = *left;
			left = make_like<UnaryExpr>(start, UnaryExpr::Op::NOT, std::move(left));
		}
	}

	return left;
}

ExprPtr Parser::parse_equality() {
	ExprPtr left = parse_comparison();

	while (match_one_of({TokenType::EQUAL, TokenType::NOT_EQUAL})) {
		Token op = previous();
		ExprPtr right = parse_comparison();

		BinaryExpr::Op bin_op = (op.type == TokenType::EQUAL) ? BinaryExpr::Op::EQ : BinaryExpr::Op::NEQ;
		left = make_binary(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_comparison() {
	ExprPtr left = parse_bit_or();

	while (match_one_of({TokenType::LESS, TokenType::LESS_EQUAL, TokenType::GREATER, TokenType::GREATER_EQUAL})) {
		Token op = previous();
		ExprPtr right = parse_bit_or();

		BinaryExpr::Op bin_op;
		switch (op.type) {
			case TokenType::LESS: bin_op = BinaryExpr::Op::LT; break;
			case TokenType::LESS_EQUAL: bin_op = BinaryExpr::Op::LTE; break;
			case TokenType::GREATER: bin_op = BinaryExpr::Op::GT; break;
			case TokenType::GREATER_EQUAL: bin_op = BinaryExpr::Op::GTE; break;
			default: throw CompilerException::parser_error("Invalid comparison operator", op.line, op.column);
		}

		left = make_binary(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_bit_or() {
	ExprPtr left = parse_bit_xor();

	while (match(TokenType::BIT_OR)) {
		ExprPtr right = parse_bit_xor();
		left = make_binary(std::move(left), BinaryExpr::Op::BIT_OR, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_bit_xor() {
	ExprPtr left = parse_bit_and();

	while (match(TokenType::BIT_XOR)) {
		ExprPtr right = parse_bit_and();
		left = make_binary(std::move(left), BinaryExpr::Op::BIT_XOR, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_bit_and() {
	ExprPtr left = parse_shift();

	while (match(TokenType::BIT_AND)) {
		ExprPtr right = parse_shift();
		left = make_binary(std::move(left), BinaryExpr::Op::BIT_AND, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_shift() {
	ExprPtr left = parse_term();

	while (match_one_of({TokenType::SHIFT_LEFT, TokenType::SHIFT_RIGHT})) {
		Token op = previous();
		ExprPtr right = parse_term();

		BinaryExpr::Op bin_op = (op.type == TokenType::SHIFT_LEFT) ? BinaryExpr::Op::SHL : BinaryExpr::Op::SHR;
		left = make_binary(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_term() {
	ExprPtr left = parse_factor();

	while (match_one_of({TokenType::PLUS, TokenType::MINUS})) {
		Token op = previous();
		ExprPtr right = parse_factor();

		BinaryExpr::Op bin_op = (op.type == TokenType::PLUS) ? BinaryExpr::Op::ADD : BinaryExpr::Op::SUB;
		left = make_binary(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_factor() {
	ExprPtr left = parse_type_test();

	while (match_one_of({TokenType::MULTIPLY, TokenType::DIVIDE, TokenType::MODULO})) {
		Token op = previous();
		ExprPtr right = parse_type_test();

		BinaryExpr::Op bin_op;
		switch (op.type) {
			case TokenType::MULTIPLY: bin_op = BinaryExpr::Op::MUL; break;
			case TokenType::DIVIDE: bin_op = BinaryExpr::Op::DIV; break;
			case TokenType::MODULO: bin_op = BinaryExpr::Op::MOD; break;
			default: throw CompilerException::parser_error("Invalid factor operator", op.line, op.column);
		}

		left = make_binary(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_unary() {
	if (match_one_of({TokenType::MINUS, TokenType::BIT_NOT, TokenType::PLUS})) {
		Token op = previous();
		ExprPtr operand = parse_unary();

		switch (op.type) {
			case TokenType::MINUS:
				return make_at<UnaryExpr>(op, UnaryExpr::Op::NEG, std::move(operand));
			case TokenType::BIT_NOT:
				return make_at<UnaryExpr>(op, UnaryExpr::Op::BIT_NOT, std::move(operand));
			default:
				// Unary '+' is a no-op
				return operand;
		}
	}

	return parse_call();
}

ExprPtr Parser::parse_power() {
	ExprPtr left = parse_unary();

	// Left-associative, and looser than a leading '-'. The manual's operator table
	// says otherwise, but the engine is authoritative: it answers 64 for
	// `2 ** 3 ** 2` ((2**3)**2) and 4 for `-2 ** 2` ((-2)**2).
	while (match(TokenType::POWER)) {
		ExprPtr right = parse_unary();
		left = make_binary(std::move(left), BinaryExpr::Op::POW, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_type_test() {
	ExprPtr left = parse_power();

	// `x is int` and `x is not int`. Both bind tighter than every operator except
	// call and subscript, so the type name is read here rather than as an
	// expression: `int` is a type in this position, not the constructor.
	while (check(TokenType::IS)) {
		advance();
		const bool negated = match(TokenType::NOT);
		const Token type_token = consume(TokenType::IDENTIFIER, "Expected a type name after 'is'");
		const Expr& start = *left;
		left = make_like<TypeTestExpr>(start, std::move(left), type_token.lexeme);
		if (negated) {
			const Expr& test = *left;
			left = make_like<UnaryExpr>(test, UnaryExpr::Op::NOT, std::move(left));
		}
	}

	return left;
}

ExprPtr Parser::parse_call() {
	ExprPtr expr = parse_primary();

	while (true) {
		if (match(TokenType::LPAREN)) {
			// Function call
			std::vector<ExprPtr> arguments;
			std::vector<std::string> names;
			parse_argument_list(arguments, names);

			// Check if this is a method call
			if (auto* var_expr = dynamic_cast<VariableExpr*>(expr.get())) {
				// Local function call
				std::string func_name = var_expr->name;
				auto call = make_like<CallExpr>(*expr, func_name, std::move(arguments));
				call->argument_names = std::move(names);
				expr = std::move(call);
			} else {
				error("Invalid call expression");
			}
		} else if (match(TokenType::DOT)) {
			// Member access
			Token member = consume(TokenType::IDENTIFIER, "Expected property or method name after '.'");

			if (match(TokenType::LPAREN)) {
				// Method call (including argument-less methods like obj.method())
				std::vector<ExprPtr> arguments;
				std::vector<std::string> names;
				parse_argument_list(arguments, names);

				auto call = make_like<MemberCallExpr>(*expr, std::move(expr), member.lexeme,
					std::move(arguments), true);
				call->argument_names = std::move(names);
				expr = std::move(call);
			} else {
				// Property access (no parentheses)
				expr = make_like<MemberCallExpr>(*expr, std::move(expr), member.lexeme, std::vector<ExprPtr>{}, false);
			}
		} else if (match(TokenType::LBRACKET)) {
			// Array indexing
			ExprPtr index = parse_expression();
			consume(TokenType::RBRACKET, "Expected ']' after index");
			expr = make_like<IndexExpr>(*expr, std::move(expr), std::move(index));
		} else {
			break;
		}
	}

	return expr;
}

ExprPtr Parser::parse_primary() {
	if (match(TokenType::TRUE)) {
		return make_at<LiteralExpr>(previous(), true);
	}
	if (match(TokenType::FALSE)) {
		return make_at<LiteralExpr>(previous(), false);
	}
	if (match(TokenType::NULL_VAL)) {
		const Token token = previous();
		auto node = LiteralExpr::null();
		node->line = token.line;
		node->column = token.column;
		return node;
	}

	if (match(TokenType::INTEGER)) {
		Token num = previous();
		return make_at<LiteralExpr>(num, std::get<int64_t>(num.value));
	}

	if (match(TokenType::FLOAT)) {
		Token num = previous();
		return make_at<LiteralExpr>(num, std::get<double>(num.value));
	}

	if (match(TokenType::STRING)) {
		Token str = previous();
		return make_at<LiteralExpr>(str, std::get<std::string>(str.value));
	}

	if (match(TokenType::IDENTIFIER)) {
		Token name = previous();
		return make_at<VariableExpr>(name, name.lexeme);
	}

	if (match(TokenType::LPAREN)) {
		ExprPtr expr = parse_expression();
		consume(TokenType::RPAREN, "Expected ')' after expression");
		return expr;
	}

	if (match(TokenType::LBRACKET)) {
		// Array literal: [1, 2, 3]
		const Token bracket = previous();
		std::vector<ExprPtr> elements;

		if (!check(TokenType::RBRACKET)) {
			do {
				elements.push_back(parse_expression());
				if (!match(TokenType::COMMA)) {
					break;
				}
			} while (!check(TokenType::RBRACKET)); // trailing ',' allowed
		}

		consume(TokenType::RBRACKET, "Expected ']' after array elements");
		return make_at<ArrayLiteralExpr>(bracket, std::move(elements));
	}

	if (match(TokenType::LBRACE)) {
		// Dictionary literal: {"key": "value", "num": 42} or {key: "value", num: 42}
		const Token brace = previous();
		std::vector<std::pair<ExprPtr, ExprPtr>> elements;

		if (!check(TokenType::RBRACE)) {
			do {
				// Check if the key is an identifier (for shorthand {name: value} syntax)
				ExprPtr key;
				if (match(TokenType::IDENTIFIER)) {
					// Convert identifier to string literal
					Token identifier = previous();
					key = make_at<LiteralExpr>(identifier, identifier.lexeme);
				} else {
					// Otherwise parse as a normal expression
					key = parse_expression();
				}

				// Godot accepts `{"k": v}` and the Lua-style `{k = v}`. The
				// latter only applies to an identifier key, the only thing that
				// can precede the '='.
				if (!match(TokenType::ASSIGN)) {
					consume(TokenType::COLON, "Expected ':' or '=' after dictionary key");
				}
				ExprPtr value = parse_expression();
				elements.push_back({std::move(key), std::move(value)});
				if (!match(TokenType::COMMA)) {
					break;
				}
			} while (!check(TokenType::RBRACE)); // trailing ',' allowed
		}

		consume(TokenType::RBRACE, "Expected '}' after dictionary elements");
		return make_at<DictionaryLiteralExpr>(brace, std::move(elements));
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

Token Parser::peek_ahead(size_t offset) const {
	const size_t index = m_current + offset;
	// The token stream always ends in EOF, so clamping to the last token gives
	// a lookahead that never runs off the end.
	return m_tokens[index < m_tokens.size() ? index : m_tokens.size() - 1];
}

Token Parser::previous() const {
	return m_tokens[m_current - 1];
}

bool Parser::is_at_end() const {
	return peek().type == TokenType::EOF_TOKEN;
}

Token Parser::consume(TokenType type, const std::string& message) {
	if (check(type)) return advance();

	error(message + ", but found " + peek().describe());
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
	while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {
		// Skip
	}
}

void Parser::consume_statement_end(const std::string& message) {
	// A ';' ends a statement as a newline does, and allows another on the same
	// line. What follows -- a statement, or the newline of a trailing ';' -- is
	// the next statement's problem; skip_newlines() in parse_statement_impl()
	// absorbs the latter.
	if (match(TokenType::SEMICOLON)) {
		return;
	}
	consume(TokenType::NEWLINE, message);
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
			skip_type_arguments();
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
				skip_type_arguments();
				return type_token.lexeme;
			}
			return "";  // Found -> but no type
		}
		// Not a return type, rewind
		m_current = saved_pos;
	}

	return "";
}

void Parser::skip_type_arguments() {
	// The element types of `Array[int]` and `Dictionary[String, int]` are not
	// checked: every value the compiler moves is a Variant, and Godot enforces
	// typed containers at the boundary. Keep the base type, skip the arguments.
	if (!match(TokenType::LBRACKET)) {
		return;
	}
	int depth = 1;
	while (depth > 0 && !is_at_end()) {
		if (match(TokenType::LBRACKET)) {
			depth++;
		} else if (match(TokenType::RBRACKET)) {
			depth--;
		} else {
			advance();
		}
	}
	if (depth != 0) {
		error("Expected ']' to close the element type");
	}
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
