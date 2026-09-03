#include "source_model.h"

#include "ast.h"
#include "globals.h"
#include "lexer.h"
#include "parser.h"
#include "variant_types.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace gdscript {
namespace {

constexpr uint32_t MAGIC = 0x4d445347u; // "GSDM"
constexpr uint16_t MAJOR = 1;
constexpr uint16_t MINOR = 0;
constexpr size_t MAX_BLOB = 32u * 1024u * 1024u;
constexpr uint32_t MAX_RECORDS = 100000;
constexpr uint32_t MAX_STRING = 1024u * 1024u;
constexpr uint32_t MAX_DIAGNOSTICS = 100;

// Append only; older decoders skip unknown sections.
enum Section : uint32_t { META = 1, DIAGNOSTICS = 2, DECLARATIONS = 3,
	PROPERTIES = 4, CARET = 5, SAFE_LINES = 6, DECLARATION_TYPES = 7,
	DECLARATION_VALUES = 8 };

template <typename T> void scalar(std::vector<uint8_t> &out, T value) {
	static_assert(std::is_trivially_copyable<T>::value, "raw bytes only");
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
	out.insert(out.end(), bytes, bytes + sizeof(T));
}
void string(std::vector<uint8_t> &out, const std::string &value) {
	scalar<uint32_t>(out, uint32_t(value.size()));
	out.insert(out.end(), value.begin(), value.end());
}
void range(std::vector<uint8_t> &out, const SourceRange &value) {
	scalar<uint32_t>(out, value.start_line); scalar<uint32_t>(out, value.start_column);
	scalar<uint32_t>(out, value.end_line); scalar<uint32_t>(out, value.end_column);
}
void section(std::vector<uint8_t> &out, uint32_t id, const std::vector<uint8_t> &body) {
	scalar<uint32_t>(out, id); scalar<uint32_t>(out, uint32_t(body.size()));
	out.insert(out.end(), body.begin(), body.end());
}

struct Reader {
	const uint8_t *data = nullptr; size_t size = 0; size_t at = 0; bool ok = true;
	template <typename T> T scalar() {
		T value{};
		if (!ok || at > size || sizeof(T) > size - at) { ok = false; return value; }
		std::memcpy(&value, data + at, sizeof(T)); at += sizeof(T); return value;
	}
	std::string string() {
		const uint32_t n = scalar<uint32_t>();
		if (!ok || n > MAX_STRING || at > size || n > size - at) { ok = false; return {}; }
		std::string value(reinterpret_cast<const char *>(data + at), n); at += n; return value;
	}
	SourceRange range() {
		return {scalar<uint32_t>(), scalar<uint32_t>(), scalar<uint32_t>(), scalar<uint32_t>()};
	}
};

bool valid_range(const SourceRange &r) {
	return r.start_line <= r.end_line &&
		(r.start_line != r.end_line || r.start_column <= r.end_column);
}

std::string trim(const std::string &s) {
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
	return s.substr(a, b - a);
}
uint32_t indentation(const std::string &s) {
	uint32_t n = 0;
	for (char c : s) { if (c == ' ') n++; else if (c == '\t') n += 4; else break; }
	return n;
}
std::vector<std::string> split_lines(const std::string &source) {
	std::vector<std::string> lines;
	std::stringstream stream(source); std::string line;
	while (std::getline(stream, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); lines.push_back(line); }
	if (source.empty() || (!source.empty() && source.back() == '\n')) lines.push_back({});
	return lines;
}
size_t expression_start(const std::string &s, size_t p_end) {
	size_t at = p_end;
	bool consumed = false;
	while (at > 0) {
		const char c = s[at - 1];
		bool group = false;
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
			while (at > 0 && (std::isalnum(static_cast<unsigned char>(s[at - 1])) || s[at - 1] == '_')) at--;
		} else if (c == ')' || c == ']') {
			int depth = 0;
			size_t scan = at;
			bool matched = false;
			while (scan > 0) {
				const char d = s[scan - 1];
				if (d == '"' || d == '\'') {
					size_t quote = scan - 1;
					while (quote > 0 && s[quote - 1] != d) quote--;
					scan = quote > 0 ? quote - 1 : 0;
					continue;
				}
				scan--;
				if (d == ')' || d == ']') depth++;
				else if (d == '(' || d == '[') { depth--; if (depth == 0) { matched = true; break; } }
			}
			if (!matched) return p_end;
			at = scan;
			group = true;
		} else if (c == '"' || c == '\'') {
			size_t quote = at - 1;
			while (quote > 0 && s[quote - 1] != c) quote--;
			if (quote == 0) return p_end;
			at = quote - 1;
			group = true;
		} else {
			break;
		}
		consumed = true;
		if (at > 0 && (s[at - 1] == '.' || s[at - 1] == '/')) { at--; continue; }
		// The group follows the name it calls or indexes.
		if (group && at > 0 && (std::isalnum(static_cast<unsigned char>(s[at - 1])) || s[at - 1] == '_')) continue;
		break;
	}
	if (!consumed) return p_end;
	if (at > 0 && (s[at - 1] == '$' || s[at - 1] == '%')) at--;
	return at;
}

