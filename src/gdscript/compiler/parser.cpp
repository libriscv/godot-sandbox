#include "parser.h"
#include "compiler_exception.h"
#include "globals.h"
#include <stdexcept>
#include <sstream>
#include <cmath>
#include <cstdint>

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
	// Contiguous ## lines above the declaration, bottom-up then reversed.
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

	// Godot answers "Unexpected \"extends\" in class body" to either keyword once
	// a declaration has been parsed: both head the file, and only each other and
	// file-level annotations may come first.
	bool saw_declaration = false;
	const auto heads_the_file = [&](const char* keyword, const Token& token) {
		if (saw_declaration) {
			error(std::string("'") + keyword + "' comes before every other declaration"
				" in the file", token.line, token.column);
		}
	};

	skip_newlines();

	while (!is_at_end()) {
		const size_t statement_start = m_current;
		try {
		bool is_static = match(TokenType::STATIC);

		if (check(TokenType::EXTENDS)) {
			const Token& extends_token = advance();
			heads_the_file("extends", extends_token);
			if (!program.base_class.empty()) {
				error("A script extends one base", extends_token.line, extends_token.column);
			}
			program.base_class_line = extends_token.line;
			program.base_class_column = extends_token.column;
			if (check(TokenType::STRING)) {
				const Token& path = advance();
				program.base_class = std::get<std::string>(path.value);
				program.base_is_path = true;
			} else {
				program.base_class = consume(TokenType::IDENTIFIER,
					"Expected a class name or a path after 'extends'").lexeme;
				while (match(TokenType::DOT)) {
					program.base_class += '.';
					program.base_class += consume(TokenType::IDENTIFIER,
						"Expected a class name after '.'").lexeme;
				}
			}
			skip_newlines();
		} else if (check(TokenType::USES)) {
			const Token& uses_token = advance();
			heads_the_file("uses", uses_token);
			if (!program.uses.empty()) {
				error("A script declares one uses list", uses_token.line,
					uses_token.column);
			}
			program.uses = parse_uses_list();
			consume_statement_end("Expected newline after 'uses'");
		} else if (check(TokenType::SIGNAL)) {
			program.signals.push_back(parse_signal());
			saw_declaration = true;
		} else if (check(TokenType::AT)) {
			// Stacked attributes: `@export_range(0, 10) @tool var x`.
			bool is_export = false;
			bool is_onready = false;
			bool is_test = false;
			std::optional<RPCConfig> rpc_config;
			ExportHint export_hint;
			while (check(TokenType::AT)) {
				is_export = parse_attribute(export_hint, &is_onready, &rpc_config, nullptr,
					&is_test) || is_export;
				skip_newlines();
			}
			is_static = match(TokenType::STATIC) || is_static;
			if (check(TokenType::VAR) || check(TokenType::CONST)) {
				if (rpc_config.has_value()) {
					error("@rpc can only be applied to a function");
				}
				if (is_test) {
					error("@test can only be applied to a function");
				}
				const bool is_const = check(TokenType::CONST);
				advance();
				if (is_onready && is_const) {
					error("@onready cannot be applied to a 'const'");
				}
				if (is_onready && is_static) {
					error("@onready is per instance, and a 'static var' is shared");
				}
				auto var_decl = parse_var_decl(is_const);
				if (auto* decl = dynamic_cast<VarDeclStmt*>(var_decl.get())) {
					decl->is_property = is_export;
					decl->is_static = is_static;
					decl->is_onready = is_onready;
					decl->export_hint = export_hint;
					decl->export_section = m_export_section;
					record_const_value(*decl);
					program.globals.push_back(std::move(*decl));
				}
				saw_declaration = true;
			} else if (is_onready) {
				error("Expected a variable declaration after '@onready'");
				synchronize();
			} else if (check(TokenType::FUNC)) {
				// @export names a property; a function is not one.
				if (is_export) {
					error("Expected a variable declaration after '@export'");
				}
				if (rpc_config.has_value() && is_static) {
					error("@rpc cannot be applied to a static function");
				}
				if (is_test && rpc_config.has_value()) {
					error("@test cannot be combined with @rpc");
				}
				FunctionDecl function = parse_function();
				function.is_static = is_static;
				if (is_test) {
					// The runner invents no argument values, and a suspended
					// coroutine would return before its assertions ran.
					if (!function.parameters.empty()) {
						error("@test function '" + function.name + "' takes no parameters",
							function.parameters.front().line,
							function.parameters.front().column);
					}
					if (function.is_coroutine) {
						error("@test function '" + function.name + "' cannot be a coroutine",
							function.line, function.column);
					}
					function.is_test = true;
				}
				if (rpc_config.has_value()) {
					rpc_config->name = function.name;
					function.rpc_config = std::move(rpc_config);
				}
				program.functions.push_back(std::move(function));
				saw_declaration = true;
			} else if (is_export) {
				error("Expected a variable declaration after '@export'");
				synchronize();
			} else if (rpc_config.has_value()) {
				error("@rpc can only be applied to a function");
				synchronize();
			} else if (is_test) {
				error("@test can only be applied to a function");
				synchronize();
			}
			// File-level annotation (@tool etc.) — no declaration follows.
		} else if (check(TokenType::VAR)) {
			advance();
			auto var_decl = parse_var_decl(false);
			if (auto* decl = dynamic_cast<VarDeclStmt*>(var_decl.get())) {
				decl->is_static = is_static;
				program.globals.push_back(std::move(*decl));
			}
			saw_declaration = true;
		} else if (check(TokenType::CONST)) {
			advance();
			auto const_decl = parse_var_decl(true);
			if (auto* decl = dynamic_cast<VarDeclStmt*>(const_decl.get())) {
				record_const_value(*decl);
				program.globals.push_back(std::move(*decl));
			}
			saw_declaration = true;
		} else if (check(TokenType::STRUCT)) {
			program.structs.push_back(parse_struct());
			saw_declaration = true;
		} else if (check(TokenType::TRAIT)) {
			program.traits.push_back(parse_trait());
			saw_declaration = true;
		} else if (check(TokenType::CLASS)) {
			program.structs.push_back(parse_class());
			saw_declaration = true;
		} else if (check(TokenType::ENUM)) {
			program.enums.push_back(parse_enum());
			saw_declaration = true;
		} else if (check(TokenType::CLASS_NAME)) {
			const Token& class_name_token = advance();
			heads_the_file("class_name", class_name_token);
			if (!program.class_name.empty()) {
				error("A script declares one class_name",
					class_name_token.line, class_name_token.column);
			}
			program.class_name_line = class_name_token.line;
			program.class_name_column = class_name_token.column;
			program.class_name = consume(TokenType::IDENTIFIER,
				"Expected a class name after 'class_name'").lexeme;
			skip_newlines();
		} else if (check(TokenType::TRAIT_NAME)) {
			const Token& trait_name_token = advance();
			heads_the_file("trait_name", trait_name_token);
			if (!program.trait_name.empty() || !program.class_name.empty()) {
				error("A script declares either one trait_name or one class_name",
					trait_name_token.line, trait_name_token.column);
			}
			program.trait_name_line = trait_name_token.line;
			program.trait_name_column = trait_name_token.column;
			program.trait_name = consume(TokenType::IDENTIFIER,
				"Expected a trait name after 'trait_name'").lexeme;
			consume_statement_end("Expected newline after 'trait_name'");
			program.traits.push_back(parse_file_trait(trait_name_token,
				program.trait_name));
			saw_declaration = true;
		} else if (check(TokenType::FUNC)) {
			FunctionDecl function = parse_function();
			function.is_static = is_static;
			program.functions.push_back(std::move(function));
			saw_declaration = true;
		} else {
			error("Expected function or variable declaration");
			synchronize();
		}
		} catch (const Recovery&) {
			if (m_current == statement_start && !is_at_end()) {
				advance();
			}
			recover_to_statement_end();
			while (!is_at_end() && !check(TokenType::FUNC) && !check(TokenType::VAR) &&
				!check(TokenType::CONST) && !check(TokenType::STATIC) &&
				!check(TokenType::CLASS) && !check(TokenType::CLASS_NAME) &&
				!check(TokenType::STRUCT) && !check(TokenType::TRAIT) &&
				!check(TokenType::TRAIT_NAME) && !check(TokenType::ENUM) &&
				!check(TokenType::SIGNAL) && !check(TokenType::EXTENDS) &&
				!check(TokenType::AT)) {
				advance();
			}
		}
		skip_newlines();
	}

	hoist_onready_initializers(program);
	program.is_tool = m_saw_tool;
	program.native_base_class = program.base_class;
	program.native_base_is_path = program.base_is_path;
	program.chain.class_names.push_back(program.class_name);
	program.chain.paths.emplace_back();
	return program;
}

void Parser::hoist_onready_initializers(Program& program) {
	std::vector<StmtPtr> prologue;
	for (VarDeclStmt& global : program.globals) {
		if (!global.is_onready || !global.initializer) {
			continue;
		}
		auto assign = std::make_unique<AssignStmt>(global.name, std::move(global.initializer));
		assign->line = global.line;
		assign->column = global.column;
		prologue.push_back(std::move(assign));
		global.initializer = nullptr;
	}
	if (prologue.empty()) {
		return;
	}

	FunctionDecl* ready = nullptr;
	for (FunctionDecl& func : program.functions) {
		if (func.name == "_ready") {
			ready = &func;
			break;
		}
	}
	if (ready == nullptr) {
		FunctionDecl created;
		created.name = "_ready";
		created.line = prologue.front()->line;
		created.column = prologue.front()->column;
		program.functions.push_back(std::move(created));
		ready = &program.functions.back();
	}

	std::vector<StmtPtr> body;
	body.reserve(prologue.size() + ready->body.size());
	for (auto& stmt : prologue) {
		body.push_back(std::move(stmt));
	}
	for (auto& stmt : ready->body) {
		body.push_back(std::move(stmt));
	}
	ready->body = std::move(body);
}

FunctionDecl Parser::parse_function() {
	FunctionDecl func;
	const Token& func_token = consume(TokenType::FUNC, "Expected 'func'");
	func.line = func_token.line;
	func.column = func_token.column;
	func.doc_comment = doc_comment_above(func_token.line);

	const Token& name = consume(TokenType::IDENTIFIER, "Expected function name");
	func.name = name.lexeme;

	consume(TokenType::LPAREN, "Expected '(' after function name");
	func.parameters = parse_parameters();
	consume(TokenType::RPAREN, "Expected ')' after parameters");

	func.return_type = parse_return_type();

	consume(TokenType::COLON, "Expected ':' after function signature");

	// Track `await` during body parse to set is_coroutine.
	const bool enclosing_saw_await = m_saw_await;
	m_saw_await = false;
	func.body = parse_suite();
	func.is_coroutine = m_saw_await;
	m_saw_await = enclosing_saw_await;

	return func;
}

bool Parser::holds_engine_constant(const Expr* expr) {
	if (expr == nullptr) {
		return false;
	}
	if (auto* member = dynamic_cast<const MemberCallExpr*>(expr)) {
		return !member->is_method_call;
	}
	if (auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
		return holds_engine_constant(unary->operand.get());
	}
	if (auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
		return holds_engine_constant(binary->left.get()) ||
			holds_engine_constant(binary->right.get());
	}
	return false;
}

