#include "ast_clone.h"
#include <stdexcept>

namespace gdscript {
namespace {

template <typename To, typename From>
std::unique_ptr<To> positioned(std::unique_ptr<To> out, const From* from) {
	out->line = from->line;
	out->column = from->column;
	return out;
}

std::vector<ExprPtr> clone_exprs(const std::vector<ExprPtr>& values) {
	std::vector<ExprPtr> out;
	out.reserve(values.size());
	for (const ExprPtr& value : values) out.push_back(clone_expr(value.get()));
	return out;
}

std::vector<StmtPtr> clone_stmts(const std::vector<StmtPtr>& values) {
	std::vector<StmtPtr> out;
	out.reserve(values.size());
	for (const StmtPtr& value : values) out.push_back(clone_stmt(value.get()));
	return out;
}

} // namespace

ExprPtr clone_expr(const Expr* expr) {
	if (expr == nullptr) return nullptr;
	if (const auto* value = dynamic_cast<const LiteralExpr*>(expr)) {
		std::unique_ptr<LiteralExpr> out;
		switch (value->lit_type) {
			case LiteralExpr::Type::INTEGER: out = std::make_unique<LiteralExpr>(std::get<int64_t>(value->value)); break;
			case LiteralExpr::Type::FLOAT: out = std::make_unique<LiteralExpr>(std::get<double>(value->value)); break;
			case LiteralExpr::Type::STRING: out = std::make_unique<LiteralExpr>(std::get<std::string>(value->value)); break;
			case LiteralExpr::Type::BOOL: out = std::make_unique<LiteralExpr>(std::get<bool>(value->value)); break;
			case LiteralExpr::Type::NULL_VAL: out = LiteralExpr::null(); break;
		}
		out->string_type = value->string_type;
		return positioned(std::move(out), expr);
	}
	if (const auto* value = dynamic_cast<const VariableExpr*>(expr))
		return positioned(std::make_unique<VariableExpr>(value->name), expr);
	if (const auto* value = dynamic_cast<const BinaryExpr*>(expr))
		return positioned(std::make_unique<BinaryExpr>(clone_expr(value->left.get()), value->op,
			clone_expr(value->right.get())), expr);
	if (const auto* value = dynamic_cast<const AwaitExpr*>(expr))
		return positioned(std::make_unique<AwaitExpr>(clone_expr(value->operand.get())), expr);
	if (const auto* value = dynamic_cast<const LambdaExpr*>(expr)) {
		auto out = std::make_unique<LambdaExpr>(std::make_unique<FunctionDecl>(clone_function(*value->decl)));
		out->lifted_name = value->lifted_name;
		return positioned(std::move(out), expr);
	}
	if (const auto* value = dynamic_cast<const UnaryExpr*>(expr))
		return positioned(std::make_unique<UnaryExpr>(value->op, clone_expr(value->operand.get())), expr);
	if (const auto* value = dynamic_cast<const TypeTestExpr*>(expr))
		return positioned(std::make_unique<TypeTestExpr>(clone_expr(value->value.get()), value->type), expr);
	if (const auto* value = dynamic_cast<const CastExpr*>(expr))
		return positioned(std::make_unique<CastExpr>(clone_expr(value->value.get()), value->type_name,
			value->type_arguments), expr);
	if (const auto* value = dynamic_cast<const TernaryExpr*>(expr))
		return positioned(std::make_unique<TernaryExpr>(clone_expr(value->condition.get()),
			clone_expr(value->true_value.get()), clone_expr(value->false_value.get())), expr);
	if (const auto* value = dynamic_cast<const CallExpr*>(expr)) {
		auto out = std::make_unique<CallExpr>(value->function_name, clone_exprs(value->arguments));
		out->argument_names = value->argument_names;
		out->is_node_path_sugar = value->is_node_path_sugar;
		return positioned(std::move(out), expr);
	}
	if (const auto* value = dynamic_cast<const MemberCallExpr*>(expr)) {
		auto out = std::make_unique<MemberCallExpr>(clone_expr(value->object.get()), value->member_name,
			clone_exprs(value->arguments), value->is_method_call);
		out->argument_names = value->argument_names;
		out->safe = value->safe;
		out->safe_chain_root = value->safe_chain_root;
		return positioned(std::move(out), expr);
	}
	if (const auto* value = dynamic_cast<const IndexExpr*>(expr)) {
		auto out = std::make_unique<IndexExpr>(clone_expr(value->object.get()),
			clone_expr(value->index.get()));
		out->safe_chain_root = value->safe_chain_root;
		return positioned(std::move(out), expr);
	}
	if (const auto* value = dynamic_cast<const ArrayLiteralExpr*>(expr))
		return positioned(std::make_unique<ArrayLiteralExpr>(clone_exprs(value->elements)), expr);
	if (const auto* value = dynamic_cast<const DictionaryLiteralExpr*>(expr)) {
		std::vector<std::pair<ExprPtr, ExprPtr>> entries;
		for (const auto& entry : value->elements)
			entries.emplace_back(clone_expr(entry.first.get()), clone_expr(entry.second.get()));
		return positioned(std::make_unique<DictionaryLiteralExpr>(std::move(entries)), expr);
	}
	throw std::logic_error("Unhandled expression in trait AST clone");
}

MatchPatternPtr clone_pattern(const MatchPattern* pattern) {
	if (pattern == nullptr) return nullptr;
	auto out = std::make_unique<MatchPattern>();
	out->kind = pattern->kind;
	out->value = clone_expr(pattern->value.get());
	out->name = pattern->name;
	for (const auto& element : pattern->elements) out->elements.push_back(clone_pattern(element.get()));
	for (const auto& entry : pattern->entries)
		out->entries.push_back({clone_expr(entry.key.get()), clone_pattern(entry.value.get())});
	out->struct_name = pattern->struct_name;
	for (const auto& entry : pattern->struct_entries)
		out->struct_entries.push_back({entry.name, clone_pattern(entry.value.get())});
	out->open = pattern->open;
	out->line = pattern->line;
	out->column = pattern->column;
	return out;
}

StmtPtr clone_stmt(const Stmt* stmt) {
	if (stmt == nullptr) return nullptr;
	if (const auto* value = dynamic_cast<const ExprStmt*>(stmt))
		return positioned(std::make_unique<ExprStmt>(clone_expr(value->expression.get())), stmt);
	if (const auto* value = dynamic_cast<const VarDeclStmt*>(stmt)) {
		auto out = std::make_unique<VarDeclStmt>(clone_var(*value));
		return positioned(std::move(out), stmt);
	}
	if (dynamic_cast<const BreakpointStmt*>(stmt)) return positioned(std::make_unique<BreakpointStmt>(), stmt);
	if (const auto* value = dynamic_cast<const AssignStmt*>(stmt)) {
		std::unique_ptr<AssignStmt> out = value->target
			? std::make_unique<AssignStmt>(clone_expr(value->target.get()), clone_expr(value->value.get()))
			: std::make_unique<AssignStmt>(value->name, clone_expr(value->value.get()));
		return positioned(std::move(out), stmt);
	}
	if (const auto* value = dynamic_cast<const ReturnStmt*>(stmt))
		return positioned(std::make_unique<ReturnStmt>(clone_expr(value->value.get())), stmt);
	if (const auto* value = dynamic_cast<const IfStmt*>(stmt)) {
		if (value->binding) {
			auto binding = std::make_unique<VarDeclStmt>(clone_var(*value->binding));
			return positioned(std::make_unique<IfStmt>(std::move(binding),
				clone_stmts(value->then_branch), clone_stmts(value->else_branch)), stmt);
		}
		return positioned(std::make_unique<IfStmt>(clone_expr(value->condition.get()),
			clone_stmts(value->then_branch), clone_stmts(value->else_branch)), stmt);
	}
	if (const auto* value = dynamic_cast<const WhileStmt*>(stmt))
		return positioned(std::make_unique<WhileStmt>(clone_expr(value->condition.get()),
			clone_stmts(value->body)), stmt);
	if (const auto* value = dynamic_cast<const ForStmt*>(stmt))
		return positioned(std::make_unique<ForStmt>(value->variable, clone_expr(value->iterable.get()),
			clone_stmts(value->body)), stmt);
	if (dynamic_cast<const BreakStmt*>(stmt)) return positioned(std::make_unique<BreakStmt>(), stmt);
	if (dynamic_cast<const ContinueStmt*>(stmt)) return positioned(std::make_unique<ContinueStmt>(), stmt);
	if (dynamic_cast<const PassStmt*>(stmt)) return positioned(std::make_unique<PassStmt>(), stmt);
	if (const auto* value = dynamic_cast<const MatchStmt*>(stmt)) {
		std::vector<MatchStmt::Branch> branches;
		for (const MatchStmt::Branch& branch : value->branches) {
			MatchStmt::Branch copy;
			for (const auto& pattern : branch.patterns) copy.patterns.push_back(clone_pattern(pattern.get()));
			copy.guard = clone_expr(branch.guard.get());
			copy.body = clone_stmts(branch.body);
			branches.push_back(std::move(copy));
		}
		return positioned(std::make_unique<MatchStmt>(clone_expr(value->subject.get()),
			std::move(branches), value->is_switch), stmt);
	}
	throw std::logic_error("Unhandled statement in trait AST clone");
}

Parameter clone_parameter(const Parameter& value) {
	Parameter out;
	out.name = value.name;
	out.type_hint = value.type_hint;
	out.default_value = clone_expr(value.default_value.get());
	out.line = value.line;
	out.column = value.column;
	return out;
}

FunctionDecl clone_function(const FunctionDecl& value) {
	FunctionDecl out;
	out.name = value.name;
	for (const Parameter& parameter : value.parameters) out.parameters.push_back(clone_parameter(parameter));
	out.return_type = value.return_type;
	out.body = clone_stmts(value.body);
	out.line = value.line;
	out.column = value.column;
	out.doc_comment = value.doc_comment;
	out.is_coroutine = value.is_coroutine;
	out.is_static = value.is_static;
	out.is_abstract = value.is_abstract;
	out.is_test = value.is_test;
	out.trait_origin = value.trait_origin;
	out.rpc_config = value.rpc_config;
	out.chain_link = value.chain_link;
	out.chain_name = value.chain_name;
	return out;
}

VarDeclStmt clone_var(const VarDeclStmt& value) {
	VarDeclStmt out(value.name, clone_expr(value.initializer.get()), value.is_const);
	out.type_hint = value.type_hint;
	out.is_property = value.is_property;
	out.is_static = value.is_static;
	out.is_onready = value.is_onready;
	out.trait_origin = value.trait_origin;
	out.doc_comment = value.doc_comment;
	out.chain_link = value.chain_link;
	out.export_hint = value.export_hint;
	out.export_section = value.export_section;
	if (value.setter_body) out.setter_body = std::make_unique<FunctionDecl>(clone_function(*value.setter_body));
	if (value.getter_body) out.getter_body = std::make_unique<FunctionDecl>(clone_function(*value.getter_body));
	out.setter_name = value.setter_name;
	out.getter_name = value.getter_name;
	out.line = value.line;
	out.column = value.column;
	return out;
}

StructField clone_field(const StructField& value) {
	StructField out;
	out.name = value.name;
	out.type_hint = value.type_hint;
	out.default_value = clone_expr(value.default_value.get());
	out.line = value.line;
	out.column = value.column;
	out.doc_comment = value.doc_comment;
	out.trait_origin = value.trait_origin;
	return out;
}

SignalDecl clone_signal(const SignalDecl& value) {
	SignalDecl out;
	out.name = value.name;
	for (const Parameter& parameter : value.parameters) out.parameters.push_back(clone_parameter(parameter));
	out.line = value.line;
	out.column = value.column;
	out.doc_comment = value.doc_comment;
	out.trait_origin = value.trait_origin;
	return out;
}

EnumDecl clone_enum(const EnumDecl& value) {
	EnumDecl out;
	out.name = value.name;
	out.line = value.line;
	out.column = value.column;
	for (const ExprPtr& owned : value.owned_values) out.owned_values.push_back(clone_expr(owned.get()));
	out.members = value.members;
	for (size_t i = 0; i < out.members.size(); i++) {
		if (value.members[i].value_expr == nullptr) continue;
		for (size_t j = 0; j < value.owned_values.size(); j++) {
			if (value.owned_values[j].get() == value.members[i].value_expr) {
				out.members[i].value_expr = out.owned_values[j].get();
				break;
			}
		}
	}
	return out;
}

} // namespace gdscript