bool tail_is(const std::string &s, const char *suffix) {
	const size_t n = std::strlen(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void analyze_caret(const std::string &before, CaretContext &caret) {
	struct Open { size_t at; int argument; bool call; };
	std::vector<Open> open;
	bool in_string = false;
	char quote = 0;
	for (size_t i = 0; i < before.size(); i++) {
		const char c = before[i];
		if (in_string) {
			if (c == '\\') i++;
			else if (c == quote) in_string = false;
			continue;
		}
		if (c == '"' || c == '\'') { in_string = true; quote = c; }
		else if (c == '(') {
			const char previous = i > 0 ? before[i - 1] : ' ';
			open.push_back({i, 0, std::isalnum(static_cast<unsigned char>(previous)) != 0 ||
					previous == '_' || previous == ')' || previous == ']'});
		} else if (c == '[' || c == '{') open.push_back({i, 0, false});
		else if (c == ')' || c == ']' || c == '}') { if (!open.empty()) open.pop_back(); }
		else if (c == ',' && !open.empty()) open.back().argument++;
	}
	// Find the innermost open call for signature display.
	for (size_t i = open.size(); i-- > 0;) {
		if (!open[i].call) continue;
		const size_t start = expression_start(before, open[i].at);
		if (start < open[i].at) {
			caret.callee = before.substr(start, open[i].at - start);
			caret.argument_index = open[i].argument;
		}
		break;
	}
	if (in_string) return;

	size_t prefix = before.size();
	while (prefix > 0 && (std::isalnum(static_cast<unsigned char>(before[prefix - 1])) || before[prefix - 1] == '_')) prefix--;
	if (prefix > 0 && before[prefix - 1] == '@') { caret.kind = CaretKind::ANNOTATION; return; }
	if (prefix > 0 && before[prefix - 1] == '.') {
		const size_t start = expression_start(before, prefix - 1);
		if (start < prefix - 1) {
			caret.kind = CaretKind::MEMBER;
			caret.receiver_text = before.substr(start, prefix - 1 - start);
			return;
		}
	}
	const std::string head = trim(before.substr(0, prefix));
	if (tail_is(head, ":") || tail_is(head, "->") || tail_is(head, " is") || tail_is(head, " as") ||
			head == "extends") {
		caret.kind = CaretKind::TYPE;
		return;
	}
	caret.kind = caret.argument_index >= 0 ? CaretKind::CALL_ARGUMENT : CaretKind::IDENTIFIER;
}

std::string type_name_of(const TypeExpr &type) {
	const std::string &single = type.single_name();
	if (!single.empty()) return single;
	return {};
}

bool is_builtin_type_name(const std::string &name) {
	return !name.empty() && Variant::type_from_name(name) != Variant::VARIANT_MAX;
}

std::string global_result_type(const GlobalFunction &info) {
	switch (info.result) {
		case GlobalResult::BOOL: return "bool";
		case GlobalResult::INT: return "int";
		case GlobalResult::FLOAT: return "float";
		case GlobalResult::STRING: return "String";
		case GlobalResult::NIL:
		case GlobalResult::NUMERIC:
		case GlobalResult::VARIANT: break;
	}
	return {};
}

bool is_numeric_type(const std::string &type) { return type == "int" || type == "float"; }

// Render a default value for signatures; ambiguous forms return empty.
std::string default_value_text(const Expr *expr) {
	if (expr == nullptr) return {};
	if (const auto *literal = dynamic_cast<const LiteralExpr *>(expr)) {
		switch (literal->lit_type) {
			case LiteralExpr::Type::INTEGER: return std::to_string(std::get<int64_t>(literal->value));
			case LiteralExpr::Type::BOOL: return std::get<bool>(literal->value) ? "true" : "false";
			case LiteralExpr::Type::NULL_VAL: return "null";
			case LiteralExpr::Type::STRING: return "\"" + std::get<std::string>(literal->value) + "\"";
			case LiteralExpr::Type::FLOAT: {
				char text[32];
				std::snprintf(text, sizeof(text), "%g", std::get<double>(literal->value));
				return text;
			}
		}
		return {};
	}
	if (const auto *variable = dynamic_cast<const VariableExpr *>(expr)) return variable->name;
	if (const auto *unary = dynamic_cast<const UnaryExpr *>(expr)) {
		const std::string inner = default_value_text(unary->operand.get());
		if (inner.empty()) return {};
		switch (unary->op) {
			case UnaryExpr::Op::NEG: return "-" + inner;
			case UnaryExpr::Op::NOT: return "not " + inner;
			case UnaryExpr::Op::BIT_NOT: return "~" + inner;
		}
		return {};
	}
	if (const auto *member = dynamic_cast<const MemberCallExpr *>(expr)) {
		if (member->is_method_call || !member->arguments.empty()) return {};
		const std::string object = default_value_text(member->object.get());
		return object.empty() ? std::string() : object + "." + member->member_name;
	}
	if (const auto *call = dynamic_cast<const CallExpr *>(expr)) {
		return call->arguments.empty() && !call->is_node_path_sugar ?
				call->function_name + "()" : std::string();
	}
	if (const auto *array = dynamic_cast<const ArrayLiteralExpr *>(expr)) {
		return array->elements.empty() ? "[]" : std::string();
	}
	if (const auto *dictionary = dynamic_cast<const DictionaryLiteralExpr *>(expr)) {
		return dictionary->elements.empty() ? "{}" : std::string();
	}
	return {};
}

struct ModelBuilder {
	SourceModel &model;
	const Program &program;
	const std::vector<std::string> &lines;
	bool warnings_wanted = true;

	std::vector<std::vector<int32_t>> scopes;
	std::vector<uint8_t> used;
	std::unordered_set<std::string> unresolved;
	std::unordered_set<std::string> literal_strings;
	std::vector<std::pair<int32_t, const FunctionDecl *>> pending_bodies;
	int32_t current_function = -1;

	ModelBuilder(SourceModel &p_model, const Program &p_program,
			const std::vector<std::string> &p_lines) :
			model(p_model), program(p_program), lines(p_lines) {}

	// Whitespace-only lines are not blank; their indent matters.
	bool blank_or_comment(size_t index) const {
		const std::string &raw = lines[index];
		if (raw.empty()) return true;
		const std::string text = trim(raw);
		return text.empty() ? false : text[0] == '#';
	}

	uint32_t block_end_line(uint32_t header_line) const {
		if (lines.empty()) return 1;
		if (header_line == 0 || header_line > lines.size()) return uint32_t(lines.size());
		const uint32_t indent = indentation(lines[header_line - 1]);
		for (size_t i = header_line; i < lines.size(); i++) {
			if (blank_or_comment(i)) continue;
			if (indentation(lines[i]) <= indent) return uint32_t(i);
		}
		return uint32_t(lines.size());
	}

	std::string doc_above(uint32_t line) const {
		std::vector<std::string> collected;
		for (uint32_t at = line; at > 1; at--) {
			const std::string text = trim(lines[at - 2]);
			if (text.rfind("##", 0) != 0) break;
			collected.push_back(trim(text.substr(2)));
		}
		std::string doc;
		for (size_t i = collected.size(); i-- > 0;) {
			if (!doc.empty()) doc += '\n';
			doc += collected[i];
		}
		return doc;
	}

	uint32_t column_of(uint32_t line, const std::string &name) const {
		if (line == 0 || line > lines.size() || name.empty()) return 1;
		const std::string &text = lines[line - 1];
		for (size_t at = 0; (at = text.find(name, at)) != std::string::npos; at += name.size()) {
			const bool left = at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) ||
					text[at - 1] == '_');
			const size_t end = at + name.size();
			const bool right = end == text.size() ||
					!(std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_');
			if (left && right) return uint32_t(at + 1);
		}
		return 1;
	}

	void warn(const char *code, const std::string &message, int line, int column, size_t width = 1) {
		if (!warnings_wanted) return;
		if (model.diagnostics.size() >= MAX_DIAGNOSTICS) return;
		const uint32_t start = uint32_t(std::max(column, 1));
		model.diagnostics.push_back({DiagnosticSeverity::WARNING, code, message, model.path,
			{uint32_t(std::max(line, 1)), start, uint32_t(std::max(line, 1)),
			 start + uint32_t(width < 1 ? 1 : width)}});
	}

	void fail(const std::string &code, const std::string &message, int line, int column,
			size_t width = 1) {
		if (model.diagnostics.size() >= MAX_DIAGNOSTICS) return;
		const uint32_t start = uint32_t(std::max(column, 1));
		model.diagnostics.push_back({DiagnosticSeverity::ERROR, code, message, model.path,
			{uint32_t(std::max(line, 1)), start, uint32_t(std::max(line, 1)),
			 start + uint32_t(width < 1 ? 1 : width)}});
	}

	int32_t add(DeclarationKind kind, const std::string &name, uint32_t line, int32_t parent,
			uint32_t scope_start, uint32_t scope_end) {
		SourceDeclaration declaration;
		declaration.kind = kind;
		declaration.name = name;
		const uint32_t column = column_of(line, name);
		declaration.declaration = {line, column, line, column + uint32_t(name.size())};
		declaration.lexical_scope = {scope_start, 1, scope_end < scope_start ? scope_start : scope_end,
			uint32_t(lines.empty() ? 1 : lines.back().size() + 1)};
		declaration.parent = parent;
		const int32_t index = int32_t(model.declarations.size());
		model.declarations.push_back(std::move(declaration));
		used.push_back(0);
		if (parent >= 0) model.declarations[size_t(parent)].children.push_back(index);
		else declare_in_scope(index);
		return index;
	}

	void declare_in_scope(int32_t index) {
		if (!scopes.empty()) scopes.back().push_back(index);
	}

	int32_t lookup(const std::string &name) const {
		for (size_t depth = scopes.size(); depth-- > 0;) {
			const std::vector<int32_t> &scope = scopes[depth];
			for (size_t i = scope.size(); i-- > 0;) {
				if (model.declarations[size_t(scope[i])].name == name) return scope[i];
			}
		}
		return -1;
	}

	void use(const std::string &name) {
		const int32_t index = lookup(name);
		if (index >= 0) {
			used[size_t(index)] = 1;
			return;
		}
		unresolved.insert(name);
	}

	const StructDecl *find_struct(const std::string &name) const {
		for (const StructDecl &declaration : program.structs) {
			if (declaration.name == name) return &declaration;
		}
		return nullptr;
	}
	const EnumDecl *find_enum(const std::string &name) const {
		for (const EnumDecl &declaration : program.enums) {
			if (!declaration.name.empty() && declaration.name == name) return &declaration;
		}
		return nullptr;
	}
	const TraitDecl *find_trait(const std::string &name) const {
		for (const TraitDecl &declaration : program.traits) {
			if (declaration.name == name) return &declaration;
		}
		return nullptr;
	}
	const FunctionDecl *find_function(const std::string &name) const {
		for (const FunctionDecl &declaration : program.functions) {
			if (declaration.name == name) return &declaration;
		}
		return nullptr;
	}
	bool names_a_type(const std::string &name) const {
		return is_builtin_type_name(name) || find_struct(name) != nullptr ||
				find_enum(name) != nullptr || find_trait(name) != nullptr;
	}

	std::string declared_type_of_name(const std::string &name) const {
		const int32_t index = lookup(name);
		if (index < 0) return {};
		const SourceDeclaration &declaration = model.declarations[size_t(index)];
		return declaration.resolved_type.empty() ? declaration.declared_type :
				declaration.resolved_type;
	}

	std::string type_of(const Expr *expr) const {
		if (expr == nullptr) return {};
		if (const auto *literal = dynamic_cast<const LiteralExpr *>(expr)) {
			switch (literal->lit_type) {
				case LiteralExpr::Type::INTEGER: return "int";
				case LiteralExpr::Type::FLOAT: return "float";
				case LiteralExpr::Type::BOOL: return "bool";
				case LiteralExpr::Type::STRING:
					if (literal->string_type == LiteralExpr::StringType::STRING_NAME) return "StringName";
					if (literal->string_type == LiteralExpr::StringType::NODE_PATH) return "NodePath";
					return "String";
				case LiteralExpr::Type::NULL_VAL: return {};
			}
			return {};
		}
		if (const auto *variable = dynamic_cast<const VariableExpr *>(expr)) {
			const std::string declared = declared_type_of_name(variable->name);
			if (!declared.empty()) return declared;
			if (names_a_type(variable->name)) return variable->name;
			return {};
		}
		if (const auto *cast = dynamic_cast<const CastExpr *>(expr)) {
			return cast->type_name;
		}
		if (dynamic_cast<const TypeTestExpr *>(expr) != nullptr) return "bool";
		if (dynamic_cast<const LambdaExpr *>(expr) != nullptr) return "Callable";
		if (dynamic_cast<const ArrayLiteralExpr *>(expr) != nullptr) return "Array";
		if (dynamic_cast<const DictionaryLiteralExpr *>(expr) != nullptr) return "Dictionary";
		if (const auto *unary = dynamic_cast<const UnaryExpr *>(expr)) {
			if (unary->op == UnaryExpr::Op::NOT) return "bool";
			return type_of(unary->operand.get());
		}
		if (const auto *ternary = dynamic_cast<const TernaryExpr *>(expr)) {
			const std::string left = type_of(ternary->true_value.get());
			return left == type_of(ternary->false_value.get()) ? left : std::string();
		}
		if (const auto *binary = dynamic_cast<const BinaryExpr *>(expr)) {
			switch (binary->op) {
				case BinaryExpr::Op::EQ: case BinaryExpr::Op::NEQ: case BinaryExpr::Op::LT:
				case BinaryExpr::Op::LTE: case BinaryExpr::Op::GT: case BinaryExpr::Op::GTE:
				case BinaryExpr::Op::AND: case BinaryExpr::Op::OR: case BinaryExpr::Op::IN:
					return "bool";
				default: break;
			}
			const std::string left = type_of(binary->left.get());
			const std::string right = type_of(binary->right.get());
			if (!is_numeric_type(left) || !is_numeric_type(right)) return {};
			if (binary->op == BinaryExpr::Op::POW) return "float";
			return left == "float" || right == "float" ? "float" : "int";
		}
		if (const auto *call = dynamic_cast<const CallExpr *>(expr)) {
			if (call->is_node_path_sugar) return {};
			const std::string &name = call->function_name;
			if (names_a_type(name)) return name;
			if (const FunctionDecl *function = find_function(name)) {
				return type_name_of(function->return_type);
			}
			if (const GlobalFunction *global = find_global_function(name)) {
				return global_result_type(*global);
			}
			return {};
		}
		if (const auto *member = dynamic_cast<const MemberCallExpr *>(expr)) {
			const std::string receiver = type_of(member->object.get());
			if (receiver.empty()) return {};
			if (member->member_name == "new") return receiver;
			if (find_enum(receiver) != nullptr) return "int";
			if (const StructDecl *declaration = find_struct(receiver)) {
				if (const StructField *field = declaration->find_field(member->member_name)) {
					return type_name_of(field->type_hint);
				}
				if (const FunctionDecl *method = declaration->find_method(member->member_name)) {
					return type_name_of(method->return_type);
				}
			}
			return {};
		}
		return {};
	}

	void build() {
		used.clear();
		scopes.emplace_back();
		if (!program.class_name.empty() || !program.base_class.empty()) {
			const uint32_t line = uint32_t(std::max(program.class_name.empty() ?
					program.base_class_line : program.class_name_line, 1));
			const int32_t index = add(DeclarationKind::CLASS, program.class_name, line, -1, 1,
					uint32_t(lines.size()));
			model.declarations[size_t(index)].base_type = program.base_class;
			model.declarations[size_t(index)].documentation = doc_above(line);
			used[size_t(index)] = 1;
		}
		emit_file_members();
		for (const auto &entry : pending_bodies) {
			walk_function_body(entry.first, *entry.second);
		}
		report_unused();
	}

	// Source order, not per-kind order.
	void emit_file_members() {
		struct Item { int line; int order; int kind; const void *node; };
		std::vector<Item> items;
		int order = 0;
		for (const VarDeclStmt &node : program.globals) items.push_back({node.line, order++, 0, &node});
		for (const StructDecl &node : program.structs) items.push_back({node.line, order++, 1, &node});
		for (const TraitDecl &node : program.traits) items.push_back({node.line, order++, 2, &node});
		for (const EnumDecl &node : program.enums) items.push_back({node.line, order++, 3, &node});
		for (const SignalDecl &node : program.signals) items.push_back({node.line, order++, 4, &node});
		for (const FunctionDecl &node : program.functions) items.push_back({node.line, order++, 5, &node});
		std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
			return a.line != b.line ? a.line < b.line : a.order < b.order;
		});
		for (const Item &item : items) {
			switch (item.kind) {
				case 0: emit_var(*static_cast<const VarDeclStmt *>(item.node), -1, 1,
						uint32_t(lines.size()), true); break;
				case 1: emit_struct(*static_cast<const StructDecl *>(item.node)); break;
				case 2: emit_trait(*static_cast<const TraitDecl *>(item.node)); break;
				case 3: emit_enum(*static_cast<const EnumDecl *>(item.node), -1); break;
				case 4: emit_signal(*static_cast<const SignalDecl *>(item.node), -1); break;
				case 5: emit_function(*static_cast<const FunctionDecl *>(item.node), -1); break;
				default: break;
			}
		}
	}

	int32_t emit_var(const VarDeclStmt &node, int32_t parent, uint32_t scope_start,
			uint32_t scope_end, bool file_level) {
		const uint32_t line = uint32_t(std::max(node.line, 1));
		const int32_t index = add(node.is_const ? DeclarationKind::CONSTANT : DeclarationKind::VARIABLE,
				node.name, line, parent, scope_start, scope_end);
		SourceDeclaration &declaration = model.declarations[size_t(index)];
		declaration.declared_type = node.type_hint.empty() ? std::string() : node.type_hint.to_string();
		declaration.resolved_type = type_name_of(node.type_hint);
		if (declaration.resolved_type.empty()) {
			declaration.resolved_type = type_of(node.initializer.get());
		}
		declaration.documentation = node.doc_comment;
		declaration.initializer_text = default_value_text(node.initializer.get());
		declaration.setter = node.setter_name.empty() && node.setter_body ?
				node.setter_body->name : node.setter_name;
		declaration.getter = node.getter_name.empty() && node.getter_body ?
				node.getter_body->name : node.getter_name;
		if (node.is_static) declaration.flags |= DECLARATION_STATIC;
		if (node.is_property) declaration.flags |= DECLARATION_EXPORT;
		if (node.is_onready) declaration.flags |= DECLARATION_ONREADY;
		if (node.is_property) declaration.annotation_arguments.push_back("@export");
		if (node.is_onready) declaration.annotation_arguments.push_back("@onready");
		if (file_level) used[size_t(index)] = 1;
		return index;
	}

	void emit_struct(const StructDecl &node) {
		const uint32_t line = uint32_t(std::max(node.line, 1));
		const uint32_t end = block_end_line(line);
		const int32_t index = add(node.is_class ? DeclarationKind::NESTED_CLASS :
				DeclarationKind::STRUCT, node.name, line, -1, line, end);
		model.declarations[size_t(index)].base_type = node.base_name;
		model.declarations[size_t(index)].documentation = node.doc_comment;
		used[size_t(index)] = 1;
		for (const StructField &field : node.fields) {
			const uint32_t field_line = uint32_t(std::max(field.line, int(line)));
			const int32_t child = add(DeclarationKind::VARIABLE, field.name, field_line, index,
					field_line, end);
			model.declarations[size_t(child)].declared_type = field.type_hint.empty() ?
					std::string() : field.type_hint.to_string();
			model.declarations[size_t(child)].resolved_type = type_name_of(field.type_hint);
			model.declarations[size_t(child)].documentation = field.doc_comment;
			used[size_t(child)] = 1;
		}
		for (const StructField &constant : node.constants) {
			const uint32_t constant_line = uint32_t(std::max(constant.line, int(line)));
			const int32_t child = add(DeclarationKind::CONSTANT, constant.name, constant_line, index,
					constant_line, end);
			used[size_t(child)] = 1;
		}
		for (const FunctionDecl &method : node.methods) {
			emit_function(method, index);
		}
	}

	void emit_trait(const TraitDecl &node) {
		const uint32_t line = uint32_t(std::max(node.line, 1));
		const uint32_t end = node.is_file_level ? uint32_t(lines.size()) : block_end_line(line);
		const int32_t index = add(DeclarationKind::TRAIT, node.name, line, -1, line, end);
		model.declarations[size_t(index)].base_type = node.base_name;
		model.declarations[size_t(index)].documentation = node.doc_comment;
		used[size_t(index)] = 1;
		for (const VarDeclStmt &var : node.vars) {
			used[size_t(emit_var(var, index, uint32_t(std::max(var.line, int(line))), end, true))] = 1;
		}
		for (const StructField &constant : node.constants) {
			const uint32_t constant_line = uint32_t(std::max(constant.line, int(line)));
			used[size_t(add(DeclarationKind::CONSTANT, constant.name, constant_line, index,
					constant_line, end))] = 1;
		}
		for (const EnumDecl &declaration : node.enums) emit_enum(declaration, index);
		for (const SignalDecl &signal : node.signals) emit_signal(signal, index);
		for (const FunctionDecl &method : node.methods) emit_function(method, index);
	}

	void emit_enum(const EnumDecl &node, int32_t parent) {
		const uint32_t line = uint32_t(std::max(node.line, 1));
		const int32_t index = add(DeclarationKind::ENUM, node.name, line, parent, line,
				block_end_line(line));
		model.declarations[size_t(index)].documentation = doc_above(line);
		used[size_t(index)] = 1;
		for (const EnumDecl::Member &member : node.members) {
			const uint32_t member_line = uint32_t(std::max(member.line, int(line)));
			const uint32_t column = column_of(member_line, member.name);
			model.declarations[size_t(index)].enum_members.push_back({member.name, member.value,
				{member_line, column, member_line, column + uint32_t(member.name.size())}});
		}
	}

	void emit_signal(const SignalDecl &node, int32_t parent) {
		const uint32_t line = uint32_t(std::max(node.line, 1));
		const int32_t index = add(DeclarationKind::SIGNAL, node.name, line, parent, line, line);
		model.declarations[size_t(index)].documentation = node.doc_comment;
		for (const Parameter &parameter : node.parameters) {
			SourceParameter published;
			published.name = parameter.name;
			published.declared_type = parameter.type_hint.empty() ? std::string() :
					parameter.type_hint.to_string();
			published.declaration = model.declarations[size_t(index)].declaration;
			model.declarations[size_t(index)].parameters.push_back(std::move(published));
		}
	}

	void emit_function(const FunctionDecl &node, int32_t parent) {
		const uint32_t line = uint32_t(std::max(node.line, 1));
		const uint32_t end = block_end_line(line);
		const int32_t index = add(DeclarationKind::FUNCTION, node.name, line, parent, line, end);
		SourceDeclaration &declaration = model.declarations[size_t(index)];
		declaration.return_type = type_name_of(node.return_type);
		declaration.documentation = node.doc_comment;
		if (node.is_static) declaration.flags |= DECLARATION_STATIC;
		if (node.is_abstract) declaration.flags |= DECLARATION_ABSTRACT;
		if (node.is_test) {
			declaration.flags |= DECLARATION_TEST;
			declaration.annotation_arguments.push_back("@test");
		}
		used[size_t(index)] = 1;
		for (const Parameter &parameter : node.parameters) {
			SourceParameter published;
			published.name = parameter.name;
			published.declared_type = parameter.type_hint.empty() ? std::string() :
					parameter.type_hint.to_string();
			published.default_text = default_value_text(parameter.default_value.get());
			const uint32_t parameter_line = uint32_t(std::max(parameter.line, int(line)));
			const uint32_t column = column_of(parameter_line, parameter.name);
			published.declaration = {parameter_line, column, parameter_line,
				column + uint32_t(parameter.name.size())};
			model.declarations[size_t(index)].parameters.push_back(std::move(published));
		}
		for (const Parameter &parameter : node.parameters) {
			const uint32_t parameter_line = uint32_t(std::max(parameter.line, int(line)));
			const int32_t child = add(DeclarationKind::PARAMETER, parameter.name, parameter_line,
					index, line, end);
			model.declarations[size_t(child)].declared_type = parameter.type_hint.empty() ?
					std::string() : parameter.type_hint.to_string();
			model.declarations[size_t(child)].resolved_type = type_name_of(parameter.type_hint);
		}
		pending_bodies.push_back({index, &node});
	}

	void walk_function_body(int32_t index, const FunctionDecl &node) {
		const int32_t previous_function = current_function;
		current_function = index;
		scopes.emplace_back();
		for (int32_t child : model.declarations[size_t(index)].children) {
			if (model.declarations[size_t(child)].kind == DeclarationKind::PARAMETER) {
				declare_in_scope(child);
			}
		}
		walk_block(node.body, model.declarations[size_t(index)].lexical_scope.end_line);
		scopes.pop_back();
		current_function = previous_function;
	}

	static bool terminates(const Stmt *stmt) {
		return dynamic_cast<const ReturnStmt *>(stmt) != nullptr ||
				dynamic_cast<const BreakStmt *>(stmt) != nullptr ||
				dynamic_cast<const ContinueStmt *>(stmt) != nullptr;
	}

	void walk_block(const std::vector<StmtPtr> &body, uint32_t scope_end) {
		scopes.emplace_back();
		bool terminated = false;
		bool reported = false;
		for (const StmtPtr &stmt : body) {
			if (!stmt) continue;
			if (terminated && !reported) {
				warn("UNREACHABLE_CODE", "Statement is unreachable", stmt->line, 1);
				reported = true;
			}
			walk_stmt(stmt.get(), scope_end);
			terminated = terminated || terminates(stmt.get());
		}
		scopes.pop_back();
	}

	void check_shadowing(const std::string &name, int line) {
		if (name.empty() || name == "_") return;
		if (lookup(name) >= 0) {
			warn("SHADOWED_VARIABLE", "'" + name + "' shadows an earlier declaration", line,
					int(column_of(uint32_t(std::max(line, 1)), name)), name.size());
		}
	}

	void check_narrowing(const std::string &target_type, const Expr *value, int line, int column) {
		if (target_type != "int" || value == nullptr) return;
		if (type_of(value) == "float") {
			warn("NARROWING_CONVERSION", "Assignment may narrow a fractional value to int",
					line, column);
		}
	}

	void walk_stmt(const Stmt *stmt, uint32_t scope_end) {
		if (const auto *node = dynamic_cast<const VarDeclStmt *>(stmt)) {
			walk_expr(node->initializer.get());
			check_shadowing(node->name, node->line);
			const int32_t index = emit_var(*node, current_function, uint32_t(std::max(node->line, 1)),
					scope_end, false);
			declare_in_scope(index);
			if (!node->initializer && node->type_hint.empty() && !node->has_accessors() &&
					!node->is_const) {
				warn("UNASSIGNED_VARIABLE", "Variable is declared without an initial value",
						node->line, int(column_of(uint32_t(std::max(node->line, 1)), node->name)),
						node->name.size());
			}
			check_narrowing(type_name_of(node->type_hint), node->initializer.get(), node->line,
					node->column);
			if (node->setter_body) walk_accessor(*node->setter_body, scope_end);
			if (node->getter_body) walk_accessor(*node->getter_body, scope_end);
			return;
		}
		if (const auto *node = dynamic_cast<const AssignStmt *>(stmt)) {
			walk_expr(node->value.get());
			if (node->target) {
				walk_expr(node->target.get());
				check_narrowing(type_of(node->target.get()), node->value.get(), node->line,
						node->column);
			} else {
				use(node->name);
				check_narrowing(declared_type_of_name(node->name), node->value.get(), node->line,
						node->column);
			}
			return;
		}
		if (const auto *node = dynamic_cast<const ReturnStmt *>(stmt)) {
			walk_expr(node->value.get());
			return;
		}
		if (const auto *node = dynamic_cast<const IfStmt *>(stmt)) {
			const uint32_t end = block_end_line(uint32_t(std::max(node->line, 1)));
			walk_expr(node->condition.get());
			scopes.emplace_back();
			if (node->binding) {
				walk_expr(node->binding->initializer.get());
				declare_in_scope(emit_var(*node->binding, current_function,
						uint32_t(std::max(node->binding->line, 1)), end, false));
			}
			walk_block(node->then_branch, end);
			scopes.pop_back();
			walk_block(node->else_branch, scope_end);
			return;
		}
		if (const auto *node = dynamic_cast<const WhileStmt *>(stmt)) {
			walk_expr(node->condition.get());
			walk_block(node->body, block_end_line(uint32_t(std::max(node->line, 1))));
			return;
		}
		if (const auto *node = dynamic_cast<const ForStmt *>(stmt)) {
			walk_expr(node->iterable.get());
			const uint32_t line = uint32_t(std::max(node->line, 1));
			const uint32_t end = block_end_line(line);
			scopes.emplace_back();
			const int32_t index = add(DeclarationKind::VARIABLE, node->variable, line,
					current_function, line, end);
			model.declarations[size_t(index)].resolved_type.clear();
			used[size_t(index)] = 1;
			declare_in_scope(index);
			walk_block(node->body, end);
			scopes.pop_back();
			return;
		}
		if (const auto *node = dynamic_cast<const MatchStmt *>(stmt)) {
			walk_expr(node->subject.get());
			for (const MatchStmt::Branch &branch : node->branches) {
				const uint32_t line = branch.patterns.empty() ? uint32_t(std::max(node->line, 1)) :
						uint32_t(std::max(branch.patterns.front()->line, 1));
				const uint32_t end = block_end_line(line);
				scopes.emplace_back();
				for (const MatchPatternPtr &pattern : branch.patterns) {
					walk_pattern(pattern.get(), end);
				}
				walk_expr(branch.guard.get());
				walk_block(branch.body, end);
				scopes.pop_back();
			}
			return;
		}
		if (const auto *node = dynamic_cast<const ExprStmt *>(stmt)) {
			check_statement_expression(node);
			walk_expr(node->expression.get());
			return;
		}
	}

	void walk_accessor(const FunctionDecl &node, uint32_t scope_end) {
		scopes.emplace_back();
		for (const Parameter &parameter : node.parameters) {
			const uint32_t line = uint32_t(std::max(parameter.line, std::max(node.line, 1)));
			const int32_t index = add(DeclarationKind::PARAMETER, parameter.name, line,
					current_function, line, block_end_line(uint32_t(std::max(node.line, 1))));
			used[size_t(index)] = 1;
			declare_in_scope(index);
		}
		walk_block(node.body, scope_end);
		scopes.pop_back();
	}

	void walk_pattern(const MatchPattern *pattern, uint32_t scope_end) {
		if (pattern == nullptr) return;
		if (pattern->kind == MatchPattern::Kind::BIND) {
			const uint32_t line = uint32_t(std::max(pattern->line, 1));
			const int32_t index = add(DeclarationKind::VARIABLE, pattern->name, line,
					current_function, line, scope_end);
			used[size_t(index)] = 1;
			declare_in_scope(index);
			return;
		}
		walk_expr(pattern->value.get());
		for (const MatchPatternPtr &element : pattern->elements) walk_pattern(element.get(), scope_end);
		for (const MatchPattern::Entry &entry : pattern->entries) {
			walk_expr(entry.key.get());
			walk_pattern(entry.value.get(), scope_end);
		}
		for (const MatchPattern::StructEntry &entry : pattern->struct_entries) {
			walk_pattern(entry.value.get(), scope_end);
		}
	}

	void check_statement_expression(const ExprStmt *stmt) {
		const Expr *expr = stmt->expression.get();
		if (expr == nullptr || dynamic_cast<const ErrorExpr *>(expr) != nullptr) return;
		if (const auto *call = dynamic_cast<const CallExpr *>(expr)) {
			if (const FunctionDecl *function = find_function(call->function_name)) {
				const std::string returns = type_name_of(function->return_type);
				if (!returns.empty() && returns != "void") {
					warn("DISCARDED_RETURN_VALUE",
							"Return value of '" + call->function_name + "' is discarded",
							expr->line, int(column_of(uint32_t(std::max(expr->line, 1)),
							call->function_name)), call->function_name.size());
				}
			}
			return;
		}
		if (const auto *member = dynamic_cast<const MemberCallExpr *>(expr)) {
			if (member->is_method_call) return;
		}
		if (dynamic_cast<const AwaitExpr *>(expr) != nullptr) return;
		warn("STANDALONE_EXPRESSION", "Standalone expression has no effect", stmt->line, 1);
	}

	void check_call_arity(const CallExpr *call) {
		const FunctionDecl *function = find_function(call->function_name);
		if (function == nullptr || call->has_named_arguments()) return;
		const size_t given = call->arguments.size();
		const size_t declared = function->parameters.size();
		if (given > declared) {
			fail("TOO_MANY_ARGUMENTS", "Too many arguments to '" + call->function_name +
					"': expected at most " + std::to_string(declared) + ", got " +
					std::to_string(given), call->line,
					int(column_of(uint32_t(std::max(call->line, 1)), call->function_name)),
					call->function_name.size());
			return;
		}
		for (size_t i = given; i < declared; i++) {
			if (function->parameters[i].default_value) continue;
			fail("MISSING_ARGUMENT", "Missing argument '" + function->parameters[i].name +
					"' in call to '" + call->function_name + "'", call->line,
					int(column_of(uint32_t(std::max(call->line, 1)), call->function_name)),
					call->function_name.size());
			return;
		}
	}

	void check_member_access(const MemberCallExpr *member) {
		const std::string receiver = type_of(member->object.get());
		if (const EnumDecl *declaration = find_enum(receiver)) {
			if (declaration->find_member(member->member_name) == nullptr) {
				fail("UNDECLARED_ENUM_MEMBER", "Enum '" + receiver + "' has no member named '" +
						member->member_name + "'", member->line,
						int(column_of(uint32_t(std::max(member->line, 1)), member->member_name)),
						member->member_name.size());
			}
			return;
		}
		const StructDecl *declaration = find_struct(receiver);
		if (declaration != nullptr && !declaration->is_class) {
			if (declaration->find_field(member->member_name) == nullptr &&
					declaration->find_method(member->member_name) == nullptr &&
					declaration->find_constant(member->member_name) == nullptr &&
					member->member_name != "new") {
				fail("UNKNOWN_STRUCT_FIELD", "Struct '" + declaration->name + "' has no field '" +
						member->member_name + "'", member->line,
						int(column_of(uint32_t(std::max(member->line, 1)), member->member_name)),
						member->member_name.size());
			}
			return;
		}
		const auto *object = dynamic_cast<const VariableExpr *>(member->object.get());
		if (object == nullptr || !receiver.empty()) return;
		const int32_t index = lookup(object->name);
		if (index < 0) return;
		const SourceDeclaration &value = model.declarations[size_t(index)];
		if (!value.declared_type.empty() || !value.resolved_type.empty()) return;
		const int column = int(column_of(uint32_t(std::max(member->line, 1)), object->name));
		if (member->is_method_call) {
			warn("UNSAFE_METHOD_ACCESS",
					"Method access on untyped value '" + object->name + "' is unsafe",
					member->line, column, object->name.size());
		} else {
			warn("UNSAFE_PROPERTY_ACCESS",
					"Property access on untyped value '" + object->name + "' is unsafe",
					member->line, column, object->name.size());
		}
	}

	void walk_expr(const Expr *expr) {
		if (expr == nullptr) return;
		if (const auto *node = dynamic_cast<const VariableExpr *>(expr)) {
			use(node->name);
			return;
		}
		if (const auto *node = dynamic_cast<const LiteralExpr *>(expr)) {
			if (node->lit_type == LiteralExpr::Type::STRING) {
				literal_strings.insert(std::get<std::string>(node->value));
			}
			return;
		}
		if (const auto *node = dynamic_cast<const CallExpr *>(expr)) {
			if (!node->is_node_path_sugar) {
				use(node->function_name);
				check_call_arity(node);
				if (node->function_name == "assert" && !node->arguments.empty()) {
					const auto *condition = dynamic_cast<const LiteralExpr *>(node->arguments[0].get());
					if (condition != nullptr && condition->lit_type == LiteralExpr::Type::BOOL) {
						warn("CONSTANT_ASSERT", "Assertion condition is constant", expr->line, 1);
					}
				}
			}
			for (const ExprPtr &argument : node->arguments) walk_expr(argument.get());
			return;
		}
		if (const auto *node = dynamic_cast<const MemberCallExpr *>(expr)) {
			check_member_access(node);
			walk_expr(node->object.get());
			for (const ExprPtr &argument : node->arguments) walk_expr(argument.get());
			return;
		}
		if (const auto *node = dynamic_cast<const BinaryExpr *>(expr)) {
			if (node->op == BinaryExpr::Op::DIV && type_of(node->left.get()) == "int" &&
					type_of(node->right.get()) == "int") {
				warn("INTEGER_DIVISION", "Integer division discards the fractional part",
						expr->line, expr->column);
			}
			walk_expr(node->left.get());
			walk_expr(node->right.get());
			return;
		}
		if (const auto *node = dynamic_cast<const UnaryExpr *>(expr)) {
			walk_expr(node->operand.get());
			return;
		}
		if (const auto *node = dynamic_cast<const AwaitExpr *>(expr)) {
			const Expr *operand = node->operand.get();
			if (dynamic_cast<const AwaitExpr *>(operand) != nullptr ||
					dynamic_cast<const LiteralExpr *>(operand) != nullptr) {
				warn("REDUNDANT_AWAIT", "Redundant await", expr->line, 1);
			}
			walk_expr(operand);
			return;
		}
		if (const auto *node = dynamic_cast<const TernaryExpr *>(expr)) {
			walk_expr(node->condition.get());
			walk_expr(node->true_value.get());
			walk_expr(node->false_value.get());
			return;
		}
		if (const auto *node = dynamic_cast<const IndexExpr *>(expr)) {
			walk_expr(node->object.get());
			walk_expr(node->index.get());
			return;
		}
		if (const auto *node = dynamic_cast<const CastExpr *>(expr)) {
			walk_expr(node->value.get());
			return;
		}
		if (const auto *node = dynamic_cast<const TypeTestExpr *>(expr)) {
			walk_expr(node->value.get());
			return;
		}
		if (const auto *node = dynamic_cast<const ArrayLiteralExpr *>(expr)) {
			for (const ExprPtr &element : node->elements) walk_expr(element.get());
			return;
		}
		if (const auto *node = dynamic_cast<const DictionaryLiteralExpr *>(expr)) {
			for (const auto &entry : node->elements) {
				walk_expr(entry.first.get());
				walk_expr(entry.second.get());
			}
			return;
		}
		if (const auto *node = dynamic_cast<const LambdaExpr *>(expr)) {
			if (!node->decl) return;
			const uint32_t line = uint32_t(std::max(node->decl->line, 1));
			const uint32_t end = block_end_line(line);
			scopes.emplace_back();
			for (const Parameter &parameter : node->decl->parameters) {
				const int32_t index = add(DeclarationKind::PARAMETER, parameter.name, line,
						current_function, line, end);
				model.declarations[size_t(index)].declared_type = parameter.type_hint.empty() ?
						std::string() : parameter.type_hint.to_string();
				model.declarations[size_t(index)].resolved_type = type_name_of(parameter.type_hint);
				declare_in_scope(index);
			}
			walk_block(node->decl->body, end);
			scopes.pop_back();
			return;
		}
	}

	void report_unused() {
		for (size_t i = 0; i < model.declarations.size(); i++) {
			const SourceDeclaration &declaration = model.declarations[i];
			const bool local = declaration.parent >= 0 &&
					model.declarations[size_t(declaration.parent)].kind == DeclarationKind::FUNCTION;
			if (!used[i] && !local && unresolved.count(declaration.name) != 0) {
				used[i] = 1;
			}
		}
		for (size_t i = 0; i < model.declarations.size(); i++) {
			const SourceDeclaration &declaration = model.declarations[i];
			if (used[i] || declaration.name.empty() || declaration.name[0] == '_') continue;
			const int line = int(declaration.declaration.start_line);
			const int column = int(declaration.declaration.start_column);
			const size_t width = declaration.name.size();
			switch (declaration.kind) {
				case DeclarationKind::PARAMETER:
					warn("UNUSED_PARAMETER", "Parameter '" + declaration.name + "' is never used",
							line, column, width);
					break;
				case DeclarationKind::VARIABLE:
					warn("UNUSED_VARIABLE", "Variable '" + declaration.name + "' is never used",
							line, column, width);
					break;
				case DeclarationKind::CONSTANT:
					warn("UNUSED_LOCAL_CONSTANT", "Local constant '" + declaration.name +
							"' is never used", line, column, width);
					break;
				default:
					break;
			}
		}
		for (size_t i = 0; i < model.declarations.size(); i++) {
			const SourceDeclaration &declaration = model.declarations[i];
			if (declaration.kind != DeclarationKind::SIGNAL || declaration.name.empty()) continue;
			if (unresolved.count(declaration.name) != 0 || used[i] != 0) continue;
			if (literal_strings.count(declaration.name) != 0) continue;
			warn("UNUSED_SIGNAL", "Signal '" + declaration.name + "' is never used",
					int(declaration.declaration.start_line),
					int(declaration.declaration.start_column), declaration.name.size());
		}
	}
};

