#include "traits.h"
#include "ast_clone.h"
#include "compiler_exception.h"
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace gdscript {
namespace {

[[noreturn]] void trait_error(const std::string& message, int line, int column,
	const std::string& hint = {}) {
	throw CompilerException(ErrorType::SEMANTIC_ERROR, message, line, column,
		"", "", "", hint);
}

bool same_type(const TypeExpr& left, const TypeExpr& right) {
	return left.to_string() == right.to_string();
}

std::string displaced_trait_symbol(const std::string& trait, const std::string& method) {
	return "@trait." + trait + "." + method;
}

struct Registry {
	std::unordered_map<std::string, const TraitDecl*> by_name;

	explicit Registry(const Program& program) {
		std::vector<const TraitDecl*> available;
		available.reserve(program.traits.size());
		for (const TraitDecl& trait : program.traits) available.push_back(&trait);
		add(available);
		validate(available);
	}

	explicit Registry(const std::vector<const TraitDecl*>& available) {
		add(available);
		validate(available);
	}

	void add(const std::vector<const TraitDecl*>& available) {
		for (const TraitDecl* trait : available) {
			if (trait == nullptr) continue;
			if (!by_name.emplace(trait->name, trait).second) {
				trait_error("Trait '" + trait->name + "' is declared more than once",
					trait->line, trait->column);
			}
		}
	}

	const TraitDecl& require(const std::string& name, int line, int column) const {
		auto found = by_name.find(name);
		if (found == by_name.end()) {
			trait_error("Trait '" + name + "' is not declared in this compilation",
				line, column);
		}
		return *found->second;
	}

	std::vector<const TraitDecl*> linearize(const std::vector<std::string>& names,
		int line, int column) const {
		std::vector<const TraitDecl*> out;
		std::unordered_set<const TraitDecl*> complete;
		std::vector<const TraitDecl*> stack;
		std::function<void(const TraitDecl&)> visit = [&](const TraitDecl& trait) {
			if (complete.count(&trait)) return;
			auto cycle = std::find(stack.begin(), stack.end(), &trait);
			if (cycle != stack.end()) {
				std::string path;
				for (auto at = cycle; at != stack.end(); ++at) {
					if (!path.empty()) path += " -> ";
					path += (*at)->name;
				}
				path += " -> " + trait.name;
				trait_error("Trait uses cycle: " + path, trait.line, trait.column);
			}
			stack.push_back(&trait);
			for (const std::string& dependency : trait.uses)
				visit(require(dependency, trait.line, trait.column));
			stack.pop_back();
			complete.insert(&trait);
			out.push_back(&trait);
		};
		for (const std::string& name : names) visit(require(name, line, column));
		return out;
	}

	void validate(const std::vector<const TraitDecl*>& available) const {
		for (const TraitDecl* trait : available) {
			if (trait != nullptr) linearize({trait->name}, trait->line, trait->column);
		}
	}
};

std::vector<std::string> names_of(const std::vector<const TraitDecl*>& traits) {
	std::vector<std::string> names;
	for (const TraitDecl* trait : traits) names.push_back(trait->name);
	return names;
}

void splice_script(Program& program, const Registry& registry) {
	const std::vector<const TraitDecl*> traits = registry.linearize(program.uses,
		program.class_name_line, program.class_name_column);
	program.uses = names_of(traits);
	size_t added_globals = 0;
	for (const TraitDecl* trait : traits)
		added_globals += trait->vars.size() + trait->constants.size();
	program.globals.reserve(program.globals.size() + added_globals);

	std::unordered_map<std::string, VarDeclStmt*> globals;
	for (VarDeclStmt& value : program.globals) globals[value.name] = &value;
	std::unordered_set<std::string> original_methods;
	for (const FunctionDecl& method : program.functions)
		if (method.name.empty() || method.name.front() != '@') original_methods.insert(method.name);
	std::unordered_map<std::string, const TraitDecl*> concrete_methods;
	std::unordered_map<std::string, const TraitDecl*> trait_vars;
	std::unordered_set<std::string> member_names;
	for (const auto& entry : globals) member_names.insert(entry.first);
	for (const SignalDecl& signal : program.signals) member_names.insert(signal.name);

	for (const TraitDecl* trait : traits) {
		// Constants precede variables so a trait variable initializer may name a
		// constant regardless of the separate AST storage vectors.
		for (const StructField& source : trait->constants) {
			if (member_names.count(source.name))
				trait_error("Trait '" + trait->name + "' constant '" + source.name +
					"' conflicts with a class member", source.line, source.column);
			VarDeclStmt constant(source.name, clone_expr(source.default_value.get()), true);
			constant.type_hint = source.type_hint;
			constant.line = source.line;
			constant.column = source.column;
			constant.trait_origin = trait->name;
			program.globals.push_back(std::move(constant));
			member_names.insert(source.name);
		}
		for (const VarDeclStmt& source : trait->vars) {
			auto existing = globals.find(source.name);
			if (existing != globals.end()) {
				if (existing->second->is_const || !same_type(existing->second->type_hint, source.type_hint)) {
					trait_error("Class '" + (program.class_name.empty() ? std::string("this script") : program.class_name) +
						"' and trait '" + trait->name + "' both declare '" + source.name + "'",
						source.line, source.column,
						"A class variable may replace a trait variable only with the identical type");
				}
				continue;
			}
			if (auto prior = trait_vars.find(source.name); prior != trait_vars.end()) {
				const VarDeclStmt* first = prior->second->find_var(source.name);
				if (!same_type(first->type_hint, source.type_hint)) {
					trait_error("Traits '" + prior->second->name + "' and '" + trait->name +
						"' both declare variable '" + source.name + "' with different types",
						source.line, source.column);
				}
				continue;
			}
			if (member_names.count(source.name))
				trait_error("Trait '" + trait->name + "' member '" + source.name +
					"' conflicts with a class member", source.line, source.column);
			program.globals.push_back(clone_var(source));
			globals[source.name] = &program.globals.back();
			trait_vars[source.name] = trait;
			member_names.insert(source.name);
		}

		for (const EnumDecl& source : trait->enums) {
			if (!source.name.empty() && member_names.count(source.name))
				trait_error("Trait '" + trait->name + "' enum '" + source.name +
					"' conflicts with a class member", source.line, source.column);
			program.enums.push_back(clone_enum(source));
			if (!source.name.empty()) member_names.insert(source.name);
		}
		for (const SignalDecl& source : trait->signals) {
			if (member_names.count(source.name))
				trait_error("Trait '" + trait->name + "' signal '" + source.name +
					"' conflicts with a class member", source.line, source.column);
			program.signals.push_back(clone_signal(source));
			member_names.insert(source.name);
		}

		for (const FunctionDecl& source : trait->methods) {
			if (source.is_abstract) continue;
			FunctionDecl copy = clone_function(source);
			copy.trait_origin = trait->name;
			if (original_methods.count(source.name)) {
				copy.name = displaced_trait_symbol(trait->name, source.name);
				copy.chain_name = source.name;
				program.functions.push_back(std::move(copy));
				concrete_methods[source.name] = trait;
				continue;
			}
			if (auto prior = concrete_methods.find(source.name); prior != concrete_methods.end()) {
				trait_error("Traits '" + prior->second->name + "' and '" + trait->name +
					"' both declare '" + source.name + "'; override it in '" +
					(program.class_name.empty() ? std::string("this script") : program.class_name) + "'",
					source.line, source.column);
			}
			program.functions.push_back(std::move(copy));
			concrete_methods[source.name] = trait;
		}
	}
}

void splice_nested(StructDecl& user, const Registry& registry) {
	const std::vector<const TraitDecl*> traits = registry.linearize(user.uses,
		user.line, user.column);
	user.uses = names_of(traits);
	std::unordered_set<std::string> original_methods;
	for (const FunctionDecl& method : user.methods) original_methods.insert(method.name);
	std::unordered_map<std::string, const TraitDecl*> concrete_methods;
	std::unordered_map<std::string, const TraitDecl*> trait_fields;

	for (const TraitDecl* trait : traits) {
		if (!trait->base_name.empty() && !user.is_class) {
			trait_error("Struct '" + user.name + "' cannot use '" + trait->name +
				"': the trait requires base '" + trait->base_name + "'", user.line, user.column);
		}
		if (!trait->signals.empty()) {
			trait_error("Class '" + user.name + "' cannot use '" + trait->name +
				"': nested classes do not support signals", user.line, user.column);
		}
		for (const VarDeclStmt& source : trait->vars) {
			if (source.is_property || source.is_onready)
				trait_error("Trait '" + trait->name + "' variable '" + source.name +
					"' uses a script-class-only annotation", source.line, source.column);
			if (const StructField* own = user.find_field(source.name)) {
				if (!same_type(own->type_hint, source.type_hint))
					trait_error("Class '" + user.name + "' and trait '" + trait->name +
						"' declare '" + source.name + "' with different types", source.line, source.column);
				continue;
			}
			if (auto prior = trait_fields.find(source.name); prior != trait_fields.end()) {
				if (!same_type(prior->second->find_var(source.name)->type_hint, source.type_hint))
					trait_error("Traits '" + prior->second->name + "' and '" + trait->name +
						"' declare '" + source.name + "' with different types", source.line, source.column);
				continue;
			}
			StructField field;
			field.name = source.name;
			field.type_hint = source.type_hint;
			field.default_value = clone_expr(source.initializer.get());
			field.line = source.line;
			field.column = source.column;
			field.trait_origin = trait->name;
			user.fields.push_back(std::move(field));
			trait_fields[source.name] = trait;
		}
		for (const StructField& source : trait->constants) {
			if (user.find_constant(source.name) || user.find_field(source.name))
				trait_error("Trait '" + trait->name + "' constant '" + source.name +
					"' conflicts in '" + user.name + "'", source.line, source.column);
			user.constants.push_back(clone_field(source));
		}
		for (const FunctionDecl& source : trait->methods) {
			if (source.is_abstract) continue;
			FunctionDecl copy = clone_function(source);
			copy.trait_origin = trait->name;
			if (original_methods.count(source.name)) {
				copy.name = displaced_trait_symbol(trait->name, source.name);
				copy.chain_name = source.name;
				user.methods.push_back(std::move(copy));
				concrete_methods[source.name] = trait;
				continue;
			}
			if (auto prior = concrete_methods.find(source.name); prior != concrete_methods.end())
				trait_error("Traits '" + prior->second->name + "' and '" + trait->name +
					"' both declare '" + source.name + "'; override it in '" + user.name + "'",
					source.line, source.column);
			user.methods.push_back(std::move(copy));
			concrete_methods[source.name] = trait;
		}
	}
}

void hoist_trait_onready(Program& program) {
	std::vector<StmtPtr> prologue;
	for (VarDeclStmt& variable : program.globals) {
		if (variable.trait_origin.empty() || !variable.is_onready || !variable.initializer) continue;
		auto assign = std::make_unique<AssignStmt>(variable.name, std::move(variable.initializer));
		assign->line = variable.line;
		assign->column = variable.column;
		prologue.push_back(std::move(assign));
	}
	if (prologue.empty()) return;
	FunctionDecl* ready = nullptr;
	for (FunctionDecl& method : program.functions) {
		if (method.name == "_ready") {
			ready = &method;
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
	for (StmtPtr& statement : ready->body) prologue.push_back(std::move(statement));
	ready->body = std::move(prologue);
}

} // namespace

void apply_traits(Program& program) {
	Registry registry(program);
	splice_script(program, registry);
	for (StructDecl& user : program.structs) splice_nested(user, registry);
	hoist_trait_onready(program);
}

void apply_traits(Program& program, const std::vector<const TraitDecl*>& available) {
	Registry registry(available);
	splice_script(program, registry);
	for (StructDecl& user : program.structs) splice_nested(user, registry);
	hoist_trait_onready(program);
}

} // namespace gdscript