bool Parser::try_fold_int(const Expr* expr, const EnumDecl* decl, int64_t& out,
	std::string& why, const Expr*& at) const
{
	const auto fail = [&](const std::string& what) {
		if (why.empty()) {
			why = what;
			at = expr;
		}
		return false;
	};

	if (auto* literal = dynamic_cast<const LiteralExpr*>(expr)) {
		switch (literal->lit_type) {
			case LiteralExpr::Type::INTEGER: out = std::get<int64_t>(literal->value); return true;
			case LiteralExpr::Type::BOOL: out = std::get<bool>(literal->value) ? 1 : 0; return true;
			default: return fail("this literal is not an integer");
		}
	}

	if (auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
		int64_t operand = 0;
		if (!try_fold_int(unary->operand.get(), decl, operand, why, at)) {
			return false;
		}
		switch (unary->op) {
			case UnaryExpr::Op::NEG: out = int64_t(0u - uint64_t(operand)); return true;
			case UnaryExpr::Op::BIT_NOT: out = ~operand; return true;
			case UnaryExpr::Op::NOT: out = operand == 0 ? 1 : 0; return true;
		}
		return fail("this operator is not an integer operator");
	}

	if (auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
		int64_t left = 0;
		int64_t right = 0;
		if (!try_fold_int(binary->left.get(), decl, left, why, at) ||
			!try_fold_int(binary->right.get(), decl, right, why, at)) {
			return false;
		}
		const uint64_t ul = uint64_t(left);
		const uint64_t ur = uint64_t(right);
		switch (binary->op) {
			case BinaryExpr::Op::ADD: out = int64_t(ul + ur); return true;
			case BinaryExpr::Op::SUB: out = int64_t(ul - ur); return true;
			case BinaryExpr::Op::MUL: out = int64_t(ul * ur); return true;
			case BinaryExpr::Op::DIV:
				if (right == 0) {
					return fail("it divides by zero");
				}
				out = (left == INT64_MIN && right == -1) ? INT64_MIN : left / right;
				return true;
			case BinaryExpr::Op::MOD:
				if (right == 0) {
					return fail("it divides by zero");
				}
				out = (left == INT64_MIN && right == -1) ? 0 : left % right;
				return true;
			case BinaryExpr::Op::POW: {
				const double result = std::pow(double(left), double(right));
				if (!std::isfinite(result) || result < double(INT64_MIN) ||
					result >= -double(INT64_MIN)) {
					return fail("this power is outside the range of an integer");
				}
				out = int64_t(result);
				return true;
			}
			case BinaryExpr::Op::SHL: out = int64_t(ul << (ur & 63)); return true;
			case BinaryExpr::Op::SHR: out = left >> (ur & 63); return true;
			case BinaryExpr::Op::BIT_AND: out = left & right; return true;
			case BinaryExpr::Op::BIT_OR: out = left | right; return true;
			case BinaryExpr::Op::BIT_XOR: out = left ^ right; return true;
			case BinaryExpr::Op::EQ: out = left == right; return true;
			case BinaryExpr::Op::NEQ: out = left != right; return true;
			case BinaryExpr::Op::LT: out = left < right; return true;
			case BinaryExpr::Op::LTE: out = left <= right; return true;
			case BinaryExpr::Op::GT: out = left > right; return true;
			case BinaryExpr::Op::GTE: out = left >= right; return true;
			case BinaryExpr::Op::AND: out = (left != 0 && right != 0) ? 1 : 0; return true;
			case BinaryExpr::Op::OR: out = (left != 0 || right != 0) ? 1 : 0; return true;
			case BinaryExpr::Op::COALESCE: out = left; return true;
			case BinaryExpr::Op::IN: break;
		}
		return fail("this operator is not an integer operator");
	}

	if (auto* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
		int64_t condition = 0;
		int64_t when_true = 0;
		int64_t when_false = 0;
		if (!try_fold_int(ternary->condition.get(), decl, condition, why, at) ||
			!try_fold_int(ternary->true_value.get(), decl, when_true, why, at) ||
			!try_fold_int(ternary->false_value.get(), decl, when_false, why, at)) {
			return false;
		}
		out = condition != 0 ? when_true : when_false;
		return true;
	}

	if (auto* variable = dynamic_cast<const VariableExpr*>(expr)) {
		if (decl != nullptr) {
			if (const EnumDecl::Member* member = decl->find_member(variable->name)) {
				if (member->value_expr != nullptr) {
					return fail("'" + variable->name + "' is an engine constant, "
						"which has no value until the program runs");
				}
				out = member->value;
				return true;
			}
		}
		if (auto it = m_const_ints.find(variable->name); it != m_const_ints.end()) {
			out = it->second;
			return true;
		}
		if (const GlobalConstant* constant = find_global_constant(variable->name)) {
			if (!constant->is_float) {
				out = constant->int_value;
				return true;
			}
		}
		return fail("'" + variable->name + "' is not one of this enum's earlier members "
			"and not an integer 'const'");
	}

	return fail("only literals, operators over them, earlier members and integer "
		"'const' values are");
}

void Parser::record_const_value(const VarDeclStmt& decl) {
	if (!decl.is_const || decl.initializer == nullptr) {
		return;
	}
	int64_t value = 0;
	std::string why;
	const Expr* at = decl.initializer.get();
	if (try_fold_int(decl.initializer.get(), nullptr, value, why, at)) {
		m_const_ints[decl.name] = value;
	}
}

int64_t Parser::fold_enum_value(const Expr* expr, const EnumDecl& decl, const Token& start) {
	int64_t value = 0;
	std::string why;
	const Expr* at = expr;
	if (!try_fold_int(expr, &decl, value, why, at)) {
		const int line = at->line ? at->line : start.line;
		const int column = at->line ? at->column : start.column;
		error("An enum member's value has to be an integer constant expression; " + why,
			line, column);
		return 0;
	}
	return value;
}