// Type the chain head; the host resolves the rest via ClassDB.
std::string caret_head_type(const SourceModel &model, const Program &program,
		const std::string &head, uint32_t line) {
	std::string text = trim(head);
	if (text.empty() || text[0] == '$' || text[0] == '%') return {};
	const size_t open = text.find('(');
	const bool is_call = open != std::string::npos;
	const std::string name = trim(is_call ? text.substr(0, open) : text);
	if (name.empty()) return {};
	for (char c : name) {
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return {};
	}
	auto names_a_type = [&](const std::string &wanted) {
		if (is_builtin_type_name(wanted)) return true;
		for (const StructDecl &declaration : program.structs) if (declaration.name == wanted) return true;
		for (const EnumDecl &declaration : program.enums) if (declaration.name == wanted) return true;
		for (const TraitDecl &declaration : program.traits) if (declaration.name == wanted) return true;
		return false;
	};
	if (!is_call) {
		const SourceDeclaration *best = nullptr;
		for (const SourceDeclaration &declaration : model.declarations) {
			if (declaration.name != name) continue;
			if (declaration.kind != DeclarationKind::VARIABLE &&
					declaration.kind != DeclarationKind::CONSTANT &&
					declaration.kind != DeclarationKind::PARAMETER) continue;
			if (declaration.parent >= 0 && (declaration.declaration.start_line > line ||
					line > declaration.lexical_scope.end_line)) continue;
			if (best == nullptr || declaration.declaration.start_line >= best->declaration.start_line) {
				best = &declaration;
			}
		}
		if (best != nullptr) {
			return best->resolved_type.empty() ? best->declared_type : best->resolved_type;
		}
		return names_a_type(name) ? name : std::string();
	}
	if (names_a_type(name)) return name;
	for (const FunctionDecl &declaration : program.functions) {
		if (declaration.name == name) return type_name_of(declaration.return_type);
	}
	if (const GlobalFunction *global = find_global_function(name)) {
		return global_result_type(*global);
	}
	return {};
}

std::string receiver_head(const std::string &chain) {
	int depth = 0;
	char quote = 0;
	for (size_t i = 0; i < chain.size(); i++) {
		const char c = chain[i];
		if (quote != 0) {
			if (c == '\\') i++;
			else if (c == quote) quote = 0;
			continue;
		}
		if (c == '"' || c == '\'') quote = c;
		else if (c == '(' || c == '[') depth++;
		else if (c == ')' || c == ']') depth--;
		else if (c == '.' && depth == 0) return chain.substr(0, i);
	}
	return chain;
}

} // namespace

std::vector<uint8_t> encode_source_model(const SourceModel &model) {
	std::vector<uint8_t> out;
	scalar<uint32_t>(out, MAGIC); scalar<uint16_t>(out, MAJOR); scalar<uint16_t>(out, MINOR);
	scalar<uint32_t>(out, 8);
	std::vector<uint8_t> body;
	string(body, model.path); section(out, META, body); body.clear();
	scalar<uint32_t>(body, uint32_t(model.diagnostics.size()));
	for (const auto &d : model.diagnostics) {
		scalar<uint8_t>(body, uint8_t(d.severity)); string(body, d.code); string(body, d.message);
		string(body, d.path); range(body, d.range);
	}
	section(out, DIAGNOSTICS, body); body.clear();
	scalar<uint32_t>(body, uint32_t(model.declarations.size()));
	for (const auto &d : model.declarations) {
		scalar<uint8_t>(body, uint8_t(d.kind)); string(body, d.name); string(body, d.declared_type);
		string(body, d.resolved_type); range(body, d.declaration); range(body, d.lexical_scope);
		scalar<int32_t>(body, d.parent); scalar<uint32_t>(body, d.flags); string(body, d.documentation);
		scalar<uint32_t>(body, uint32_t(d.children.size())); for (int32_t v : d.children) scalar<int32_t>(body, v);
		scalar<uint32_t>(body, uint32_t(d.parameters.size()));
		for (const auto &p : d.parameters) { string(body, p.name); string(body, p.declared_type); string(body, p.default_text); range(body, p.declaration); }
		string(body, d.return_type);
		scalar<uint32_t>(body, uint32_t(d.enum_members.size()));
		for (const auto &e : d.enum_members) { string(body, e.name); scalar<int64_t>(body, e.value); range(body, e.declaration); }
		scalar<uint32_t>(body, uint32_t(d.annotation_arguments.size())); for (const auto &a : d.annotation_arguments) string(body, a);
	}
	section(out, DECLARATIONS, body); body.clear();
	const std::vector<uint8_t> props = encode_property_signatures(model.properties);
	body.insert(body.end(), props.begin(), props.end()); section(out, PROPERTIES, body); body.clear();
	scalar<uint8_t>(body, uint8_t(model.caret.kind)); scalar<int32_t>(body, model.caret.declaration);
	string(body, model.caret.receiver_text); string(body, model.caret.receiver_type); string(body, model.caret.callee);
	scalar<int32_t>(body, model.caret.argument_index); section(out, CARET, body); body.clear();
	scalar<uint32_t>(body, uint32_t(model.safe_lines.size())); for (uint32_t line : model.safe_lines) scalar<uint32_t>(body, line);
	section(out, SAFE_LINES, body); body.clear();
	scalar<uint32_t>(body, uint32_t(model.declarations.size()));
	for (const auto &d : model.declarations) string(body, d.base_type);
	section(out, DECLARATION_TYPES, body); body.clear();
	scalar<uint32_t>(body, uint32_t(model.declarations.size()));
	for (const auto &d : model.declarations) {
		string(body, d.initializer_text); string(body, d.setter); string(body, d.getter);
	}
	section(out, DECLARATION_VALUES, body);
	return out;
}