EnumDecl Parser::parse_enum() {
	EnumDecl decl;
	const Token& enum_token = consume(TokenType::ENUM, "Expected 'enum'");
	decl.line = enum_token.line;
	decl.column = enum_token.column;

	// Named enum vs unnamed (file-scope members).
	if (check(TokenType::IDENTIFIER)) {
		decl.name = advance().lexeme;
	}

	consume(TokenType::LBRACE, "Expected '{' after 'enum'");

	// Newlines inside braces are swallowed by the lexer.
	int64_t next_value = 0;
	const Expr* next_value_expr = nullptr;
	while (!check(TokenType::RBRACE) && !is_at_end()) {
		const Token& member_name = consume(TokenType::IDENTIFIER, "Expected an enum member name");

		EnumDecl::Member member;
		member.name = member_name.lexeme;
		member.line = member_name.line;
		member.column = member_name.column;

		if (match(TokenType::ASSIGN)) {
			const Token& start = peek();
			ExprPtr initializer = parse_expression();
			if (holds_engine_constant(initializer.get())) {
				next_value = 0;
				next_value_expr = initializer.get();
				decl.owned_values.push_back(std::move(initializer));
			} else {
				next_value = fold_enum_value(initializer.get(), decl, start);
				next_value_expr = nullptr;
			}
		}

		member.value = next_value;
		member.value_expr = next_value_expr;
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
	const Token& struct_token = consume(TokenType::STRUCT, "Expected 'struct'");
	decl.line = struct_token.line;
	decl.column = struct_token.column;
	decl.doc_comment = doc_comment_above(struct_token.line);

	const Token& name = consume(TokenType::IDENTIFIER, "Expected struct name");
	decl.name = name.lexeme;
	if (match(TokenType::USES)) {
		decl.uses = parse_uses_list();
	}

	consume(TokenType::COLON, "Expected ':' after struct name");
	consume(TokenType::NEWLINE, "Expected newline after struct declaration");

	skip_newlines();
	consume(TokenType::INDENT, "Expected indented block after 'struct " + decl.name + ":'");

	while (!check(TokenType::DEDENT) && !is_at_end()) {
		skip_newlines();
		if (check(TokenType::DEDENT) || is_at_end()) {
			break;
		}

		if (match(TokenType::PASS)) {
			consume_statement_end("Expected newline after 'pass'");
			continue;
		}

		bool is_static = match(TokenType::STATIC);

		if (check(TokenType::AT)) {
			bool ignored_export = false;
			bool ignored_onready = false;
			std::optional<RPCConfig> ignored_rpc;
			ExportHint ignored_hint;
			while (check(TokenType::AT)) {
				ignored_export = parse_attribute(ignored_hint, &ignored_onready, &ignored_rpc) || ignored_export;
				skip_newlines();
			}
			(void)ignored_export;
			is_static = match(TokenType::STATIC) || is_static;
			if (!check(TokenType::FUNC)) {
				error("A class annotation can only be applied to a function");
			}
		}
		if (check(TokenType::FUNC)) {
			FunctionDecl method = parse_function();
			method.is_static = is_static;
			if (decl.find_method(method.name) != nullptr) {
				throw CompilerException::parser_error(
					"Struct '" + decl.name + "' declares '" + method.name + "()' more than once",
					method.line, method.column);
			}
			decl.methods.push_back(std::move(method));
			continue;
		}

		if (check(TokenType::CONST)) {
			const Token& const_token = advance();
			if (is_static) {
				error("'static' is redundant on a struct const", const_token.line, const_token.column);
			}
			StructField constant;
			constant.line = const_token.line;
			constant.column = const_token.column;
			constant.doc_comment = doc_comment_above(const_token.line);
			constant.name = consume(TokenType::IDENTIFIER, "Expected a name after 'const'").lexeme;
			constant.type_hint = parse_type_hint();
			consume(TokenType::ASSIGN, "A struct const needs a value");
			constant.default_value = parse_expression();
			consume_statement_end("Expected newline after the constant declaration");
			if (decl.find_constant(constant.name) != nullptr || decl.find_field(constant.name) != nullptr) {
				throw CompilerException::parser_error(
					"Struct '" + decl.name + "' declares '" + constant.name + "' more than once",
					constant.line, constant.column);
			}
			decl.constants.push_back(std::move(constant));
			continue;
		}

		const Token& var_token = consume(TokenType::VAR,
			"A struct body holds constant, field and function declarations");
		if (is_static) {
			error("A struct field is one per instance, so it cannot be 'static'",
				var_token.line, var_token.column);
		}

		StructField field;
		field.line = var_token.line;
		field.column = var_token.column;
		field.doc_comment = doc_comment_above(var_token.line);

		const Token& field_name = consume(TokenType::IDENTIFIER, "Expected field name");
		field.name = field_name.lexeme;
		field.type_hint = parse_type_hint();

		if (match(TokenType::ASSIGN)) {
			field.default_value = parse_expression();
		}
		consume(TokenType::NEWLINE, "Expected newline after field declaration");

		if (decl.find_field(field.name) != nullptr || decl.find_constant(field.name) != nullptr) {
			throw CompilerException::parser_error(
				"Struct '" + decl.name + "' declares field '" + field.name + "' more than once",
				field.line, field.column);
		}
		decl.fields.push_back(std::move(field));
	}

	consume(TokenType::DEDENT, "Expected dedent after struct body");
	return decl;
}

StructDecl Parser::parse_class() {
	StructDecl decl;
	decl.is_class = true;
	const Token& class_token = consume(TokenType::CLASS, "Expected 'class'");
	decl.line = class_token.line;
	decl.column = class_token.column;
	decl.doc_comment = doc_comment_above(class_token.line);

	const Token& name = consume(TokenType::IDENTIFIER, "Expected a class name after 'class'");
	decl.name = name.lexeme;

	auto parse_extends = [&]() {
		const Token& base = consume(TokenType::IDENTIFIER,
			"Expected a class name after 'extends'");
		decl.base_name = base.lexeme;
		if (check(TokenType::DOT)) {
			std::string qualified = decl.base_name;
			while (match(TokenType::DOT)) {
				qualified += '.';
				qualified += consume(TokenType::IDENTIFIER,
					"Expected a class name after '.'").lexeme;
			}
			error("Class '" + decl.name + "' cannot extend '" + qualified +
				"': a base class has to be one declared in this file",
				base.line, base.column);
		}
	};

	if (match(TokenType::EXTENDS)) {
		parse_extends();
	}
	if (match(TokenType::USES)) {
		decl.uses = parse_uses_list();
	}

	consume(TokenType::COLON, "Expected ':' after the class name");
	consume(TokenType::NEWLINE, "Expected newline after the class declaration");

	skip_newlines();
	consume(TokenType::INDENT, "Expected indented block after 'class " + decl.name + ":'");

	skip_newlines();
	if (check(TokenType::EXTENDS)) {
		const Token& extends_token = advance();
		if (!decl.base_name.empty()) {
			error("Class '" + decl.name + "' already declares a base class",
				extends_token.line, extends_token.column);
		}
		parse_extends();
		consume_statement_end("Expected newline after 'extends'");
	}
	skip_newlines();
	if (check(TokenType::USES)) {
		const Token& token = advance();
		if (!decl.uses.empty()) {
			error("Class '" + decl.name + "' already declares an uses list",
				token.line, token.column);
		}
		decl.uses = parse_uses_list();
		consume_statement_end("Expected newline after 'uses'");
	}

	while (!check(TokenType::DEDENT) && !is_at_end()) {
		skip_newlines();
		if (check(TokenType::DEDENT) || is_at_end()) {
			break;
		}

		if (match(TokenType::PASS)) {
			consume_statement_end("Expected newline after 'pass'");
			continue;
		}

		bool is_static = match(TokenType::STATIC);

		if (check(TokenType::AT)) {
			bool ignored_export = false;
			bool ignored_onready = false;
			std::optional<RPCConfig> ignored_rpc;
			ExportHint ignored_hint;
			while (check(TokenType::AT)) {
				ignored_export = parse_attribute(ignored_hint, &ignored_onready, &ignored_rpc) || ignored_export;
				skip_newlines();
			}
			(void)ignored_export;
			is_static = match(TokenType::STATIC) || is_static;
			if (!check(TokenType::FUNC)) {
				error("A class annotation can only be applied to a function");
			}
		}

		if (check(TokenType::FUNC)) {
			FunctionDecl method = parse_function();
			method.is_static = is_static;
			if (decl.find_method(method.name) != nullptr) {
				throw CompilerException::parser_error(
					"Class '" + decl.name + "' declares '" + method.name + "()' more than once",
					method.line, method.column);
			}
			decl.methods.push_back(std::move(method));
			continue;
		}

		if (check(TokenType::CONST)) {
			const Token& const_token = advance();
			if (is_static) {
				error("'static' says one per class, and a 'const' is already that",
					const_token.line, const_token.column);
			}
			StructField constant;
			constant.line = const_token.line;
			constant.column = const_token.column;
			constant.doc_comment = doc_comment_above(const_token.line);
			constant.name = consume(TokenType::IDENTIFIER,
				"Expected a name after 'const'").lexeme;
			constant.type_hint = parse_type_hint();
			consume(TokenType::ASSIGN, "A 'const' is given its value where it is declared");
			constant.default_value = parse_expression();
			consume_statement_end("Expected newline after the constant declaration");

			if (decl.find_constant(constant.name) != nullptr
				|| decl.find_field(constant.name) != nullptr) {
				throw CompilerException::parser_error(
					"Class '" + decl.name + "' declares '" + constant.name + "' more than once",
					constant.line, constant.column);
			}
			decl.constants.push_back(std::move(constant));
			continue;
		}

		const Token& var_token = consume(TokenType::VAR,
			"A class body holds constant, field and function declarations");
		if (is_static) {
			// A static var is one slot shared by every instance; the class has no
			// storage of its own, only the Dictionary each new() builds.
			error("A class field is one per instance, so it cannot be 'static'",
				var_token.line, var_token.column);
		}

		StructField field;
		field.line = var_token.line;
		field.column = var_token.column;
		field.doc_comment = doc_comment_above(var_token.line);

		const Token& field_name = consume(TokenType::IDENTIFIER, "Expected a field name");
		field.name = field_name.lexeme;
		field.type_hint = parse_type_hint();

		if (match(TokenType::ASSIGN)) {
			field.default_value = parse_expression();
		}
		consume_statement_end("Expected newline after the field declaration");

		if (decl.find_field(field.name) != nullptr || decl.find_constant(field.name) != nullptr) {
			throw CompilerException::parser_error(
				"Class '" + decl.name + "' declares field '" + field.name + "' more than once",
				field.line, field.column);
		}
		decl.fields.push_back(std::move(field));
	}

	consume(TokenType::DEDENT, "Expected dedent after the class body");
	return decl;
}

std::vector<std::string> Parser::parse_uses_list() {
	std::vector<std::string> names;
	do {
		const Token& name = consume(TokenType::IDENTIFIER,
			"Expected a trait name after 'uses'");
		std::string qualified = name.lexeme;
		while (match(TokenType::DOT)) {
			qualified += ".";
			qualified += consume(TokenType::IDENTIFIER,
				"Expected a trait name after '.'").lexeme;
		}
		for (const std::string& existing : names) {
			if (existing == qualified) {
				error("Trait '" + qualified + "' is listed more than once",
					name.line, name.column);
			}
		}
		names.push_back(std::move(qualified));
	} while (match(TokenType::COMMA));
	return names;
}

FunctionDecl Parser::parse_trait_method(const TraitDecl& owner, bool is_static,
	bool annotated_abstract, std::optional<RPCConfig> rpc_config) {
	FunctionDecl func;
	const Token& func_token = consume(TokenType::FUNC, "Expected 'func'");
	func.line = func_token.line;
	func.column = func_token.column;
	func.doc_comment = doc_comment_above(func_token.line);
	func.name = consume(TokenType::IDENTIFIER, "Expected function name").lexeme;
	if (func.name == "_init") {
		error("A trait cannot declare '_init'", func.line, func.column);
	}
	func.is_static = is_static;
	func.is_abstract = annotated_abstract;
	consume(TokenType::LPAREN, "Expected '(' after function name");
	func.parameters = parse_parameters();
	consume(TokenType::RPAREN, "Expected ')' after parameters");
	func.return_type = parse_return_type();

	if (match(TokenType::COLON)) {
		func.body = parse_suite();
		if (annotated_abstract) {
			error("@abstract method '" + func.name + "' cannot have a body",
				func.line, func.column);
		}
	} else {
		if (is_static) {
			error("A static trait method must have a body", func.line, func.column);
		}
		func.is_abstract = true;
		consume_statement_end("Expected newline after trait method signature");
	}
	if (owner.find_method(func.name) != nullptr) {
		error("Trait '" + owner.name + "' declares '" + func.name +
			"()' more than once", func.line, func.column);
	}
	if (rpc_config.has_value()) {
		rpc_config->name = func.name;
		func.rpc_config = std::move(rpc_config);
	}
	return func;
}

TraitDecl Parser::parse_trait() {
	TraitDecl decl;
	const Token& token = consume(TokenType::TRAIT, "Expected 'trait'");
	decl.line = token.line;
	decl.column = token.column;
	decl.doc_comment = doc_comment_above(token.line);
	decl.name = consume(TokenType::IDENTIFIER, "Expected trait name").lexeme;
	if (match(TokenType::EXTENDS)) {
		decl.base_name = consume(TokenType::IDENTIFIER,
			"Expected a base class after 'extends'").lexeme;
	}
	if (match(TokenType::USES)) decl.uses = parse_uses_list();
	consume(TokenType::COLON, "Expected ':' after trait name");
	consume(TokenType::NEWLINE, "Expected newline after trait declaration");
	skip_newlines();
	consume(TokenType::INDENT, "Expected indented block after 'trait " + decl.name + ":'");

	while (!check(TokenType::DEDENT) && !is_at_end()) {
		skip_newlines();
		if (check(TokenType::DEDENT) || is_at_end()) break;
		if (match(TokenType::PASS)) {
			consume_statement_end("Expected newline after 'pass'");
			continue;
		}
		if (match(TokenType::EXTENDS)) {
			if (!decl.base_name.empty()) error("Trait '" + decl.name + "' already declares a base requirement");
			decl.base_name = consume(TokenType::IDENTIFIER,
				"Expected a base class after 'extends'").lexeme;
			consume_statement_end("Expected newline after trait 'extends'");
			continue;
		}
		if (match(TokenType::USES)) {
			if (!decl.uses.empty()) error("Trait '" + decl.name + "' already declares a uses list");
			decl.uses = parse_uses_list();
			consume_statement_end("Expected newline after trait 'uses'");
			continue;
		}

		bool is_export = false;
		bool is_onready = false;
		bool is_abstract = false;
		std::optional<RPCConfig> rpc_config;
		ExportHint export_hint;
		while (check(TokenType::AT)) {
			is_export = parse_attribute(export_hint, &is_onready, &rpc_config,
				&is_abstract) || is_export;
			skip_newlines();
		}
		const bool is_static = match(TokenType::STATIC);
		if (check(TokenType::CLASS) || check(TokenType::STRUCT) || check(TokenType::TRAIT)) {
			error("A trait cannot declare an inner class");
		}
		if (check(TokenType::FUNC)) {
			if (is_export || is_onready) error("Variable annotations cannot be applied to a trait method");
			decl.methods.push_back(parse_trait_method(decl, is_static, is_abstract,
				std::move(rpc_config)));
			decl.methods.back().trait_origin = decl.name;
			continue;
		}
		if (is_abstract) error("@abstract can only be applied to a trait method");
		if (rpc_config.has_value()) error("@rpc can only be applied to a function");
		if (check(TokenType::SIGNAL)) {
			if (is_static || is_export || is_onready) error("A trait signal cannot use variable annotations");
			decl.signals.push_back(parse_signal());
			decl.signals.back().trait_origin = decl.name;
			continue;
		}
		if (check(TokenType::ENUM)) {
			if (is_static || is_export || is_onready) error("A trait enum cannot use variable annotations");
			decl.enums.push_back(parse_enum());
			continue;
		}
		if (check(TokenType::VAR) || check(TokenType::CONST)) {
			const bool is_const = check(TokenType::CONST);
			advance();
			if (is_onready && (is_const || is_static)) error("@onready requires an instance variable");
			auto statement = parse_var_decl(is_const);
			auto* var = dynamic_cast<VarDeclStmt*>(statement.get());
			var->is_property = is_export;
			var->is_onready = is_onready;
			var->is_static = is_static;
			var->export_hint = export_hint;
			var->export_section = m_export_section;
			var->trait_origin = decl.name;
			if (is_const) {
				StructField value;
				value.name = var->name;
				value.type_hint = std::move(var->type_hint);
				value.default_value = std::move(var->initializer);
				value.line = var->line;
				value.column = var->column;
				value.trait_origin = decl.name;
				decl.constants.push_back(std::move(value));
			} else {
				decl.vars.push_back(std::move(*var));
			}
			continue;
		}
		error("A trait body accepts variables, constants, enums, signals and functions");
	}
	consume(TokenType::DEDENT, "Expected dedent after trait body");
	return decl;
}

TraitDecl Parser::parse_file_trait(const Token& token, std::string name) {
	TraitDecl decl;
	decl.name = std::move(name);
	decl.line = token.line;
	decl.column = token.column;
	decl.doc_comment = doc_comment_above(token.line);
	decl.is_file_level = true;

	while (!is_at_end()) {
		skip_newlines();
		if (is_at_end()) break;
		if (match(TokenType::PASS)) {
			consume_statement_end("Expected newline after 'pass'");
			continue;
		}
		if (match(TokenType::EXTENDS)) {
			if (!decl.base_name.empty()) error("Trait '" + decl.name + "' already declares a base requirement");
			decl.base_name = consume(TokenType::IDENTIFIER,
				"Expected a base class after 'extends'").lexeme;
			consume_statement_end("Expected newline after trait 'extends'");
			continue;
		}
		if (match(TokenType::USES)) {
			if (!decl.uses.empty()) error("Trait '" + decl.name + "' already declares a uses list");
			decl.uses = parse_uses_list();
			consume_statement_end("Expected newline after trait 'uses'");
			continue;
		}

		bool is_export = false;
		bool is_onready = false;
		bool is_abstract = false;
		std::optional<RPCConfig> rpc_config;
		ExportHint export_hint;
		while (check(TokenType::AT)) {
			is_export = parse_attribute(export_hint, &is_onready, &rpc_config,
				&is_abstract) || is_export;
			skip_newlines();
		}
		const bool is_static = match(TokenType::STATIC);
		if (check(TokenType::CLASS) || check(TokenType::STRUCT) || check(TokenType::TRAIT) ||
			check(TokenType::CLASS_NAME) || check(TokenType::TRAIT_NAME)) {
			error("A trait cannot declare an inner class");
		}
		if (check(TokenType::FUNC)) {
			if (is_export || is_onready) error("Variable annotations cannot be applied to a trait method");
			decl.methods.push_back(parse_trait_method(decl, is_static, is_abstract,
				std::move(rpc_config)));
			decl.methods.back().trait_origin = decl.name;
			continue;
		}
		if (is_abstract) error("@abstract can only be applied to a trait method");
		if (rpc_config.has_value()) error("@rpc can only be applied to a function");
		if (check(TokenType::SIGNAL)) {
			if (is_static || is_export || is_onready) error("A trait signal cannot use variable annotations");
			decl.signals.push_back(parse_signal());
			decl.signals.back().trait_origin = decl.name;
			continue;
		}
		if (check(TokenType::ENUM)) {
			if (is_static || is_export || is_onready) error("A trait enum cannot use variable annotations");
			decl.enums.push_back(parse_enum());
			continue;
		}
		if (check(TokenType::VAR) || check(TokenType::CONST)) {
			const bool is_const = check(TokenType::CONST);
			advance();
			if (is_onready && (is_const || is_static)) error("@onready requires an instance variable");
			auto statement = parse_var_decl(is_const);
			auto* var = dynamic_cast<VarDeclStmt*>(statement.get());
			var->is_property = is_export;
			var->is_onready = is_onready;
			var->is_static = is_static;
			var->export_hint = export_hint;
			var->export_section = m_export_section;
			var->trait_origin = decl.name;
			if (is_const) {
				StructField value;
				value.name = var->name;
				value.type_hint = std::move(var->type_hint);
				value.default_value = std::move(var->initializer);
				value.line = var->line;
				value.column = var->column;
				value.trait_origin = decl.name;
				decl.constants.push_back(std::move(value));
			} else {
				decl.vars.push_back(std::move(*var));
			}
			continue;
		}
		error("A trait body accepts variables, constants, enums, signals and functions");
	}
	return decl;
}

void Parser::parse_argument_list(std::vector<ExprPtr>& arguments, std::vector<std::string>& names) {
	if (!check(TokenType::RPAREN)) {
		bool seen_named = false;
		do {
			std::string name;
			// Named argument: IDENTIFIER followed by ASSIGN (not EQUAL).
			if (check(TokenType::IDENTIFIER) && peek_ahead(1).type == TokenType::ASSIGN) {
				name = advance().lexeme;
				advance();
				seen_named = true;
			} else if (seen_named) {
				error("A positional argument cannot follow a named argument");
			}
			arguments.push_back(parse_expression());
			names.push_back(std::move(name));
			if (!match(TokenType::COMMA)) {
				break;
			}
		} while (!check(TokenType::RPAREN)); // trailing comma allowed
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
		const Token& param_name = consume(TokenType::IDENTIFIER, "Expected parameter name");
		Parameter param;
		param.name = param_name.lexeme;
		param.line = param_name.line;
		param.column = param_name.column;

		param.type_hint = parse_type_hint();

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

std::vector<StmtPtr> Parser::parse_suite() {
	if (check(TokenType::NEWLINE)) {
		advance();
		return parse_block();
	}
	return parse_inline_suite();
}

std::vector<StmtPtr> Parser::parse_inline_suite() {
	std::vector<StmtPtr> statements;

	m_inline_suite_depth++;
	do {
		if (at_inline_suite_end()) {
			break;
		}
		statements.push_back(parse_statement());
	} while (previous().type == TokenType::SEMICOLON);
	m_inline_suite_depth--;

	if (statements.empty()) {
		error("Expected a statement after ':'");
	}
	return statements;
}

bool Parser::at_inline_suite_end() const {
	switch (peek().type) {
		case TokenType::NEWLINE:
		case TokenType::DEDENT:
		case TokenType::RPAREN:
		case TokenType::RBRACKET:
		case TokenType::RBRACE:
		case TokenType::COMMA:
		case TokenType::EOF_TOKEN:
			return true;
		default:
			return false;
	}
}

StmtPtr Parser::parse_statement() {
	// Central source-position assignment; adding a statement kind cannot forget.
	const int line = peek().line;
	const int column = peek().column;
	const size_t entry = m_current;
	StmtPtr stmt;
	if (tolerant()) {
		try {
			stmt = parse_statement_impl();
		} catch (const Recovery&) {
			recover_to_statement_end();
			if (m_current == entry && !is_at_end() && !check(TokenType::DEDENT)) {
				advance();
			}
			stmt = std::make_unique<PassStmt>();
		}
	} else {
		stmt = parse_statement_impl();
	}
	if (stmt && stmt->line == 0) {
		stmt->line = line;
		stmt->column = column;
	}
	return stmt;
}

StmtPtr Parser::parse_statement_impl() {
	skip_newlines();

	while (check(TokenType::AT)) {
		ExportHint discarded;
		if (parse_attribute(discarded)) {
			error("@export names a script property; a local variable is not one");
		}
		skip_newlines();
	}

	if (match(TokenType::VAR)) {
		return parse_var_decl(false);
	}
	if (match(TokenType::CONST)) {
		return parse_var_decl(true);
	}
	if (check(TokenType::CLASS)) {
		error("A class can only be declared at the top level of the file");
	}
	if (check(TokenType::TRAIT)) {
		error("A trait can only be declared at the top level of the file");
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
		return parse_match_stmt(false);
	}
	if (match(TokenType::SWITCH)) {
		return parse_match_stmt(true);
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
	if (check(TokenType::BREAKPOINT)) {
		auto stmt = make_at<BreakpointStmt>(advance());
		consume_statement_end("Expected newline after 'breakpoint'");
		return stmt;
	}

	return parse_expr_or_assign_stmt();
}

bool Parser::at_property_accessor() const {
	if (!check(TokenType::IDENTIFIER)) {
		return false;
	}
	const TokenType next = peek_ahead(1).type;
	if (peek().lexeme == "set") {
		return next == TokenType::ASSIGN || next == TokenType::LPAREN ||
			next == TokenType::COLON;
	}
	if (peek().lexeme == "get") {
		return next == TokenType::ASSIGN || next == TokenType::LPAREN ||
			next == TokenType::COLON;
	}
	return false;
}

StmtPtr Parser::parse_var_decl(bool is_const) {
	const Token& name = consume(TokenType::IDENTIFIER, "Expected variable name");

	// ':' is ambiguous: type hint, accessor block, or bare `var x:`.
	TypeExpr type_hint;
	bool accessors_follow = false;
	if (match(TokenType::COLON)) {
		if (at_property_accessor() || check(TokenType::NEWLINE)) {
			accessors_follow = true;
		} else if (check(TokenType::IDENTIFIER) || check(TokenType::NULL_VAL)) {
			type_hint = parse_type_expr();
		}
	}

	ExprPtr initializer = nullptr;
	if (!accessors_follow) {
		if (match(TokenType::ASSIGN)) {
			initializer = parse_expression();
		} else if (is_const) {
			error("Const variables must have an initializer");
		}
		// Accessors may follow the initializer: `var x: int = 3: set(v): ...`
		accessors_follow = match(TokenType::COLON);
	}

	auto stmt = make_at<VarDeclStmt>(name, name.lexeme, std::move(initializer), is_const);
	stmt->type_hint = type_hint;
	stmt->doc_comment = doc_comment_above(name.line);

	if (accessors_follow) {
		if (is_const) {
			error("A const cannot have a setter or a getter");
		}
		parse_property_accessors(*stmt);
		return stmt;
	}

	consume_statement_end("Expected newline after variable declaration");
	return stmt;
}

void Parser::parse_property_accessors(VarDeclStmt& decl) {
	const bool block = check(TokenType::NEWLINE);
	if (block) {
		advance();
		consume(TokenType::INDENT, "Expected an indented block of property accessors");
	}

	while (true) {
		if (block) {
			skip_newlines();
			if (check(TokenType::DEDENT)) {
				break;
			}
		}
		parse_one_property_accessor(decl);
		const bool comma = match(TokenType::COMMA);
		if (!block && !comma) {
			break;
		}
	}

	if (block) {
		skip_newlines();
		consume(TokenType::DEDENT, "Expected the property accessors to end");
	} else {
		consume_statement_end("Expected newline after the property accessors");
	}

	if (!decl.has_accessors()) {
		error("Expected 'set' or 'get' after ':'");
	}
}

void Parser::parse_one_property_accessor(VarDeclStmt& decl) {
	if (!at_property_accessor()) {
		error("Expected 'set' or 'get' in a property declaration");
	}
	const Token& keyword = advance();
	const bool is_setter = keyword.lexeme == "set";

	if (is_setter ? (!decl.setter_name.empty() || decl.setter_body)
	              : (!decl.getter_name.empty() || decl.getter_body)) {
		error("Property '" + decl.name + "' already has a '" + keyword.lexeme + "'");
	}

	if (match(TokenType::ASSIGN)) {
		const Token& target = consume(TokenType::IDENTIFIER,
			"Expected a function name after '" + keyword.lexeme + " ='");
		(is_setter ? decl.setter_name : decl.getter_name) = target.lexeme;
		return;
	}

	auto body = std::make_unique<FunctionDecl>();
	// '@' prefix avoids collision with user-declared functions.
	body->name = "@" + decl.name + (is_setter ? "_setter" : "_getter");
	body->line = keyword.line;
	body->column = keyword.column;

	if (match(TokenType::LPAREN)) {
		if (is_setter || !check(TokenType::RPAREN)) {
			const Token& param = consume(TokenType::IDENTIFIER,
				"Expected the assigned value's name in 'set(...)'");
			Parameter parameter;
			parameter.name = param.lexeme;
			parameter.line = param.line;
			parameter.column = param.column;
			parameter.type_hint = parse_type_hint();
			body->parameters.push_back(std::move(parameter));
		}
		consume(TokenType::RPAREN, "Expected ')' after the accessor's parameter");
	} else if (is_setter) {
		error("Expected '(' after 'set': a setter body names the assigned value, "
			"as in 'set(value):'");
	}

	consume(TokenType::COLON, "Expected ':' after '" + keyword.lexeme + "'");

	const bool was_coroutine = m_saw_await;
	m_saw_await = false;
	body->body = parse_suite();
	body->is_coroutine = m_saw_await;
	m_saw_await = was_coroutine;

	if (body->is_coroutine) {
		error("A property accessor cannot await");
	}

	(is_setter ? decl.setter_body : decl.getter_body) = std::move(body);
}

StmtPtr Parser::parse_if_stmt() {
	ExprPtr condition;
	std::unique_ptr<VarDeclStmt> binding;
	if (match(TokenType::VAR)) {
		binding = parse_if_var_binding();
	} else {
		condition = parse_expression();
	}
	consume(TokenType::COLON, "Expected ':' after if condition");

	std::vector<StmtPtr> then_branch = parse_suite();
	std::vector<StmtPtr> else_branch;

	skip_newlines();

	if (match(TokenType::ELIF)) {
		auto elif_stmt = parse_if_stmt();
		else_branch.push_back(std::move(elif_stmt));
	} else if (match(TokenType::ELSE)) {
		consume(TokenType::COLON, "Expected ':' after else");
		else_branch = parse_suite();
	}

	if (binding) {
		return std::make_unique<IfStmt>(std::move(binding), std::move(then_branch),
			std::move(else_branch));
	}
	return std::make_unique<IfStmt>(std::move(condition), std::move(then_branch),
		std::move(else_branch));
}

std::unique_ptr<VarDeclStmt> Parser::parse_if_var_binding() {
	const Token& name = consume(TokenType::IDENTIFIER,
		"Expected variable name after 'if var'");

	TypeExpr type_hint;
	bool has_initializer = false;
	if (match(TokenType::COLON)) {
		// `:=` is lexed as COLON, ASSIGN.  A type name after ':' is the
		// ordinary annotated declaration form.
		if (match(TokenType::ASSIGN)) {
			has_initializer = true;
		} else if (check(TokenType::IDENTIFIER) || check(TokenType::NULL_VAL)) {
			type_hint = parse_type_expr();
			consume(TokenType::ASSIGN,
				"An 'if var' binding needs an initializer after its type");
			has_initializer = true;
		} else {
			error("An 'if var' binding needs an initializer with '=' or ':='");
		}
	} else if (match(TokenType::ASSIGN)) {
		has_initializer = true;
	}

	if (!has_initializer) {
		error("An 'if var' binding needs an initializer with '=' or ':='");
	}

	ExprPtr initializer = parse_expression();
	auto binding = make_at<VarDeclStmt>(name, name.lexeme, std::move(initializer), false);
	binding->type_hint = std::move(type_hint);
	return binding;
}

StmtPtr Parser::parse_while_stmt() {
	ExprPtr condition = parse_expression();
	consume(TokenType::COLON, "Expected ':' after while condition");

	std::vector<StmtPtr> body = parse_suite();

	return std::make_unique<WhileStmt>(std::move(condition), std::move(body));
}

StmtPtr Parser::parse_for_stmt() {
	const Token& var_name = consume(TokenType::IDENTIFIER, "Expected variable name in for loop");
	// Loop-variable type hint: parsed and dropped.
	parse_type_hint();
	consume(TokenType::IN, "Expected 'in' after for loop variable");
	ExprPtr iterable = parse_expression();
	consume(TokenType::COLON, "Expected ':' after for loop iterable");

	std::vector<StmtPtr> body = parse_suite();

	return std::make_unique<ForStmt>(var_name.lexeme, std::move(iterable), std::move(body));
}

StmtPtr Parser::parse_match_stmt(bool is_switch) {
	const Token& keyword = previous();
	const std::string kw = is_switch ? "switch" : "match";

	ExprPtr subject = parse_expression();
	consume(TokenType::COLON, "Expected ':' after " + kw + " subject");
	consume(TokenType::NEWLINE, "Expected newline after ':'");

	skip_newlines();
	consume(TokenType::INDENT, "Expected indented block after '" + kw + "'");

	std::vector<MatchStmt::Branch> branches;

	while (!check(TokenType::DEDENT) && !is_at_end()) {
		skip_newlines();
		if (check(TokenType::DEDENT) || is_at_end()) {
			break;
		}

		MatchStmt::Branch branch;

		do {
			branch.patterns.push_back(parse_match_pattern());
		} while (match(TokenType::COMMA) && !check(TokenType::COLON));

		// A binding alongside other patterns would be undefined when a sibling matched.
		if (branch.patterns.size() > 1) {
			for (const auto& pattern : branch.patterns) {
				if (pattern->binds()) {
					throw CompilerException::parser_error(
						"A pattern that binds a name cannot share an arm with other patterns",
						pattern->line, pattern->column);
				}
			}
		}

		// `when` is a contextual keyword; matched by lexeme, not token type.
		if (check(TokenType::IDENTIFIER) && peek().lexeme == "when") {
			advance();
			branch.guard = parse_expression();
		}

		consume(TokenType::COLON, "Expected ':' after " + kw + " pattern");
		branch.body = parse_suite();

		branches.push_back(std::move(branch));
	}

	skip_newlines();
	consume(TokenType::DEDENT, "Expected dedent after " + kw + " block");

	if (branches.empty()) {
		error("'" + kw + "' requires at least one pattern");
	}

	auto stmt = std::make_unique<MatchStmt>(std::move(subject), std::move(branches), is_switch);
	stmt->line = keyword.line;
	stmt->column = keyword.column;
	return stmt;
}

MatchPatternPtr Parser::parse_match_pattern() {
	auto pattern = std::make_unique<MatchPattern>();
	pattern->line = peek().line;
	pattern->column = peek().column;

	// Wildcard; matched by lexeme since '_' lexes as IDENTIFIER.
	if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
		advance();
		pattern->kind = MatchPattern::Kind::WILDCARD;
		return pattern;
	}

	if (match(TokenType::VAR)) {
		const Token& name = consume(TokenType::IDENTIFIER, "Expected a name after 'var' in a pattern");
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
	if (check(TokenType::IDENTIFIER) && peek_ahead(1).type == TokenType::LPAREN) {
		return parse_match_struct_pattern();
	}

	// Value pattern: compared against the subject by equality.
	pattern->kind = MatchPattern::Kind::VALUE;
	pattern->value = parse_expression();
	pattern->line = pattern->value->line;
	pattern->column = pattern->value->column;
	return pattern;
}

MatchPatternPtr Parser::parse_match_struct_pattern() {
	auto pattern = std::make_unique<MatchPattern>();
	const Token& name = consume(TokenType::IDENTIFIER, "Expected a struct name in the pattern");
	pattern->kind = MatchPattern::Kind::STRUCT;
	pattern->struct_name = name.lexeme;
	pattern->line = name.line;
	pattern->column = name.column;
	consume(TokenType::LPAREN, "Expected '(' after the struct name in the pattern");

	bool saw_named = false;
	while (!check(TokenType::RPAREN) && !is_at_end()) {
		MatchPattern::StructEntry entry;
		if (check(TokenType::IDENTIFIER) && peek_ahead(1).type == TokenType::ASSIGN) {
			entry.name = advance().lexeme;
			advance(); // '='
			saw_named = true;
		} else if (saw_named) {
			const Token& at = peek();
			throw CompilerException::parser_error(
				"A positional struct pattern cannot follow a named field pattern",
				at.line, at.column);
		}
		entry.value = parse_match_pattern();
		pattern->struct_entries.push_back(std::move(entry));
		if (!match(TokenType::COMMA)) {
			break;
		}
	}
	consume(TokenType::RPAREN, "Expected ')' after the struct pattern");
	return pattern;
}

bool Parser::parse_pattern_rest(const char* what) {
	if (!match(TokenType::DOT_DOT)) {
		return false;
	}
	match(TokenType::COMMA);
	if (!check(TokenType::RBRACKET) && !check(TokenType::RBRACE)) {
		const Token& at = peek();
		throw CompilerException::parser_error(
			std::string("'..' has to be the last entry of ") + what, at.line, at.column);
	}
	return true;
}

MatchPatternPtr Parser::parse_match_array_pattern() {
	auto pattern = std::make_unique<MatchPattern>();
	const Token& open_bracket = consume(TokenType::LBRACKET, "Expected '[' to start an array pattern");
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
	const Token& open_brace = consume(TokenType::LBRACE, "Expected '{' to start a dictionary pattern");
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
		// COLON introduces a value pattern; absence tests key presence only.
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
	// `await` is an expression statement; not an lvalue, so handle before parse_call().
	if (check(TokenType::AWAIT)) {
		ExprPtr expr = parse_expression();
		consume_statement_end("Expected newline after expression");
		return std::make_unique<ExprStmt>(std::move(expr));
	}

	ExprPtr lhs = parse_call();

	if (match(TokenType::ASSIGN)) {
		ExprPtr value = parse_expression();
		consume_statement_end("Expected newline after assignment");

		if (auto* var_expr = dynamic_cast<VariableExpr*>(lhs.get())) {
			return std::make_unique<AssignStmt>(var_expr->name, std::move(value));
		} else if (auto* index_expr = dynamic_cast<IndexExpr*>(lhs.get())) {
			if (index_expr->safe_chain_root) {
				throw CompilerException::parser_error(
					"Cannot assign through '?.'; write the null check out",
					lhs->line, lhs->column);
			}
			return std::make_unique<AssignStmt>(std::move(lhs), std::move(value));
		} else if (auto* member_expr = dynamic_cast<MemberCallExpr*>(lhs.get())) {
			if (member_expr->is_method_call) {
				throw CompilerException::parser_error("Cannot assign to method call", lhs->line, lhs->column);
			}
			if (member_expr->safe) {
				throw CompilerException::parser_error(
					"Cannot assign through '?.'; write the null check out",
					lhs->line, lhs->column);
			}
			return std::make_unique<AssignStmt>(std::move(lhs), std::move(value));
		} else {
			throw CompilerException::parser_error("Invalid assignment target", lhs->line, lhs->column);
		}
	}

	// Compound assignment: `a op= b` -> `a = a op b` via clone_lvalue().
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
		ExprPtr read = clone_lvalue(lhs.get());
		if (!read) {
			throw CompilerException::parser_error(
				"Invalid target for compound assignment", lhs->line, lhs->column);
		}
		advance();

		ExprPtr rhs = parse_expression();
		ExprPtr combined = make_binary(std::move(read), entry.op, std::move(rhs));
		consume_statement_end("Expected newline after assignment");

		if (auto* var_expr = dynamic_cast<VariableExpr*>(lhs.get())) {
			return std::make_unique<AssignStmt>(var_expr->name, std::move(combined));
		}
		return std::make_unique<AssignStmt>(std::move(lhs), std::move(combined));
	}

	consume_statement_end("Expected newline after expression");
	return std::make_unique<ExprStmt>(std::move(lhs));
}

ExprPtr Parser::clone_lvalue(const Expr* expr) {
	// Only pure lookups are cloneable; calls would re-evaluate.
	if (auto* var = dynamic_cast<const VariableExpr*>(expr)) {
		return make_like<VariableExpr>(*expr, var->name);
	}
	if (auto* lit = dynamic_cast<const LiteralExpr*>(expr)) {
		auto copy = std::make_unique<LiteralExpr>(*lit);
		copy->line = expr->line;
		copy->column = expr->column;
		return copy;
	}
	if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
		if (!call->is_node_path_sugar) {
			return nullptr;
		}
		std::vector<ExprPtr> arguments;
		arguments.reserve(call->arguments.size());
		for (const auto& argument : call->arguments) {
			ExprPtr copy = clone_lvalue(argument.get());
			if (!copy) {
				return nullptr;
			}
			arguments.push_back(std::move(copy));
		}
		auto copy = make_like<CallExpr>(*expr, call->function_name, std::move(arguments));
		copy->is_node_path_sugar = true;
		return copy;
	}
	if (auto* member = dynamic_cast<const MemberCallExpr*>(expr)) {
		if (member->is_method_call || member->safe) {
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
	if (!tolerant()) {
		return parse_expression_impl();
	}
	const int line = peek().line;
	const int column = peek().column;
	try {
		return parse_expression_impl();
	} catch (const Recovery&) {
		// Skip to the next statement boundary.
		while (!is_at_end()) {
			switch (peek().type) {
				case TokenType::NEWLINE: case TokenType::SEMICOLON: case TokenType::COLON:
				case TokenType::INDENT: case TokenType::DEDENT: case TokenType::COMMA:
				case TokenType::RPAREN: case TokenType::RBRACKET: case TokenType::RBRACE:
					break;
				default:
					advance();
					continue;
			}
			break;
		}
		auto node = std::make_unique<ErrorExpr>();
		node->line = line;
		node->column = column;
		return node;
	}
}

ExprPtr Parser::parse_expression_impl() {
	ExprPtr expr = parse_ternary();

	// `as` is the loosest operator: casts the entire preceding expression.
	while (true) {
		if (match(TokenType::AS)) {
			std::vector<TypeExpr> type_arguments;
			const std::string type_name = parse_type_name(&type_arguments);
			if (check(TokenType::QUESTION)) {
				error("'as' cannot take a nullable type; the result of 'as' is already nullable");
			}
			const Expr& start = *expr;
			expr = make_like<CastExpr>(start, std::move(expr), type_name,
				std::move(type_arguments));
			continue;
		}
		// A cast is the only expression `??` can meet here: everything else was
		// already taken by parse_coalesce, which binds tighter than `as`.
		if (match(TokenType::QUESTION_QUESTION)) {
			ExprPtr fallback = parse_expression_impl();
			expr = make_binary(std::move(expr), BinaryExpr::Op::COALESCE, std::move(fallback));
			continue;
		}
		break;
	}

	return expr;
}

ExprPtr Parser::parse_ternary() {
	ExprPtr true_value = parse_coalesce();

	if (!match(TokenType::IF)) {
		return true_value;
	}

	ExprPtr condition = parse_coalesce();
	consume(TokenType::ELSE, "Expected 'else' in conditional expression");
	ExprPtr false_value = parse_ternary(); // right-associative
	const Expr& start = *true_value;
	return make_like<TernaryExpr>(start, std::move(condition), std::move(true_value), std::move(false_value));
}

// `a ?? b`: right-associative, so a chain of fallbacks reads left to right.
ExprPtr Parser::parse_coalesce() {
	ExprPtr left = parse_or_expression();

	if (!match(TokenType::QUESTION_QUESTION)) {
		return left;
	}
	ExprPtr right = parse_coalesce();
	return make_binary(std::move(left), BinaryExpr::Op::COALESCE, std::move(right));
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
	// `not` binds looser than comparison: `not a == b` is `not (a == b)`.
	if (match(TokenType::NOT)) {
		const Token& op = previous();
		return make_at<UnaryExpr>(op, UnaryExpr::Op::NOT, parse_not());
	}
	return parse_inclusion();
}

ExprPtr Parser::parse_inclusion() {
	ExprPtr left = parse_equality();

	while (true) {
		// `not in` is two tokens; disambiguated by lookahead.
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
		const Token& op = previous();
		ExprPtr right = parse_comparison();

		BinaryExpr::Op bin_op = (op.type == TokenType::EQUAL) ? BinaryExpr::Op::EQ : BinaryExpr::Op::NEQ;
		left = make_binary(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_comparison() {
	ExprPtr left = parse_bit_or();

	while (match_one_of({TokenType::LESS, TokenType::LESS_EQUAL, TokenType::GREATER, TokenType::GREATER_EQUAL})) {
		const Token& op = previous();
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
		const Token& op = previous();
		ExprPtr right = parse_term();

		BinaryExpr::Op bin_op = (op.type == TokenType::SHIFT_LEFT) ? BinaryExpr::Op::SHL : BinaryExpr::Op::SHR;
		left = make_binary(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_term() {
	ExprPtr left = parse_factor();

	while (match_one_of({TokenType::PLUS, TokenType::MINUS})) {
		const Token& op = previous();
		ExprPtr right = parse_factor();

		BinaryExpr::Op bin_op = (op.type == TokenType::PLUS) ? BinaryExpr::Op::ADD : BinaryExpr::Op::SUB;
		left = make_binary(std::move(left), bin_op, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_factor() {
	ExprPtr left = parse_type_test();

	while (match_one_of({TokenType::MULTIPLY, TokenType::DIVIDE, TokenType::MODULO})) {
		const Token& op = previous();
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
	// `await` at unary precedence, matching GDScript.
	if (check(TokenType::AWAIT)) {
		const Token& op = advance();
		m_saw_await = true;
		return make_at<AwaitExpr>(op, parse_unary());
	}
	if (match_one_of({TokenType::MINUS, TokenType::BIT_NOT, TokenType::PLUS})) {
		const Token& op = previous();
		ExprPtr operand = parse_unary();

		switch (op.type) {
			case TokenType::MINUS:
				return make_at<UnaryExpr>(op, UnaryExpr::Op::NEG, std::move(operand));
			case TokenType::BIT_NOT:
				return make_at<UnaryExpr>(op, UnaryExpr::Op::BIT_NOT, std::move(operand));
			default:
				return operand; // unary '+' is a no-op
		}
	}

	return parse_call();
}

ExprPtr Parser::parse_power() {
	ExprPtr left = parse_unary();

	// Left-associative, looser than unary '-'. Engine is authoritative over manual.
	while (match(TokenType::POWER)) {
		ExprPtr right = parse_unary();
		left = make_binary(std::move(left), BinaryExpr::Op::POW, std::move(right));
	}

	return left;
}

ExprPtr Parser::parse_type_test() {
	ExprPtr left = parse_power();

	while (check(TokenType::IS)) {
		advance();
		const bool negated = match(TokenType::NOT);
		TypeExpr type = parse_type_expr();
		const Expr& start = *left;
		left = make_like<TypeTestExpr>(start, std::move(left), std::move(type));
		if (negated) {
			const Expr& test = *left;
			left = make_like<UnaryExpr>(test, UnaryExpr::Op::NOT, std::move(left));
		}
	}

	return left;
}

ExprPtr Parser::parse_call() {
	ExprPtr expr = parse_primary();
	// A postfix chain short-circuits as a whole, so the chain -- not the link
	// that tested -- owns the null result.
	bool safe_link = false;

	while (true) {
		if (match(TokenType::QUESTION_DOT)) {
			const Token& member = consume(TokenType::IDENTIFIER,
				"Expected property or method name after '?.'");
			std::vector<ExprPtr> arguments;
			std::vector<std::string> names;
			const bool is_method = match(TokenType::LPAREN);
			if (is_method) {
				parse_argument_list(arguments, names);
			}
			auto call = make_like<MemberCallExpr>(*expr, std::move(expr), member.lexeme,
				std::move(arguments), is_method);
			call->argument_names = std::move(names);
			call->safe = true;
			expr = std::move(call);
			safe_link = true;
			continue;
		}
		if (match(TokenType::LPAREN)) {
			std::vector<ExprPtr> arguments;
			std::vector<std::string> names;
			parse_argument_list(arguments, names);

			if (auto* var_expr = dynamic_cast<VariableExpr*>(expr.get())) {
				std::string func_name = var_expr->name;
				auto call = make_like<CallExpr>(*expr, func_name, std::move(arguments));
				call->argument_names = std::move(names);
				expr = std::move(call);
			} else {
				// Non-identifier callee: lower to `.call()` (Callable VCALL).
				for (const std::string& name : names) {
					if (!name.empty()) {
						error("A call on an expression cannot name its arguments");
					}
				}
				expr = make_like<MemberCallExpr>(*expr, std::move(expr), "call",
					std::move(arguments), true);
			}
		} else if (match(TokenType::DOT)) {
			const Token& member = consume(TokenType::IDENTIFIER, "Expected property or method name after '.'");

			if (match(TokenType::LPAREN)) {
				std::vector<ExprPtr> arguments;
				std::vector<std::string> names;
				parse_argument_list(arguments, names);

				auto call = make_like<MemberCallExpr>(*expr, std::move(expr), member.lexeme,
					std::move(arguments), true);
				call->argument_names = std::move(names);
				expr = std::move(call);
			} else {
				expr = make_like<MemberCallExpr>(*expr, std::move(expr), member.lexeme, std::vector<ExprPtr>{}, false);
			}
		} else if (match(TokenType::LBRACKET)) {
			ExprPtr index = parse_expression();
			consume(TokenType::RBRACKET, "Expected ']' after index");
			expr = make_like<IndexExpr>(*expr, std::move(expr), std::move(index));
		} else {
			break;
		}
	}

	if (safe_link) {
		if (auto* member = dynamic_cast<MemberCallExpr*>(expr.get())) {
			member->safe_chain_root = true;
		} else if (auto* index = dynamic_cast<IndexExpr*>(expr.get())) {
			index->safe_chain_root = true;
		}
	}

	return expr;
}

// $Name, $Path/To/Node, $"quoted/path", $%Unique/Child, %Unique.
// Segments after the first are IDENTIFIER / DIVIDE token pairs.
ExprPtr Parser::parse_node_path() {
	const Token& marker = advance();
	const bool unique = marker.type == TokenType::MODULO;

	std::string path;
	if (!unique && (check(TokenType::STRING) || check(TokenType::NODE_PATH))) {
		path = std::get<std::string>(advance().value);
	} else {
		bool first = true;
		do {
			std::string prefix;
			if (match(TokenType::MODULO)) {
				prefix = "%";
			} else if (first && unique) {
				prefix = "%";
			}
			const Token& name = consume(TokenType::IDENTIFIER,
				std::string("Expected a node name after '") +
				(first ? (unique ? "%" : "$") : "/") + "'");
			if (!first) {
				path += "/";
			}
			path += prefix + name.lexeme;
			first = false;
		} while (match(TokenType::DIVIDE));
	}

	std::vector<ExprPtr> arguments;
	arguments.push_back(make_at<LiteralExpr>(marker, path));
	auto call = std::make_unique<CallExpr>("get_node", std::move(arguments));
	call->line = marker.line;
	call->column = marker.column;
	call->is_node_path_sugar = true;
	return call;
}

ExprPtr Parser::parse_lambda() {
	const Token& func_token = consume(TokenType::FUNC, "Expected 'func'");

	auto decl = std::make_unique<FunctionDecl>();
	decl->line = func_token.line;
	decl->column = func_token.column;
	if (check(TokenType::IDENTIFIER)) {
		decl->name = advance().lexeme;
	}

	consume(TokenType::LPAREN, "Expected '(' after 'func' in a lambda");
	decl->parameters = parse_parameters();
	consume(TokenType::RPAREN, "Expected ')' after lambda parameters");

	decl->return_type = parse_return_type();
	consume(TokenType::COLON, "Expected ':' after lambda signature");

	// Scope await tracking to the lambda body.
	const bool enclosing_saw_await = m_saw_await;
	m_saw_await = false;
	decl->body = parse_suite();
	decl->is_coroutine = m_saw_await;
	m_saw_await = enclosing_saw_await;

	return make_at<LambdaExpr>(func_token, std::move(decl));
}

ExprPtr Parser::parse_primary() {
	if (check(TokenType::FUNC)) {
		return parse_lambda();
	}

	if (match(TokenType::TRUE)) {
		return make_at<LiteralExpr>(previous(), true);
	}
	if (match(TokenType::FALSE)) {
		return make_at<LiteralExpr>(previous(), false);
	}
	if (match(TokenType::NULL_VAL)) {
		const Token& token = previous();
		auto node = LiteralExpr::null();
		node->line = token.line;
		node->column = token.column;
		return node;
	}

	if (match(TokenType::INTEGER)) {
		const Token& num = previous();
		return make_at<LiteralExpr>(num, std::get<int64_t>(num.value));
	}

	if (match(TokenType::FLOAT)) {
		const Token& num = previous();
		return make_at<LiteralExpr>(num, std::get<double>(num.value));
	}

	if (match(TokenType::STRING)) {
		const Token& str = previous();
		return make_at<LiteralExpr>(str, std::get<std::string>(str.value));
	}

	if (match(TokenType::STRING_NAME) || match(TokenType::NODE_PATH)) {
		const Token& str = previous();
		auto node = make_at<LiteralExpr>(str, std::get<std::string>(str.value));
		node->string_type = str.type == TokenType::STRING_NAME
			? LiteralExpr::StringType::STRING_NAME
			: LiteralExpr::StringType::NODE_PATH;
		return node;
	}

	// $Node, $"a/b", %Unique → get_node(). `%` unambiguous in expression position.
	if (check(TokenType::DOLLAR) || check(TokenType::MODULO)) {
		return parse_node_path();
	}

	if (match(TokenType::IDENTIFIER)) {
		const Token& name = previous();
		return make_at<VariableExpr>(name, name.lexeme);
	}

	if (match(TokenType::LPAREN)) {
		ExprPtr expr = parse_expression();
		consume(TokenType::RPAREN, "Expected ')' after expression");
		return expr;
	}

	if (match(TokenType::LBRACKET)) {
		const Token& bracket = previous();
		std::vector<ExprPtr> elements;

		if (!check(TokenType::RBRACKET)) {
			do {
				elements.push_back(parse_expression());
				if (!match(TokenType::COMMA)) {
					break;
				}
			} while (!check(TokenType::RBRACKET));
		}

		consume(TokenType::RBRACKET, "Expected ']' after array elements");
		return make_at<ArrayLiteralExpr>(bracket, std::move(elements));
	}

	if (match(TokenType::LBRACE)) {
		const Token& brace = previous();
		std::vector<std::pair<ExprPtr, ExprPtr>> elements;

		enum class DictStyle { UNKNOWN, LUA, PYTHON };
		DictStyle style = DictStyle::UNKNOWN;

		if (!check(TokenType::RBRACE)) {
			do {
				const bool lua_here =
					(check(TokenType::IDENTIFIER) || check(TokenType::STRING)) &&
					peek_ahead(1).type == TokenType::ASSIGN;
				const bool first = elements.empty();
				if (first) {
					style = lua_here ? DictStyle::LUA : DictStyle::PYTHON;
				}

				ExprPtr key;
				if (style == DictStyle::LUA) {
					if (!lua_here) {
						error("Expected '=' after dictionary key. "
							"Mixing dictionary styles is not allowed");
					}
					const Token& name = advance();
					key = name.type == TokenType::IDENTIFIER
						? make_at<LiteralExpr>(name, name.lexeme)
						: make_at<LiteralExpr>(name, std::get<std::string>(name.value));
					advance(); // the `=`
				} else {
					key = parse_expression();
					if (check(TokenType::ASSIGN)) {
						error(first
							? "Expected an identifier or a string as a Lua-style "
								"dictionary key (e.g. '{ key = value }')"
							: "Expected ':' after dictionary key. "
								"Mixing dictionary styles is not allowed");
					}
					consume(TokenType::COLON, "Expected ':' after dictionary key");
				}
				ExprPtr value = parse_expression();
				elements.push_back({std::move(key), std::move(value)});
				if (!match(TokenType::COMMA)) {
					break;
				}
			} while (!check(TokenType::RBRACE));
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

const Token& Parser::advance() {
	if (!is_at_end()) m_current++;
	return previous();
}

const Token& Parser::peek() const {
	return m_tokens[m_current];
}

const Token& Parser::peek_ahead(size_t offset) const {
	const size_t index = m_current + offset;
	// Clamp to EOF; stream always ends in EOF_TOKEN.
	return m_tokens[index < m_tokens.size() ? index : m_tokens.size() - 1];
}

const Token& Parser::previous() const {
	return m_tokens[m_current - 1];
}

bool Parser::is_at_end() const {
	return peek().type == TokenType::EOF_TOKEN;
}

const Token& Parser::consume(TokenType type, const std::string& message) {
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

namespace {
const char* diagnostic_code(const std::string& message) {
	if (message.compare(0, 19, "Expected expression") == 0) return "EXPECTED_EXPRESSION";
	if (message.find("Expected ':'") != std::string::npos) return "EXPECTED_COLON";
	if (message.find("Expected a type") != std::string::npos) return "EXPECTED_TYPE";
	if (message.find("Expected newline") != std::string::npos) return "EXPECTED_NEWLINE";
	return "PARSE_ERROR";
}
} // namespace

void Parser::error(const std::string& message) {
	const Token& token = peek();
	if (tolerant()) {
		const int width = int(token.lexeme.size());
		const int start = token.column - width;
		m_diagnostics->add(diagnostic_code(message), message, token.line,
			start > 0 ? start : token.column, token.line,
			start > 0 ? token.column : token.column + 1);
		throw Recovery{};
	}
	throw CompilerException(ErrorType::PARSER_ERROR, message, token.line, token.column);
}

void Parser::error(const std::string& message, int line, int column) {
	if (tolerant()) {
		m_diagnostics->add(diagnostic_code(message), message, line, column, line, column + 1);
		throw Recovery{};
	}
	throw CompilerException(ErrorType::PARSER_ERROR, message, line, column);
}

void Parser::warn(std::string code, std::string message, int line, int column, int width) {
	static constexpr size_t MAX_WARNINGS = 100;
	if (m_warnings.size() >= MAX_WARNINGS) {
		return;
	}
	if (line < 1) line = 1;
	if (column < 1) column = 1;
	if (width < 1) width = 1;
	m_warnings.push_back({std::move(code), std::move(message), line, column, line,
		column + width});
}

// Leaves layout tokens intact so DEDENT still closes its suite.
void Parser::recover_to_statement_end() {
	while (!is_at_end()) {
		const TokenType type = peek().type;
		if (type == TokenType::DEDENT || type == TokenType::INDENT) {
			return;
		}
		advance();
		if (type == TokenType::NEWLINE || type == TokenType::SEMICOLON) {
			return;
		}
	}
}

void Parser::skip_newlines() {
	while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {
		// Skip
	}
}

void Parser::consume_statement_end(const std::string& message) {
	if (match(TokenType::SEMICOLON)) {
		return;
	}
	if (m_inline_suite_depth > 0 && at_inline_suite_end()) {
		return;
	}
	if (check(TokenType::DEDENT)) {
		return;
	}
	if (previous().type == TokenType::DEDENT) {
		return;
	}
	consume(TokenType::NEWLINE, message);
}

TypeExpr Parser::parse_type_hint() {
	if (match(TokenType::COLON)) {
		if (check(TokenType::IDENTIFIER) || check(TokenType::NULL_VAL)) {
			return parse_type_expr();
		}
	}
	return {};
}

TypeExpr Parser::parse_type_expr() {
	TypeExpr type;
	Token type_end;
	auto member = [&]() {
		if (match(TokenType::NULL_VAL)) {
			if (type.nullable) {
				error("Union type repeats member 'null'");
			}
			type.nullable = true;
			type_end = previous();
			return;
		}
		type.names.push_back(parse_type_name(type.names.empty() ? &type.arguments : nullptr));
		type_end = previous();
	};

	member();
	while (match(TokenType::BIT_OR)) {
		if (!check(TokenType::IDENTIFIER) && !check(TokenType::NULL_VAL)) {
			error("Expected a type name or 'null' after '|'");
		}
		member();
	}

	// `T??` is the doubled suffix, not a type followed by the `??` operator: the
	// operator form needs a space, the same way `?` must touch the type name.
	if (check(TokenType::QUESTION_QUESTION) && type_end.line == peek().line &&
		type_end.column + 2 == peek().column) {
		error("Unexpected second '?'");
	}

	if (match(TokenType::QUESTION)) {
		const Token& question = previous();
		// Token columns point just past the token, so adjacent one-character '?'
		// ends exactly one column after the type's final token.
		if (type_end.line != question.line || type_end.column + 1 != question.column) {
			error("'?' must directly follow the type name");
		}
		if (type.nullable) {
			error("'?' on a type that already includes 'null'");
		}
		if (type.is_union()) {
			error("'?' cannot follow a union member. Write 'A | B | null'");
		}
		if (type.names.size() == 1 && type.names.front() == "Variant") {
			error("'Variant' already includes 'null'");
		}
		type.nullable = true;
		type.spelled_nullable = true;
		if (check(TokenType::QUESTION)) {
			error("Unexpected second '?'");
		}
		if (check(TokenType::BIT_OR)) {
			error("'?' must come last in a type");
		}
	}

	// Qualified engine types are deliberately untyped in this compiler. A suffix
	// must not turn that compatibility fallback into the invalid type `null`.
	if (type.names.size() == 1 && type.names.front().empty()) {
		type.nullable = false;
		type.spelled_nullable = false;
	}
	return type;
}

// Qualified names (A.B) are dropped; only the engine can resolve them.
std::string Parser::parse_type_name(std::vector<TypeExpr>* arguments) {
	const Token& type_token = consume(TokenType::IDENTIFIER, "Expected type name");
	std::string qualified;
	while (match(TokenType::DOT)) {
		const Token& part = consume(TokenType::IDENTIFIER, "Expected a type name after '.'");
		if (qualified.empty()) {
			qualified = type_token.lexeme;
		}
		qualified += '.';
		qualified += part.lexeme;
	}
	if (arguments != nullptr && match(TokenType::LBRACKET)) {
		if (!check(TokenType::RBRACKET)) {
			do {
				if (!check(TokenType::IDENTIFIER) && !check(TokenType::NULL_VAL)) {
					error("Expected a type argument");
				}
				arguments->push_back(parse_type_expr());
			} while (match(TokenType::COMMA));
		}
		consume(TokenType::RBRACKET, "Expected ']' to close the element type");
	} else {
		skip_type_arguments();
	}
	if (!qualified.empty()) {
		warn("UNRESOLVED_TYPE_HINT", "Qualified type '" + qualified +
			"' is not resolved yet; the declaration stays untyped",
			type_token.line, type_token.column - int(type_token.lexeme.size()),
			int(qualified.size()));
		return std::string();
	}
	return type_token.lexeme;
}

TypeExpr Parser::parse_return_type() {
	// `->` is MINUS + GREATER; no dedicated arrow token.
	size_t saved_pos = m_current;

	if (match(TokenType::MINUS)) {
		if (match(TokenType::GREATER)) {
			if (check(TokenType::IDENTIFIER) || check(TokenType::NULL_VAL)) {
				return parse_type_expr();
			}
			return {};
		}
		m_current = saved_pos;
	}

	return {};
}

void Parser::skip_type_arguments() {
	// Element types not checked; every value is a Variant.
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

bool Parser::parse_attribute(ExportHint& hint, bool* is_onready,
	std::optional<RPCConfig>* rpc_config, bool* is_abstract, bool* is_test) {
	consume(TokenType::AT, "Expected '@' for attribute");

	const Token& name = consume(TokenType::IDENTIFIER, "Expected an attribute name after '@'");
	const std::vector<ExportArgument> arguments = parse_attribute_arguments();

	// Inspector-section annotations; not tied to a declaration.
	if (name.lexeme == "export_group" || name.lexeme == "export_subgroup" ||
		name.lexeme == "export_category")
	{
		const size_t max_args = name.lexeme == "export_category" ? 1 : 2;
		if (arguments.empty() || arguments.size() > max_args) {
			error("@" + name.lexeme + " takes a name" +
				(max_args == 2 ? " and an optional prefix" : ""), name.line, name.column);
			return false;
		}
		for (const ExportArgument& argument : arguments) {
			if (argument.kind != ExportArgument::Kind::STRING) {
				error("@" + name.lexeme + " takes strings; this argument is not one",
					argument.line, argument.column);
				return false;
			}
		}
		const std::string prefix = arguments.size() > 1 ? arguments[1].text : std::string();
		if (name.lexeme == "export_category") {
			m_export_section = ExportSection{arguments[0].text, "", "", "", ""};
		} else if (name.lexeme == "export_group") {
			m_export_section.group = arguments[0].text;
			m_export_section.group_prefix = prefix;
			m_export_section.subgroup.clear();
			m_export_section.subgroup_prefix.clear();
		} else {
			m_export_section.subgroup = arguments[0].text;
			m_export_section.subgroup_prefix = prefix;
		}
		return false;
	}
	if (name.lexeme.rfind("export", 0) == 0) {
		std::string error;
		ExportHint parsed;
		if (build_export_hint(name.lexeme, arguments, parsed, error)) {
			if (!error.empty()) {
				this->error(error, name.line, name.column);
			} else if (!parsed.is_default()) {
				hint = parsed;
			}
		} else {
			warn("UNSUPPORTED_ANNOTATION", "@" + name.lexeme +
				" is not implemented yet; it acts as a plain @export",
				name.line, name.column - int(name.lexeme.size()) - 1,
				int(name.lexeme.size()) + 1);
		}
		return true;
	}
	if (name.lexeme == "onready") {
		if (is_onready == nullptr) {
			error("@onready is for a member variable; a local is assigned where it is declared",
				name.line, name.column);
		} else {
			*is_onready = true;
		}
		return false;
	}
	if (name.lexeme == "tool") {
		m_saw_tool = true;
		return false;
	}
	if (name.lexeme == "abstract") {
		if (is_abstract == nullptr) {
			error("@abstract is only supported on a trait method", name.line, name.column);
		}
		*is_abstract = true;
		return false;
	}
	if (name.lexeme == "test") {
		if (is_test == nullptr) {
			error("@test is for a file-level function", name.line, name.column);
			return false;
		}
		if (*is_test) {
			error("@test can only be used once per function", name.line, name.column);
			return false;
		}
		if (!arguments.empty()) {
			error("@test takes no arguments", name.line, name.column);
			return false;
		}
		*is_test = true;
		return false;
	}
	if (name.lexeme == "rpc") {
		if (rpc_config == nullptr) {
			error("@rpc can only be applied to a function", name.line, name.column);
			return false;
		}
		if (rpc_config->has_value()) {
			error("@rpc can only be used once per function", name.line, name.column);
			return false;
		}
		if (arguments.size() > 4) {
			error("@rpc accepts at most four arguments", name.line, name.column);
			return false;
		}

		RPCConfig config;
		unsigned locality_args = 0;
		unsigned permission_args = 0;
		unsigned transfer_args = 0;
		for (size_t i = 0; i < arguments.size(); i++) {
			const ExportArgument& argument = arguments[i];
			if (i == 3) {
				const double channel = argument.number;
				if (argument.kind != ExportArgument::Kind::NUMBER || channel < 0.0 ||
						channel > double(INT32_MAX) || channel != double(int32_t(channel))) {
					error("The fourth @rpc argument must be a non-negative integer channel",
						argument.line, argument.column);
					return false;
				}
				config.channel = int32_t(channel);
				continue;
			}
			if (argument.kind != ExportArgument::Kind::STRING) {
				error("The first three @rpc arguments must be string options",
					argument.line, argument.column);
				return false;
			}
			if (argument.text == "call_local") {
				locality_args++;
				config.call_local = true;
			} else if (argument.text == "call_remote") {
				locality_args++;
				config.call_local = false;
			} else if (argument.text == "any_peer") {
				permission_args++;
				config.rpc_mode = 1;
			} else if (argument.text == "authority") {
				permission_args++;
				config.rpc_mode = 2;
			} else if (argument.text == "unreliable") {
				transfer_args++;
				config.transfer_mode = 0;
			} else if (argument.text == "unreliable_ordered") {
				transfer_args++;
				config.transfer_mode = 1;
			} else if (argument.text == "reliable") {
				transfer_args++;
				config.transfer_mode = 2;
			} else {
				error("Invalid @rpc option: " + argument.text, argument.line, argument.column);
				return false;
			}
		}
		if (locality_args > 1 || permission_args > 1 || transfer_args > 1) {
			error("Each @rpc option category can be specified only once", name.line, name.column);
			return false;
		}
		*rpc_config = std::move(config);
		return false;
	}
	if (name.lexeme == "icon" ||
		name.lexeme == "warning_ignore" || name.lexeme == "warning_ignore_start" ||
		name.lexeme == "warning_ignore_restore" || name.lexeme == "static_unload" ||
		false)
	{
		return false;
	}

	error("Unknown attribute: @" + name.lexeme);
	return false;
}

std::vector<ExportArgument> Parser::parse_attribute_arguments() {
	std::vector<ExportArgument> arguments;
	if (!match(TokenType::LPAREN)) {
		return arguments;
	}

	int depth = 1;
	std::vector<Token> chunk;
	const auto end_chunk = [&]() {
		if (chunk.empty()) {
			return;
		}
		ExportArgument arg;
		arg.line = chunk.front().line;
		arg.column = chunk.front().column;
		arg.kind = ExportArgument::Kind::OTHER;

		size_t at = 0;
		double sign = 1.0;
		if (chunk.size() == 2 && chunk[0].is_one_of(TokenType::MINUS, TokenType::PLUS)) {
			sign = chunk[0].type == TokenType::MINUS ? -1.0 : 1.0;
			at = 1;
		}
		if (chunk.size() - at == 1) {
			const Token& token = chunk[at];
			if (token.type == TokenType::INTEGER) {
				arg.kind = ExportArgument::Kind::NUMBER;
				arg.number = sign * double(std::get<int64_t>(token.value));
			} else if (token.type == TokenType::FLOAT) {
				arg.kind = ExportArgument::Kind::NUMBER;
				arg.number = sign * std::get<double>(token.value);
			} else if (token.type == TokenType::STRING && at == 0) {
				arg.kind = ExportArgument::Kind::STRING;
				arg.text = std::get<std::string>(token.value);
			} else if (token.type == TokenType::IDENTIFIER && at == 0) {
				arg.kind = ExportArgument::Kind::NAME;
				arg.text = token.lexeme;
			}
		}
		arguments.push_back(std::move(arg));
		chunk.clear();
	};

	while (depth > 0 && !is_at_end()) {
		if (check(TokenType::LPAREN)) {
			depth++;
			chunk.push_back(advance());
		} else if (check(TokenType::RPAREN)) {
			depth--;
			if (depth == 0) {
				advance();
				break;
			}
			chunk.push_back(advance());
		} else if (depth == 1 && check(TokenType::COMMA)) {
			advance();
			end_chunk();
		} else if (check(TokenType::NEWLINE) || check(TokenType::INDENT) || check(TokenType::DEDENT)) {
			advance();
		} else {
			chunk.push_back(advance());
		}
	}
	end_chunk();

	if (depth != 0) {
		error("Expected ')' to close the attribute arguments");
	}
	return arguments;
}

SignalDecl Parser::parse_signal() {
	SignalDecl decl;
	const Token& signal_token = consume(TokenType::SIGNAL, "Expected 'signal'");
	decl.line = signal_token.line;
	decl.column = signal_token.column;
	decl.doc_comment = doc_comment_above(signal_token.line);

	const Token& name = consume(TokenType::IDENTIFIER, "Expected a signal name after 'signal'");
	decl.name = name.lexeme;

	if (match(TokenType::LPAREN)) {
		decl.parameters = parse_parameters();
		consume(TokenType::RPAREN, "Expected ')' after the signal parameters");
		for (const Parameter& param : decl.parameters) {
			if (param.default_value) {
				error("Signal parameters cannot have a default value", param.line, param.column);
			}
		}
	}

	consume_statement_end("Expected newline after signal declaration");
	return decl;
}

} // namespace gdscript