bool decode_source_model(const uint8_t *data, size_t size, SourceModel &out) {
	std::string ignored;
	return decode_source_model(data, size, out, ignored);
}

bool decode_source_model(const uint8_t *data, size_t size, SourceModel &out,
		std::string &error) {
	out = SourceModel{};
	error.clear();
	auto reject = [&error](std::string reason) {
		error = std::move(reason);
		return false;
	};
	if (data == nullptr) return reject("source model data is null");
	if (size > MAX_BLOB) return reject("source model exceeds the 32 MiB size limit");
	Reader reader{data, size};
	const uint32_t magic = reader.scalar<uint32_t>();
	const uint16_t major = reader.scalar<uint16_t>();
	if (!reader.ok) return reject("source model header is truncated");
	if (magic != MAGIC) return reject("source model has an invalid magic value");
	if (major != MAJOR) return reject("source model uses unsupported major version " +
			std::to_string(major));
	reader.scalar<uint16_t>();
	const uint32_t section_count = reader.scalar<uint32_t>();
	if (!reader.ok) return reject("source model header is truncated");
	if (section_count > 256) return reject("source model contains too many sections");
	SourceModel staged;
	for (uint32_t s = 0; s < section_count; s++) {
		const uint32_t id = reader.scalar<uint32_t>(); const uint32_t length = reader.scalar<uint32_t>();
		if (!reader.ok) return reject("source model section header " + std::to_string(s) +
				" is truncated");
		if (reader.at > size || length > size - reader.at) return reject(
				"source model section " + std::to_string(id) + " is truncated");
		Reader part{reader.data + reader.at, length}; reader.at += length;
		if (id == META) {
			staged.path = part.string();
		}
		else if (id == DIAGNOSTICS) {
			const uint32_t count = part.scalar<uint32_t>();
			if (count > MAX_RECORDS) return reject("source model contains too many diagnostics");
			for (uint32_t i = 0; i < count; i++) {
				SourceDiagnostic d; const uint8_t severity = part.scalar<uint8_t>();
				if (severity > uint8_t(DiagnosticSeverity::INFO)) return reject("diagnostic " +
						std::to_string(i) + " has unsupported severity " +
						std::to_string(severity));
				d.severity = DiagnosticSeverity(severity); d.code = part.string(); d.message = part.string(); d.path = part.string(); d.range = part.range();
				if (!part.ok) return reject("diagnostic " + std::to_string(i) + " is truncated");
				if (!valid_range(d.range)) return reject("diagnostic " + std::to_string(i) +
						" has an invalid source range");
				staged.diagnostics.push_back(std::move(d));
			}
		} else if (id == DECLARATIONS) {
			const uint32_t count = part.scalar<uint32_t>();
			if (count > MAX_RECORDS) return reject("source model contains too many declarations");
			for (uint32_t i = 0; i < count; i++) {
				SourceDeclaration d; const uint8_t kind = part.scalar<uint8_t>();
				if (kind >= uint8_t(DeclarationKind::COUNT)) return reject("declaration " +
						std::to_string(i) + " has unsupported kind " + std::to_string(kind));
				d.kind = DeclarationKind(kind); d.name = part.string(); d.declared_type = part.string(); d.resolved_type = part.string(); d.declaration = part.range(); d.lexical_scope = part.range(); d.parent = part.scalar<int32_t>(); d.flags = part.scalar<uint32_t>(); d.documentation = part.string();
				uint32_t n = part.scalar<uint32_t>(); if (n > MAX_RECORDS) return reject("declaration " + std::to_string(i) + " has too many children"); for (uint32_t j = 0; j < n; j++) d.children.push_back(part.scalar<int32_t>());
				n = part.scalar<uint32_t>(); if (n > MAX_RECORDS) return reject("declaration " + std::to_string(i) + " has too many parameters"); for (uint32_t j = 0; j < n; j++) { SourceParameter p; p.name = part.string(); p.declared_type = part.string(); p.default_text = part.string(); p.declaration = part.range(); d.parameters.push_back(std::move(p)); }
				d.return_type = part.string(); n = part.scalar<uint32_t>(); if (n > MAX_RECORDS) return reject("declaration " + std::to_string(i) + " has too many enum members");
				for (uint32_t j = 0; j < n; j++) { SourceEnumMember e; e.name = part.string(); e.value = part.scalar<int64_t>(); e.declaration = part.range(); d.enum_members.push_back(std::move(e)); }
				n = part.scalar<uint32_t>(); if (n > MAX_RECORDS) return reject("declaration " + std::to_string(i) + " has too many annotation arguments"); for (uint32_t j = 0; j < n; j++) d.annotation_arguments.push_back(part.string());
				if (!part.ok) return reject("declaration " + std::to_string(i) + " is truncated");
				if (!valid_range(d.declaration) || !valid_range(d.lexical_scope)) return reject(
						"declaration " + std::to_string(i) + " has an invalid source range");
				staged.declarations.push_back(std::move(d));
			}
		} else if (id == PROPERTIES) {
			if (!decode_property_signatures(part.data, part.size, staged.properties)) return reject(
					"source model contains an invalid property signature table");
			part.at = part.size;
		} else if (id == CARET) {
			const uint8_t kind = part.scalar<uint8_t>(); if (kind > uint8_t(CaretKind::STRING_NAME)) return reject(
					"caret context has unsupported kind " + std::to_string(kind));
			staged.caret.kind = CaretKind(kind); staged.caret.declaration = part.scalar<int32_t>(); staged.caret.receiver_text = part.string(); staged.caret.receiver_type = part.string(); staged.caret.callee = part.string(); staged.caret.argument_index = part.scalar<int32_t>();
		} else if (id == DECLARATION_TYPES) {
			const uint32_t count = part.scalar<uint32_t>();
			if (count > MAX_RECORDS) return reject("source model contains too many declaration types");
			if (count != staged.declarations.size()) return reject(
					"source model declaration type table does not match its declarations");
			for (uint32_t i = 0; i < count; i++) staged.declarations[i].base_type = part.string();
		} else if (id == DECLARATION_VALUES) {
			const uint32_t count = part.scalar<uint32_t>();
			if (count > MAX_RECORDS) return reject("source model contains too many declaration values");
			if (count != staged.declarations.size()) return reject(
					"source model declaration value table does not match its declarations");
			for (uint32_t i = 0; i < count; i++) {
				staged.declarations[i].initializer_text = part.string();
				staged.declarations[i].setter = part.string();
				staged.declarations[i].getter = part.string();
			}
		} else if (id == SAFE_LINES) {
			const uint32_t count = part.scalar<uint32_t>();
			if (count > MAX_RECORDS) return reject("source model contains too many safe lines");
			for (uint32_t i = 0; i < count; i++) staged.safe_lines.push_back(part.scalar<uint32_t>());
		} else { part.at = part.size; }
		if (!part.ok) return reject("source model section " + std::to_string(id) + " is truncated");
		if (part.at != part.size) return reject("source model section " + std::to_string(id) +
				" contains trailing data");
	}
	if (!reader.ok) return reject("source model is truncated");
	if (reader.at != size) return reject("source model contains trailing data");
	for (size_t i = 0; i < staged.declarations.size(); i++) {
		const auto &d = staged.declarations[i];
		if (d.parent < -1 || d.parent >= int32_t(staged.declarations.size())) return reject("declaration " +
				std::to_string(i) + " refers to missing parent " + std::to_string(d.parent));
		for (int32_t child : d.children) if (child < 0 || child >= int32_t(staged.declarations.size())) return reject(
				"declaration " + std::to_string(i) + " refers to missing child " +
				std::to_string(child));
	}
	for (size_t i = 0; i < staged.declarations.size(); i++) {
		const auto &d = staged.declarations[i];
		int depth = 0; int32_t parent = d.parent;
		while (parent >= 0 && depth++ <= 64) { parent = staged.declarations[size_t(parent)].parent; }
		if (depth > 64) return reject("declaration " + std::to_string(i) +
				" has an invalid parent cycle or nesting depth");
	}
	if (staged.caret.declaration < -1 ||
			staged.caret.declaration >= int32_t(staged.declarations.size())) return reject(
			"caret context refers to missing declaration " +
			std::to_string(staged.caret.declaration));
	out = std::move(staged); return true;
}

SourceModel analyze_source(const std::string &source, const std::string &path,
		uint32_t flags, int32_t caret_line, int32_t caret_column) {
	SourceModel model; model.path = path;
	const std::vector<std::string> lines = split_lines(source);

	DiagnosticSink sink;
	sink.limit = MAX_DIAGNOSTICS;
	std::vector<Token> tokens;
	Lexer lexer(source);
	lexer.set_diagnostics(&sink);
	try {
		tokens = lexer.tokenize();
	} catch (...) {
		tokens.clear();
	}
	Program program;
	std::vector<std::pair<int, std::string>> doc_comments = lexer.doc_comments();
	if (!tokens.empty()) {
		Parser parser(tokens);
		parser.set_doc_comments(std::move(doc_comments));
		parser.set_diagnostics(&sink);
		try {
			program = parser.parse();
		} catch (...) {
		}
	}
	if ((flags & ANALYZE_DIAGNOSTICS) != 0) {
		for (const ParseDiagnostic &entry : sink.diagnostics) {
			if (model.diagnostics.size() >= MAX_DIAGNOSTICS) break;
			model.diagnostics.push_back({DiagnosticSeverity::ERROR, entry.code, entry.message,
				path, {uint32_t(entry.line), uint32_t(entry.column), uint32_t(entry.end_line),
				uint32_t(entry.end_column)}});
		}
	}
	if ((flags & ANALYZE_SAFE_LINES) != 0) {
		uint32_t previous = 0;
		for (const Token &token : tokens) {
			if (token.type == TokenType::NEWLINE || token.type == TokenType::INDENT ||
					token.type == TokenType::DEDENT || token.type == TokenType::EOF_TOKEN) continue;
			if (uint32_t(token.line) == previous) continue;
			previous = uint32_t(token.line);
			model.safe_lines.push_back(previous);
		}
	}
	if ((flags & ANALYZE_DECLARATIONS) != 0) {
		ModelBuilder builder(model, program, lines);
		builder.warnings_wanted = (flags & ANALYZE_DIAGNOSTICS) != 0;
		builder.build();
	}
	std::stable_sort(model.diagnostics.begin(), model.diagnostics.end(),
			[](const SourceDiagnostic &a, const SourceDiagnostic &b) {
				if (a.range.start_line != b.range.start_line) return a.range.start_line < b.range.start_line;
				return a.range.start_column < b.range.start_column;
			});
	// Apply warning annotations as lexical state after warnings have been
	// collected. Codes are compared case-insensitively with GDScript's setting
	// names (for example UNUSED_VARIABLE vs "unused_variable").
	if ((flags & ANALYZE_DIAGNOSTICS) != 0 && !model.diagnostics.empty()) {
		std::vector<std::unordered_set<std::string>> ignored(lines.size() + 1);
		std::unordered_set<std::string> active;
		std::unordered_set<std::string> one_shot;
		for (size_t i = 0; i < lines.size(); i++) {
			const std::string text = trim(lines[i]);
			auto annotation_code = [&]() {
				const size_t quote = text.find_first_of("\"'");
				if (quote == std::string::npos) return std::string();
				const size_t end = text.find(text[quote], quote + 1);
				if (end == std::string::npos) return std::string();
				std::string code = text.substr(quote + 1, end - quote - 1);
				std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return char(std::toupper(c)); });
				return code;
			};
			if (text.rfind("@warning_ignore_start", 0) == 0) active.insert(annotation_code());
			else if (text.rfind("@warning_ignore_restore", 0) == 0) active.erase(annotation_code());
			else if (text.rfind("@warning_ignore", 0) == 0) one_shot.insert(annotation_code());
			ignored[i + 1] = active;
			if (!one_shot.empty() && !text.empty() && text[0] != '@' && text[0] != '#') {
				ignored[i + 1].insert(one_shot.begin(), one_shot.end());
				one_shot.clear();
			}
		}
		model.diagnostics.erase(std::remove_if(model.diagnostics.begin(), model.diagnostics.end(),
				[&](const SourceDiagnostic &diagnostic) {
					return diagnostic.severity == DiagnosticSeverity::WARNING &&
							diagnostic.range.start_line < ignored.size() &&
							ignored[diagnostic.range.start_line].count(diagnostic.code) != 0;
				}), model.diagnostics.end());
	}
	if ((flags & ANALYZE_DIAGNOSTICS) != 0 && model.diagnostics.size() >= MAX_DIAGNOSTICS) {
		model.diagnostics.resize(MAX_DIAGNOSTICS - 1);
		model.diagnostics.push_back({DiagnosticSeverity::ERROR, "TOO_MANY_ERRORS", "Too many errors",
			model.path, {uint32_t(lines.size()), 1, uint32_t(lines.size()), 2}});
	}
	if ((flags & ANALYZE_CARET) != 0 && caret_line > 0 && size_t(caret_line) <= lines.size()) {
		std::vector<bool> continuation(lines.size(), false);
		{
			int depth = 0;
			for (size_t i = 0; i < lines.size(); i++) {
				continuation[i] = depth > 0;
				bool in_string = false;
				char quote = 0;
				const std::string &raw = lines[i];
				for (size_t at = 0; at < raw.size(); at++) {
					const char c = raw[at];
					if (in_string) {
						if (c == '\\') at++;
						else if (c == quote) in_string = false;
						continue;
					}
					if (c == '#') break;
					if (c == '"' || c == '\'') { in_string = true; quote = c; }
					else if (c == '(' || c == '[' || c == '{') depth++;
					else if ((c == ')' || c == ']' || c == '}') && depth > 0) depth--;
				}
			}
		}
		size_t first = size_t(caret_line - 1);
		while (first > 0 && continuation[first]) first--;
		std::string before;
		for (size_t i = first; i + 1 < size_t(caret_line); i++) {
			std::string code = lines[i];
			const size_t comment = code.find('#');
			if (comment != std::string::npos) code = code.substr(0, comment);
			before += trim(code);
			before += ' ';
		}
		const std::string &caret_text = lines[size_t(caret_line - 1)];
		before += caret_text.substr(0, std::min<size_t>(size_t(std::max(caret_column, 0)), caret_text.size()));
		analyze_caret(before, model.caret);
		if (!model.caret.receiver_text.empty() && (flags & ANALYZE_DECLARATIONS) != 0) {
			model.caret.receiver_type = caret_head_type(model, program,
					receiver_head(model.caret.receiver_text), uint32_t(caret_line));
		}
	}
	return model;
}

} // namespace gdscript
