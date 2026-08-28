#include "script_language_safegdscript.h"
#include "compiler_backend.h"
#include "editor_analysis_safegdscript.h"
#include "../script_language_common.h"
#include "script_safegdscript.h"
#include "../sandbox.h"

void safegdscript_sandbox_profiling_toggled(Sandbox &p_sandbox, bool p_enabled);
void safegdscript_sync_engine_breakpoints();
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/input_map.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Forward-declared; globals.h pulls variant_types.h whose namespace Variant
// clashes with godot::Variant.
namespace gdscript {
struct GlobalConstant;
struct GlobalFunction;
const GlobalConstant *find_global_constant(const std::string &name);
const GlobalFunction *find_global_function(const std::string &name);
size_t global_constant_count();
const char *global_constant_name(size_t index);
size_t global_function_count();
const char *global_function_name(size_t index);
size_t builtin_constant_count();
const char *builtin_constant_type(size_t index);
const char *builtin_constant_name(size_t index);
size_t global_enum_value_count();
const char *global_enum_value_enum(size_t index);
const char *global_enum_value_name(size_t index);
bool is_global_enum(const std::string &name);
} // namespace gdscript
static constexpr const char *icon_path = "res://addons/godot_sandbox/SafeGDScript.svg";

static SafeGDScriptLanguage *safegdscript_language;

void SafeGDScriptLanguage::_bind_methods() {
	ClassDB::bind_method(D_METHOD("editor_validate", "script", "path", "functions", "errors",
			"warnings", "safe_lines"), &SafeGDScriptLanguage::editor_validate,
			DEFVAL(true), DEFVAL(true), DEFVAL(true), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("editor_complete", "code", "path", "owner"),
			&SafeGDScriptLanguage::editor_complete, DEFVAL(nullptr));
	ClassDB::bind_method(D_METHOD("editor_lookup", "code", "symbol", "path", "owner"),
			&SafeGDScriptLanguage::editor_lookup, DEFVAL(nullptr));
}

Dictionary SafeGDScriptLanguage::editor_validate(const String &p_script, const String &p_path,
		bool p_functions, bool p_errors, bool p_warnings, bool p_safe_lines) const {
	ERR_FAIL_COND_V_MSG(!Engine::get_singleton()->is_editor_hint(), Dictionary(),
			"SafeGDScript.editor_validate() is available only in editor mode.");
	return _validate(p_script, p_path, p_functions, p_errors, p_warnings, p_safe_lines);
}

Dictionary SafeGDScriptLanguage::editor_complete(const String &p_code, const String &p_path,
		Object *p_owner) const {
	ERR_FAIL_COND_V_MSG(!Engine::get_singleton()->is_editor_hint(), Dictionary(),
			"SafeGDScript.editor_complete() is available only in editor mode.");
	return _complete_code(p_code, p_path, p_owner);
}

Dictionary SafeGDScriptLanguage::editor_lookup(const String &p_code, const String &p_symbol,
		const String &p_path, Object *p_owner) const {
	ERR_FAIL_COND_V_MSG(!Engine::get_singleton()->is_editor_hint(), Dictionary(),
			"SafeGDScript.editor_lookup() is available only in editor mode.");
	return _lookup_code(p_code, p_symbol, p_path, p_owner);
}

namespace {

// ---------------------------------------------------------------------------
// Editor support helpers (syntax colors, source scanning, code completion)
//
// SafeGDScript is compiled by the GDScript compiler that lives inside a Sandbox,
// so no parser is available on the host side. Everything below works off the
// source text with a line-based scanner, which is enough for the editor basics:
// the members overview, "go to function", auto-indent and code completion.
// ---------------------------------------------------------------------------

constexpr char32_t COMPLETION_MARKER = 0xFFFF; // Inserted by the editor at the caret.

bool is_identifier_char(char32_t c) {
	return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

int skip_spaces(const String &p_text, int p_from) {
	while (p_from < p_text.length() && (p_text[p_from] == ' ' || p_text[p_from] == '\t')) {
		p_from++;
	}
	return p_from;
}

String identifier_at(const String &p_text, int p_from) {
	int end = p_from;
	while (end < p_text.length() && is_identifier_char(p_text[end])) {
		end++;
	}
	return p_text.substr(p_from, end - p_from);
}

// Cut the line at the first '#' that is not inside a string literal.
String strip_comment(const String &p_line) {
	bool in_string = false;
	char32_t quote = 0;
	for (int i = 0; i < p_line.length(); i++) {
		const char32_t c = p_line[i];
		if (in_string) {
			if (c == '\\') {
				i++;
			} else if (c == quote) {
				in_string = false;
			}
		} else if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
		} else if (c == '#') {
			return p_line.substr(0, i);
		}
	}
	return p_line;
}

struct SourceSymbol {
	String name;
	int line = 0; // 1-based, as the editor counts lines.
	// Parameters as the 'func' line writes them, kept for the call hint the
	// editor shows while a call to this function is being typed.
	PackedStringArray parameters;
};

// A struct declaration and the fields its body declares. A struct is sugar for
// a Dictionary with a fixed set of keys, so the field names are everything the
// editor needs to complete a member access on one.
struct SourceStruct {
	String name;
	int line = 0;
	std::vector<String> fields;
};

struct SourceSymbols {
	std::vector<SourceSymbol> functions;
	std::vector<SourceSymbol> signals;
	std::vector<SourceSymbol> variables;
	std::vector<SourceSymbol> constants;
	std::vector<SourceStruct> structs;
	// Names whose type the source states: a ': Type' hint on a variable or a
	// parameter, or an assignment from Type.new(). The type is stored as
	// written, and what it names -- a struct, a class Godot knows, or something
	// neither of us does -- is decided where it is used.
	std::vector<std::pair<String, String>> declared_types;
	// What an 'extends' line names, which is what the script is attached to when
	// the editor has no instance to ask.
	String base_class;
};

int leading_indent(const String &p_line) {
	int indent = 0;
	while (indent < p_line.length() && (p_line[indent] == ' ' || p_line[indent] == '\t')) {
		indent++;
	}
	return indent;
}

// The type a declaration names, from a ': Type' hint or from the constructor it
// is assigned: 'var a: Foo', 'var a := Foo.new()', 'var a = Foo()'.
String declared_type(const String &p_line, int p_after_name) {
	int p = skip_spaces(p_line, p_after_name);
	if (p < p_line.length() && p_line[p] == ':') {
		p = skip_spaces(p_line, p + 1);
		if (p < p_line.length() && p_line[p] != '=') {
			return identifier_at(p_line, p);
		}
	}
	if (p < p_line.length() && p_line[p] == '=') {
		return identifier_at(p_line, skip_spaces(p_line, p + 1));
	}
	return String();
}

// The parameters of a 'func' line: each one as written, for the call hint, and
// the type-hinted ones also as name/type pairs.
void scan_parameters(const String &p_line, PackedStringArray &r_parameters, std::vector<std::pair<String, String>> &r_typed) {
	const int open = p_line.find("(");
	const int close = p_line.rfind(")");
	if (open < 0 || close <= open) {
		return;
	}
	const String inside = p_line.substr(open + 1, close - open - 1).strip_edges();
	if (inside.is_empty()) {
		return;
	}
	const PackedStringArray parameters = inside.split(",");
	for (int i = 0; i < parameters.size(); i++) {
		const String parameter = parameters[i].strip_edges();
		if (parameter.is_empty()) {
			continue;
		}
		r_parameters.push_back(parameter);
		const int colon = parameter.find(":");
		if (colon < 0) {
			continue;
		}
		const String name = parameter.substr(0, colon).strip_edges();
		// A default value follows the hint, and is not part of the type name.
		const String type = parameter.substr(colon + 1).get_slice("=", 0).strip_edges();
		if (!name.is_empty() && !type.is_empty()) {
			r_typed.push_back({ name, type });
		}
	}
}

SourceSymbols scan_source(const String &p_code) {
	SourceSymbols symbols;
	const PackedStringArray lines = p_code.split("\n");
	int struct_indent = -1; // Indent of the 'struct' line whose body we are in.
	for (int i = 0; i < lines.size(); i++) {
		const String raw = strip_comment(lines[i]);
		const String line = raw.strip_edges();
		if (line.is_empty()) {
			continue;
		}
		const int indent = leading_indent(raw);
		if (struct_indent >= 0 && indent <= struct_indent) {
			struct_indent = -1; // The body ends at the first line back out.
		}
		if (struct_indent >= 0) {
			// Fields belong to the struct, not to the script's own symbols.
			if (line.begins_with("var ")) {
				const String name = identifier_at(line, skip_spaces(line, 4));
				if (!name.is_empty()) {
					symbols.structs.back().fields.push_back(name);
				}
			}
			continue;
		}
		if (line.begins_with("struct ")) {
			const String name = identifier_at(line, skip_spaces(line, 7));
			if (!name.is_empty()) {
				symbols.structs.push_back({ name, i + 1, {} });
				struct_indent = indent;
			}
		} else if (line.begins_with("func ") || line.begins_with("static func ")) {
			const int name_start = skip_spaces(line, line.begins_with("func ") ? 5 : 12);
			const String name = identifier_at(line, name_start);
			PackedStringArray parameters;
			scan_parameters(line, parameters, symbols.declared_types);
			if (!name.is_empty()) {
				symbols.functions.push_back({ name, i + 1, parameters });
			}
		} else if (line.begins_with("extends ")) {
			symbols.base_class = identifier_at(line, skip_spaces(line, 8));
		} else if (line.begins_with("signal ")) {
			const int name_start = skip_spaces(line, 7);
			const String name = identifier_at(line, name_start);
			PackedStringArray parameters;
			scan_parameters(line, parameters, symbols.declared_types);
			if (!name.is_empty()) {
				symbols.signals.push_back({ name, i + 1, parameters });
			}
		} else if (line.begins_with("var ")) {
			const int name_start = skip_spaces(line, 4);
			const String name = identifier_at(line, name_start);
			if (!name.is_empty()) {
				symbols.variables.push_back({ name, i + 1 });
				const String type = declared_type(line, name_start + name.length());
				if (!type.is_empty()) {
					symbols.declared_types.push_back({ name, type });
				}
			}
		} else if (line.begins_with("const ")) {
			const String name = identifier_at(line, skip_spaces(line, 6));
			if (!name.is_empty()) {
				symbols.constants.push_back({ name, i + 1 });
			}
		}
	}
	return symbols;
}

const SourceStruct *find_struct(const SourceSymbols &p_symbols, const String &p_name) {
	for (const SourceStruct &declaration : p_symbols.structs) {
		if (declaration.name == p_name) {
			return &declaration;
		}
	}
	return nullptr;
}

// The type the source states for a name, empty when it states none. The last
// statement wins, which is what a reader of the file would assume too.
String declared_type_of(const SourceSymbols &p_symbols, const String &p_name) {
	String type;
	for (const std::pair<String, String> &typed : p_symbols.declared_types) {
		if (typed.first == p_name) {
			type = typed.second;
		}
	}
	return type;
}

// What the caret is sitting on, derived from the code the editor hands us with
// COMPLETION_MARKER inserted at the caret position.
struct CompletionContext {
	String prefix; // Partial identifier being typed.
	String receiver; // Identifier in front of '.', empty when there is none.
	bool member = false; // Completing after a '.'.
	bool annotation = false; // Completing after a '@'.
	bool func_definition = false; // Completing the name in a 'func ' line.
	bool type_position = false; // After ':', '->', 'is', 'as', or 'extends'.
	char32_t scene_sugar = 0; // '$' or '%' immediately before the path prefix.
	String string_call; // Recognized call whose string argument owns the caret.
	String string_prefix;
	bool valid = true; // False inside a comment or a string literal.
	// The call the caret sits inside, for the signature shown above the caret
	// while its arguments are typed. Empty name when it sits inside none.
	String call_name;
	String call_receiver; // What the call is made on, empty for a plain call.
	int call_argument = 0; // Which argument the caret is on, counting from zero.
};

// Fill in the call the caret sits inside, including calls split across lines.
void find_enclosing_call(const String &p_code, int p_line_start, int p_caret, CompletionContext &r_ctx) {
	struct OpenBracket {
		int offset; // Where the '(' is, or -1 for a bracket that opens no call.
		int argument; // Commas seen inside it so far.
	};
	std::vector<OpenBracket> open;
	bool in_string = false;
	char32_t quote = 0;
	bool in_comment = false;
	for (int i = 0; i < p_caret; i++) {
		const char32_t c = p_code[i];
		if (in_comment) {
			if (c == '\n') in_comment = false;
			continue;
		}
		if (in_string) {
			if (c == '\\') {
				i++;
			} else if (c == quote) {
				in_string = false;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
		} else if (c == '#') {
			in_comment = true;
		} else if (c == '(') {
			// A '(' straight after an identifier opens a call; one after an
			// operator only groups an expression.
			const bool is_call = i > 0 && is_identifier_char(p_code[i - 1]);
			open.push_back({ is_call ? i : -1, 0 });
		} else if (c == '[' || c == '{') {
			open.push_back({ -1, 0 });
		} else if (c == ')' || c == ']' || c == '}') {
			if (!open.empty()) {
				open.pop_back();
			}
		} else if (c == ',' && !open.empty()) {
			open.back().argument++;
		}
	}

	for (int i = int(open.size()) - 1; i >= 0; i--) {
		if (open[size_t(i)].offset < 0) {
			continue;
		}
		const int name_end = open[size_t(i)].offset;
		int name_start = name_end;
		while (name_start > 0 && is_identifier_char(p_code[name_start - 1])) {
			name_start--;
		}
		r_ctx.call_name = p_code.substr(name_start, name_end - name_start);
		r_ctx.call_argument = open[size_t(i)].argument;
		if (name_start > 0 && p_code[name_start - 1] == '.') {
			int receiver_end = name_start - 1;
			int receiver_start = receiver_end;
			while (receiver_start > 0 && is_identifier_char(p_code[receiver_start - 1])) {
				receiver_start--;
			}
			r_ctx.call_receiver = p_code.substr(receiver_start, receiver_end - receiver_start);
		}
		return;
	}
}

CompletionContext analyze_completion(const String &p_code) {
	CompletionContext ctx;

	int caret = p_code.length();
	for (int i = 0; i < p_code.length(); i++) {
		if (p_code[i] == COMPLETION_MARKER) {
			caret = i;
			break;
		}
	}

	int line_start = 0;
	for (int i = caret - 1; i >= 0; i--) {
		if (p_code[i] == '\n') {
			line_start = i + 1;
			break;
		}
	}

	// Never complete inside a comment or an unterminated string literal.
	bool in_string = false;
	char32_t quote = 0;
	int quote_start = -1;
	for (int i = line_start; i < caret; i++) {
		const char32_t c = p_code[i];
		if (in_string) {
			if (c == '\\') {
				i++;
			} else if (c == quote) {
				in_string = false;
			}
		} else if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
			quote_start = i;
		} else if (c == '#') {
			ctx.valid = false;
			return ctx;
		}
	}
	if (in_string) {
		ctx.string_prefix = p_code.substr(quote_start + 1, caret - quote_start - 1);
		int at = quote_start - 1;
		while (at >= line_start && (p_code[at] == ' ' || p_code[at] == '\t')) at--;
		if (at >= line_start && p_code[at] == '(') {
			int end = at--;
			while (at >= line_start && is_identifier_char(p_code[at])) at--;
			ctx.string_call = p_code.substr(at + 1, end - at - 1);
		}
		find_enclosing_call(p_code, line_start, caret, ctx);
		ctx.valid = !ctx.string_call.is_empty();
		return ctx;
	}

	int start = caret;
	while (start > line_start && is_identifier_char(p_code[start - 1])) {
		start--;
	}
	ctx.prefix = p_code.substr(start, caret - start);
	find_enclosing_call(p_code, line_start, caret, ctx);
	if (start > line_start && (p_code[start - 1] == '$' || p_code[start - 1] == '%')) {
		ctx.scene_sugar = p_code[start - 1];
		return ctx;
	}

	if (start > line_start && p_code[start - 1] == '@') {
		ctx.annotation = true;
		return ctx;
	}
	if (start > line_start && p_code[start - 1] == '.') {
		ctx.member = true;
		const int receiver_end = start - 1;
		int receiver_start = receiver_end;
		while (receiver_start > line_start && is_identifier_char(p_code[receiver_start - 1])) {
			receiver_start--;
		}
		ctx.receiver = p_code.substr(receiver_start, receiver_end - receiver_start);
		return ctx;
	}

	// "func <caret>" means the user is naming a function: offer overridable ones.
	const String head = p_code.substr(line_start, start - line_start).strip_edges();
	ctx.func_definition = (head == "func" || head == "static func");
	ctx.type_position = head.ends_with(":") || head.ends_with("->") ||
			head.ends_with(" is") || head.ends_with(" as") || head == "extends";
	return ctx;
}

// Colors follow the user's editor theme so completion matches the highlighter.
struct CompletionColors {
	Color keyword = Color(1.0, 0.44, 0.52);
	Color control_flow = Color(1.0, 0.55, 0.8);
	Color function = Color(0.34, 0.7, 1.0);
	Color member = Color(0.74, 0.88, 1.0);
	Color type = Color(0.26, 1.0, 0.76);
	Color text = Color(0.85, 0.85, 0.85);
};

Color editor_color(const Ref<EditorSettings> &p_settings, const String &p_setting, const Color &p_fallback) {
	if (p_settings.is_null() || !p_settings->has_setting(p_setting)) {
		return p_fallback;
	}
	const Variant value = p_settings->get_setting(p_setting);
	if (value.get_type() != Variant::COLOR) {
		return p_fallback;
	}
	return value;
}

CompletionColors completion_colors() {
	CompletionColors colors;
	if (!Engine::get_singleton()->is_editor_hint() || EditorInterface::get_singleton() == nullptr) {
		return colors;
	}
	const Ref<EditorSettings> settings = EditorInterface::get_singleton()->get_editor_settings();
	colors.keyword = editor_color(settings, "text_editor/theme/highlighting/keyword_color", colors.keyword);
	colors.control_flow = editor_color(settings, "text_editor/theme/highlighting/control_flow_keyword_color", colors.control_flow);
	colors.function = editor_color(settings, "text_editor/theme/highlighting/function_color", colors.function);
	colors.member = editor_color(settings, "text_editor/theme/highlighting/member_variable_color", colors.member);
	colors.type = editor_color(settings, "text_editor/theme/highlighting/base_type_color", colors.type);
	colors.text = editor_color(settings, "text_editor/theme/highlighting/text_color", colors.text);
	return colors;
}

// All six keys are mandatory: complete_code() in script_language_extension.h
// does an ERR_CONTINUE on every one of them, so an option missing any single
// key is dropped with an error printed to the output log.
void add_option(Array &r_options, int p_kind, const String &p_display, const String &p_insert, const Color &p_color, int p_location = ScriptLanguageExtension::LOCATION_OTHER) {
	Dictionary option;
	option["kind"] = p_kind;
	option["display"] = p_display;
	option["insert_text"] = p_insert;
	option["font_color"] = p_color;
	option["icon"] = Variant();
	option["default_value"] = Variant();
	option["location"] = p_location;
	r_options.push_back(option);
}

void add_resource_paths(Array &r_options, const String &p_directory, const String &p_prefix,
		const CompletionColors &p_colors, int p_depth = 0) {
	if (p_depth > 16 || r_options.size() >= 512) return;
	for (const String &file : DirAccess::get_files_at(p_directory)) {
		if (file.ends_with(".import") || file.begins_with(".")) continue;
		const String path = p_directory.path_join(file);
		if (path.begins_with(p_prefix)) {
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FILE_PATH,
					path, path, p_colors.text);
		}
	}
	for (const String &directory : DirAccess::get_directories_at(p_directory)) {
		if (directory.begins_with(".")) continue;
		add_resource_paths(r_options, p_directory.path_join(directory), p_prefix, p_colors, p_depth + 1);
	}
}

void add_node_paths(Array &r_options, Node *p_root, Node *p_node, const String &p_prefix,
		const CompletionColors &p_colors, int p_depth = 0) {
	if (p_root == nullptr || p_node == nullptr || p_depth > 64 || r_options.size() >= 512) return;
	if (p_node != p_root) {
		const String path = String(p_root->get_path_to(p_node));
		if (path.begins_with(p_prefix)) {
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_NODE_PATH,
					path, path, p_colors.member);
		}
		if (p_node->is_unique_name_in_owner()) {
			const String unique = "%" + String(p_node->get_name());
			if (unique.begins_with(p_prefix)) {
				add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_NODE_PATH,
						unique, unique, p_colors.member);
			}
		}
	}
	const TypedArray<Node> children = p_node->get_children();
	for (int64_t i = 0; i < children.size(); i++) {
		add_node_paths(r_options, p_root, Object::cast_to<Node>(children[i]), p_prefix, p_colors,
				p_depth + 1);
	}
}

void add_scene_sugar_options(Array &r_options, Node *p_root, Node *p_node,
		char32_t p_sugar, const String &p_prefix, const CompletionColors &p_colors,
		int p_depth = 0) {
	if (p_root == nullptr || p_node == nullptr || p_depth > 64 || r_options.size() >= 512) return;
	if (p_node != p_root) {
		if (p_sugar == '$') {
			const String path = String(p_root->get_path_to(p_node));
			if (path.begins_with(p_prefix)) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_NODE_PATH,
					String("$") + path, path, p_colors.member);
		} else if (p_sugar == '%' && p_node->is_unique_name_in_owner()) {
			const String name = String(p_node->get_name());
			if (name.begins_with(p_prefix)) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_NODE_PATH,
					String("%") + name, name, p_colors.member);
		}
	}
	const TypedArray<Node> children = p_node->get_children();
	for (int64_t i = 0; i < children.size(); i++) {
		add_scene_sugar_options(r_options, p_root, Object::cast_to<Node>(children[i]),
				p_sugar, p_prefix, p_colors, p_depth + 1);
	}
}

void add_string_context_options(Array &r_options, const CompletionContext &p_ctx,
		const SourceSymbols &p_symbols, const StringName &p_owner_class, Object *p_owner,
		const CompletionColors &p_colors) {
	const String call = p_ctx.string_call;
	if (call == "preload" || call == "load" || call == "icon") {
		add_resource_paths(r_options, "res://", p_ctx.string_prefix, p_colors);
		return;
	}
	if (call == "export_file" || call == "export_global_file") {
		static const char *filters[] = { "*.tscn", "*.tres", "*.gd", "*.sgd", "*.png",
			"*.svg", "*.wav", "*.ogg", "*.json", "*.*", nullptr };
		for (const char **filter = filters; *filter != nullptr; filter++) {
			const String value(*filter);
			if (value.begins_with(p_ctx.string_prefix)) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_PLAIN_TEXT,
					value, value, p_colors.text);
		}
		return;
	}
	if (call == "rpc") {
		static const char *rpc_options[] = { "authority", "any_peer", "call_local", "call_remote",
			"reliable", "unreliable", "unreliable_ordered", nullptr };
		for (const char **option = rpc_options; *option != nullptr; option++) {
			const String value(*option);
			if (value.begins_with(p_ctx.string_prefix)) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
					value, value, p_colors.text);
		}
		return;
	}
	if (call.begins_with("warning_ignore")) {
		static const char *warning_codes[] = { "unused_variable", "unused_local_constant",
			"unused_parameter", "unused_signal", "shadowed_variable", "unreachable_code",
			"standalone_expression", "unassigned_variable", "discarded_return_value",
			"integer_division", "narrowing_conversion", "unsafe_method_access",
			"unsafe_property_access", "redundant_await", "constant_assert", nullptr };
		for (const char **code = warning_codes; *code != nullptr; code++) {
			const String value(*code);
			if (value.begins_with(p_ctx.string_prefix)) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
					value, value, p_colors.text);
		}
		return;
	}
	if (call == "export_node_path") {
		if (ClassDBSingleton *class_db = ClassDBSingleton::get_singleton()) {
			for (const String &name : class_db->get_class_list()) {
				if (class_db->is_parent_class(name, "Node") && name.begins_with(p_ctx.string_prefix)) {
					add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS,
							name, name, p_colors.type);
				}
			}
		}
		return;
	}
	if (call == "get_node" || call == "has_node") {
		Node *owner = Object::cast_to<Node>(p_owner);
		add_node_paths(r_options, owner, owner, p_ctx.string_prefix, p_colors);
		return;
	}
	if (call.begins_with("is_action_") || call == "get_action_strength" ||
			call == "get_axis" || call == "get_vector") {
		if (InputMap::get_singleton() != nullptr) {
			for (const StringName &action : InputMap::get_singleton()->get_actions()) {
				const String name(action);
				if (name.begins_with(p_ctx.string_prefix)) {
					add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
							name, name, p_colors.text);
				}
			}
		}
		return;
	}
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db == nullptr || !class_db->class_exists(p_owner_class)) return;
	if (call == "connect" || call == "emit_signal") {
		for (const SourceSymbol &source_signal : p_symbols.signals) {
			if (source_signal.name.begins_with(p_ctx.string_prefix)) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL, source_signal.name,
					source_signal.name, p_colors.function, ScriptLanguageExtension::LOCATION_LOCAL);
		}
		for (const Dictionary &signal : class_db->class_get_signal_list(p_owner_class, false)) {
			const String name = signal["name"];
			if (name.begins_with(p_ctx.string_prefix)) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL, name, name, p_colors.function);
		}
		return;
	}
	if (call == "call" || call == "has_method") {
		for (const Dictionary &method : class_db->class_get_method_list(p_owner_class, false)) {
			const String name = method["name"];
			if (name.begins_with(p_ctx.string_prefix)) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION, name, name, p_colors.function);
		}
		for (const SourceSymbol &function : p_symbols.functions) {
			if (function.name.begins_with(p_ctx.string_prefix)) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION, function.name,
					function.name, p_colors.function, ScriptLanguageExtension::LOCATION_LOCAL);
		}
		return;
	}
	if (call == "get" || call == "set") {
		for (const Dictionary &property : class_db->class_get_property_list(p_owner_class, false)) {
			const String name = property["name"];
			if (name.begins_with(p_ctx.string_prefix)) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER, name, name, p_colors.member);
		}
		for (const SourceSymbol &variable : p_symbols.variables) {
			if (variable.name.begins_with(p_ctx.string_prefix)) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER, variable.name,
					variable.name, p_colors.member, ScriptLanguageExtension::LOCATION_LOCAL);
		}
	}
}

// Variant types the compiler can construct inline, plus the rest of the type
// names that are useful in type hints.
const char *const builtin_types[] = {
	"bool", "int", "float", "String", "StringName", "NodePath",
	"Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i",
	"Rect2", "Rect2i", "Transform2D", "Transform3D", "Plane", "Quaternion",
	"AABB", "Basis", "Projection", "Color", "Array", "Dictionary", "Callable",
	"PackedByteArray", "PackedInt32Array", "PackedInt64Array",
	"PackedFloat32Array", "PackedFloat64Array", "PackedStringArray",
	"PackedVector2Array", "PackedVector3Array", "PackedVector4Array",
	"PackedColorArray",
	nullptr
};

// Types with an inline constructor in the compiler (codegen.cpp).
const char *const constructible_types[] = {
	"Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i",
	"Color", "Rect2", "Rect2i", "Plane", "Array", "Dictionary",
	"PackedByteArray", "PackedInt32Array", "PackedInt64Array",
	"PackedFloat32Array", "PackedFloat64Array", "PackedStringArray",
	"PackedVector2Array", "PackedVector3Array", "PackedVector4Array",
	"PackedColorArray",
	nullptr
};

// Completion and lookup both resolve from the compiler's tables.
void add_global_constants(Array &r_options, const CompletionColors &p_colors) {
	for (size_t i = 0; i < gdscript::global_constant_count(); i++) {
		const char *name = gdscript::global_constant_name(i);
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
				name, name, p_colors.text);
	}
}

// Built-in type constants (Vector2.ZERO, Color.RED). Returns false when none
// match, distinguishing type names from other receivers. Full scan; not
// contiguous by type.
bool add_builtin_constants(Array &r_options, const String &p_type, const CompletionColors &p_colors) {
	bool found = false;
	for (size_t i = 0; i < gdscript::builtin_constant_count(); i++) {
		if (p_type != gdscript::builtin_constant_type(i)) {
			continue;
		}
		const char *name = gdscript::builtin_constant_name(i);
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
				name, name, p_colors.text);
		found = true;
	}
	return found;
}

// Members of a @GlobalScope enum (Side.SIDE_LEFT). Same shape as the built-in
// type constants above: false when the receiver names no such enum, so the
// caller can go on looking. Full scan; rows are grouped but not indexed.
bool add_global_enum_members(Array &r_options, const String &p_enum, const CompletionColors &p_colors) {
	bool found = false;
	for (size_t i = 0; i < gdscript::global_enum_value_count(); i++) {
		if (p_enum != gdscript::global_enum_value_enum(i)) {
			continue;
		}
		const char *name = gdscript::global_enum_value_name(i);
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
				name, name, p_colors.text);
		found = true;
	}
	return found;
}

// The enum names themselves, offered where a type or a qualifier can go.
void add_global_enum_names(Array &r_options, const CompletionColors &p_colors) {
	String previous;
	for (size_t i = 0; i < gdscript::global_enum_value_count(); i++) {
		const String name = gdscript::global_enum_value_enum(i);
		if (name == previous) {
			continue; // Rows are grouped by enum; one option per group.
		}
		previous = name;
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_ENUM,
				name, name, p_colors.type);
	}
}

void add_global_functions(Array &r_options, const CompletionColors &p_colors) {
	for (size_t i = 0; i < gdscript::global_function_count(); i++) {
		const char *name = gdscript::global_function_name(i);
		if (name == nullptr) {
			continue; // Internal lowering form; not source-visible.
		}
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION,
				String(name) + String("("), String(name) + String("("), p_colors.function);
	}
}

// Members of the built-in Variant types. Member calls compile to a vcall on the
// Variant, so any of these work regardless of what the receiver turns out to be.
const char *const variant_properties[] = {
	"x", "y", "z", "w", "r", "g", "b", "a", "position", "size", "end",
	"normal", "d", "origin", "basis", "start",
	nullptr
};

const char *const variant_methods[] = {
	// Arrays and dictionaries
	"size", "is_empty", "clear", "append", "append_array", "push_back", "push_front",
	"pop_back", "pop_front", "pop_at", "insert", "remove_at", "resize", "fill",
	"has", "has_all", "count", "find", "rfind", "erase", "duplicate", "slice",
	"sort", "reverse", "shuffle", "front", "back", "get", "set", "keys", "values",
	"merge", "max", "min", "sum",
	// Strings
	"length", "begins_with", "ends_with", "contains", "substr", "split", "join",
	"replace", "strip_edges", "to_lower", "to_upper", "to_int", "to_float",
	"format", "left", "right", "pad_zeros", "hash", "num", "repeat",
	// Vectors, colors and other math types
	"length_squared", "normalized", "is_normalized", "dot", "cross", "abs",
	"floor", "ceil", "round", "sign", "clamp", "lerp", "distance_to",
	"distance_squared_to", "angle", "angle_to", "direction_to", "rotated",
	"snapped", "is_equal_approx", "inverse", "to_rgba32", "to_html",
	nullptr
};

void add_class_members(Array &r_options, const StringName &p_class, const CompletionColors &p_colors) {
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db == nullptr || !class_db->class_exists(p_class)) {
		return;
	}
	const TypedArray<Dictionary> methods = class_db->class_get_method_list(p_class, false);
	for (int i = 0; i < methods.size(); i++) {
		const Dictionary method = methods[i];
		const String name = method["name"];
		if (name.is_empty() || name.begins_with("_")) {
			continue; // Internal and virtual methods are not callable from a script.
		}
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION, name + String("("), name + String("("), p_colors.function);
	}
	const TypedArray<Dictionary> properties = class_db->class_get_property_list(p_class, false);
	for (int i = 0; i < properties.size(); i++) {
		const Dictionary property = properties[i];
		const String name = property["name"];
		// Groups and categories are property list entries without a type.
		if (name.is_empty() || int(property["type"]) == Variant::NIL) {
			continue;
		}
		if (name.contains("/") || name.contains(" ")) {
			continue;
		}
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER, name, name, p_colors.member);
	}
}

// Everything reachable through a value of the named type. False when the source
// names a type that is neither a struct nor a class Godot knows, which leaves
// the caller to fall back on the members any Variant might have.
bool add_type_members(Array &r_options, const String &p_type, const SourceSymbols &p_symbols, const CompletionColors &p_colors) {
	if (p_type.is_empty()) {
		return false;
	}
	if (const SourceStruct *declaration = find_struct(p_symbols, p_type)) {
		// A struct is a Dictionary with a fixed set of keys, so its declared
		// fields are the whole of it.
		for (const String &field : declaration->fields) {
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER, field, field,
					p_colors.member, ScriptLanguageExtension::LOCATION_LOCAL);
		}
		return true;
	}
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db != nullptr && class_db->class_exists(p_type)) {
		add_class_members(r_options, p_type, p_colors);
		return true;
	}
	return false;
}

// The script's own functions, offered wherever a call on the script itself is
// being written: after 'self.' as well as unqualified.
void add_script_functions(Array &r_options, const SourceSymbols &p_symbols, const CompletionColors &p_colors) {
	for (const SourceSymbol &function : p_symbols.functions) {
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION,
				function.name + String("("), function.name + String("("), p_colors.function,
				ScriptLanguageExtension::LOCATION_LOCAL);
	}
}

// -= Call hints =-

// A call's signature with the argument the caret is on wrapped in the marker
// character, which is how the editor knows which one to highlight. It is the
// same character it uses to tell us where the caret is.
String argument_hint(const String &p_name, const PackedStringArray &p_arguments, int p_current) {
	String hint = p_name + String("(");
	for (int i = 0; i < p_arguments.size(); i++) {
		if (i > 0) {
			hint += ", ";
		}
		const bool current = (i == p_current);
		if (current) {
			hint += String::chr(COMPLETION_MARKER);
		}
		hint += p_arguments[i];
		if (current) {
			hint += String::chr(COMPLETION_MARKER);
		}
	}
	hint += ")";
	return hint;
}

// The parameters of a method Godot knows, written the way a signature reads.
// False when the class has no such method.
bool engine_method_arguments(const StringName &p_class, const String &p_method, PackedStringArray &r_arguments) {
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db == nullptr || !class_db->class_exists(p_class) || !class_db->class_has_method(p_class, p_method, false)) {
		return false;
	}
	const TypedArray<Dictionary> methods = class_db->class_get_method_list(p_class, false);
	for (int i = 0; i < methods.size(); i++) {
		const Dictionary method = methods[i];
		if (String(method["name"]) != p_method) {
			continue;
		}
		const Array arguments = method["args"];
		for (int j = 0; j < arguments.size(); j++) {
			const Dictionary argument = arguments[j];
			const String name = argument["name"];
			const String class_name = argument.get("class_name", StringName());
			// A typed argument reads better as "name: Type", and the class name
			// is the only type an Object-valued one carries.
			if (!class_name.is_empty()) {
				r_arguments.push_back(name + String(": ") + class_name);
			} else if (int(argument["type"]) != Variant::NIL) {
				r_arguments.push_back(name + String(": ") + Variant::get_type_name(Variant::Type(int(argument["type"]))));
			} else {
				r_arguments.push_back(name);
			}
		}
		return true;
	}
	return false;
}

// The types the compiler builds inline, and what their components are called.
// Only these have a fixed shape the compiler itself relies on (codegen.cpp), so
// only these get a hint that is certain to be right.
struct InlineConstructor {
	const char *name;
	const char *arguments;
};

const InlineConstructor inline_constructors[] = {
	{ "Vector2", "x, y" },
	{ "Vector2i", "x, y" },
	{ "Vector3", "x, y, z" },
	{ "Vector3i", "x, y, z" },
	{ "Vector4", "x, y, z, w" },
	{ "Vector4i", "x, y, z, w" },
	{ "Color", "r, g, b, a" },
	{ nullptr, nullptr }
};

// The hint for the call the caret sits inside, empty when nothing is known
// about it. A wrong signature on screen is worse than none.
String call_hint_for(const CompletionContext &p_ctx, const SourceSymbols &p_symbols, const StringName &p_owner_class) {
	if (p_ctx.call_name.is_empty()) {
		return String();
	}

	// A call on a receiver reaches the receiver's class, and only a type the
	// source states says which class that is.
	if (!p_ctx.call_receiver.is_empty() && p_ctx.call_receiver != "self") {
		const String type = declared_type_of(p_symbols, p_ctx.call_receiver);
		PackedStringArray arguments;
		if (!type.is_empty() && engine_method_arguments(type, p_ctx.call_name, arguments)) {
			return argument_hint(p_ctx.call_name, arguments, p_ctx.call_argument);
		}
		return String();
	}

	// The script's own functions, whose parameters the source spells out.
	for (const SourceSymbol &function : p_symbols.functions) {
		if (function.name == p_ctx.call_name) {
			return argument_hint(function.name, function.parameters, p_ctx.call_argument);
		}
	}

	for (const InlineConstructor *constructor = inline_constructors; constructor->name != nullptr; constructor++) {
		if (p_ctx.call_name == constructor->name) {
			return argument_hint(p_ctx.call_name, String(constructor->arguments).split(", "), p_ctx.call_argument);
		}
	}

	if (p_ctx.call_name == "print") {
		return argument_hint("print", String("...").split(","), 0);
	}

	// Anything left unqualified compiles to a call on the node the script is
	// attached to.
	PackedStringArray arguments;
	if (engine_method_arguments(p_owner_class, p_ctx.call_name, arguments)) {
		return argument_hint(p_ctx.call_name, arguments, p_ctx.call_argument);
	}
	return String();
}

// Overridable methods of the base class, inserted as a complete signature.
void add_virtual_methods(Array &r_options, const StringName &p_class, const CompletionColors &p_colors) {
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db == nullptr || !class_db->class_exists(p_class)) {
		return;
	}
	const TypedArray<Dictionary> methods = class_db->class_get_method_list(p_class, false);
	for (int i = 0; i < methods.size(); i++) {
		const Dictionary method = methods[i];
		const String name = method["name"];
		if (name.is_empty() || !(int(method["flags"]) & METHOD_FLAG_VIRTUAL)) {
			continue;
		}
		String signature = name + String("(");
		const Array arguments = method["args"];
		for (int j = 0; j < arguments.size(); j++) {
			const Dictionary argument = arguments[j];
			if (j > 0) {
				signature += ", ";
			}
			signature += String(argument["name"]);
		}
		signature += String(")");
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION, signature, signature + String(":"), p_colors.function);
	}
}

using ValidationResult = GDScriptCompilerBackend::Validation;

bool validate_with_compiler(const String &p_source, const String &p_path,
		ValidationResult &r_result) {
	// The editor validates on every idle tick and asks about identical text
	// more than once, so one remembered answer keeps the compiler from running
	// again for nothing.
	static String cached_source;
	static String cached_path;
	static ValidationResult cached_result;
	static bool has_cached = false;
	if (has_cached && cached_source == p_source && cached_path == p_path) {
		r_result = cached_result;
		return true;
	}

	GDScriptCompilerBackend &compiler = gdscript_compiler::backend_for(false);
	if (!compiler.available()) {
		return false;
	}
	// prepare() resets sticky inputs so validation inherits nothing from a prior compile.
	gdscript_compiler::prepare(compiler, false,
		SafeGDScript::resolve_base_sources(p_source, p_path), p_path);

	ValidationResult result;
	if (!compiler.validate(p_source, result)) {
		return false;
	}

	cached_source = p_source;
	cached_path = p_path;
	cached_result = result;
	has_cached = true;
	r_result = result;
	return true;
}

bool analyze_with_compiler(const String &p_source, const String &p_path, uint32_t p_flags,
		int32_t p_caret_line, int32_t p_caret_column, gdscript::SourceModel &r_model) {
	const String canonical_path = p_path.simplify_path();
	const PackedStringArray base_sources = SafeGDScript::resolve_base_sources(p_source, canonical_path);
	const CharString source_utf8 = p_source.utf8();
	const std::string source_key(source_utf8.get_data(), size_t(source_utf8.length()));
	String context;
	for (const String &entry : base_sources) context += entry + String("\n");
	if (ProjectSettings *project = ProjectSettings::get_singleton()) {
		for (const Dictionary &entry : project->get_global_class_list()) {
			context += String(entry.get("class", String())) + ":" +
					String(entry.get("path", String())) + "\n";
		}
		for (const Dictionary &entry : project->get_property_list()) {
			const String setting = entry.get("name", String());
			if (setting.begins_with("autoload/")) context += setting + String("=") +
					String(project->get_setting(setting, String())) + String("\n");
		}
	}
	const CharString context_utf8 = context.utf8();
	const std::string key = std::string(canonical_path.utf8().get_data()) + "\n" + source_key +
			"\nctx=" + std::string(context_utf8.get_data(), size_t(context_utf8.length())) +
			"\n" + std::to_string(p_flags) + ":" + std::to_string(p_caret_line) + ":" +
			std::to_string(p_caret_column) + ":" + gdscript_compiler::policy_name();
	static std::unordered_map<std::string, std::vector<uint8_t>> cache;
	if (const auto it = cache.find(key); it != cache.end()) {
		return gdscript::decode_source_model(it->second.data(), it->second.size(), r_model);
	}
	GDScriptCompilerBackend &compiler = gdscript_compiler::backend_for(false);
	if (!compiler.available() || !compiler.can_analyze()) {
		return false;
	}
	gdscript_compiler::prepare(compiler, false,
			base_sources, canonical_path);
	GDScriptCompilerBackend::AnalysisRequest request;
	request.source = p_source;
	request.path = canonical_path;
	request.flags = p_flags;
	request.caret_line = p_caret_line;
	request.caret_column = p_caret_column;
	const PackedByteArray bytes = compiler.analyze(request);
	if (bytes.is_empty() || !gdscript::decode_source_model(bytes.ptr(), size_t(bytes.size()), r_model)) {
		return false;
	}
	if (cache.size() >= 64) {
		cache.erase(cache.begin());
	}
	cache.emplace(key, std::vector<uint8_t>(bytes.ptr(), bytes.ptr() + bytes.size()));
	return true;
}

// The class free-standing calls end up on: every unqualified call in a
// SafeGDScript compiles to self.<name>() on the node the script is attached to.
StringName owner_class(Object *p_owner, const SourceSymbols &p_symbols) {
	if (p_owner != nullptr) {
		return p_owner->get_class();
	}
	// With no instance to ask -- a script open in the editor but not attached
	// to anything -- the 'extends' line is what the script itself claims.
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (!p_symbols.base_class.is_empty() && class_db != nullptr && class_db->class_exists(p_symbols.base_class)) {
		return StringName(p_symbols.base_class);
	}
	return StringName("Sandbox");
}

} // namespace

void SafeGDScriptLanguage::init() {
	safegdscript_language = memnew(SafeGDScriptLanguage);
	Engine::get_singleton()->register_script_language(safegdscript_language);
	Sandbox::set_profiling_toggle_callback(safegdscript_sandbox_profiling_toggled);
}
void SafeGDScriptLanguage::deinit() {
	if (safegdscript_language) {
		Sandbox::set_profiling_toggle_callback(nullptr);
		Engine::get_singleton()->unregister_script_language(safegdscript_language);
		memdelete(safegdscript_language);
		safegdscript_language = nullptr;
	}
}

SafeGDScriptLanguage *SafeGDScriptLanguage::get_singleton() {
	return safegdscript_language;
}

String SafeGDScriptLanguage::_get_name() const {
	return "SafeGD";
}
void SafeGDScriptLanguage::_init() {}
String SafeGDScriptLanguage::_get_type() const {
	return "SafeGDScript";
}
String SafeGDScriptLanguage::_get_extension() const {
	return "sgd";
}
void SafeGDScriptLanguage::_finish() {}
PackedStringArray SafeGDScriptLanguage::_get_reserved_words() const {
	// Mirrors GDScript's list: .sgd is GDScript syntax, so the editor should
	// highlight the same words even where the compiler does not implement them
	// yet. Type names are included so they get highlighted as well.
	static const PackedStringArray reserved_words{
		// Control flow
		"break",
		"continue",
		"elif",
		"else",
		"if",
		"for",
		"match",
		"pass",
		"return",
		"switch",
		"when",
		"while",
		// Declarations
		"class",
		"class_name",
		"const",
		"enum",
		"extends",
		"func",
		"signal",
		"static",
		"struct",
		"var",
		// Other keywords
		"await",
		"breakpoint",
		"self",
		"super",
		"yield",
		// Operators
		"and",
		"as",
		"in",
		"is",
		"not",
		"or",
		// Literals
		"false",
		"null",
		"true",
		// Constants
		"INF",
		"NAN",
		"PI",
		"TAU",
		// Functions
		"assert",
		"preload",
		// Types
		"void",
		"bool",
		"int",
		"float",
		"String",
		"StringName",
		"NodePath",
		"Vector2",
		"Vector2i",
		"Vector3",
		"Vector3i",
		"Vector4",
		"Vector4i",
		"Rect2",
		"Rect2i",
		"Transform2D",
		"Transform3D",
		"Plane",
		"Quaternion",
		"AABB",
		"Basis",
		"Projection",
		"Color",
		"Array",
		"Dictionary",
		"Callable",
		"Signal",
		"PackedByteArray",
		"PackedInt32Array",
		"PackedInt64Array",
		"PackedFloat32Array",
		"PackedFloat64Array",
		"PackedStringArray",
		"PackedVector2Array",
		"PackedVector3Array",
		"PackedVector4Array",
		"PackedColorArray",
		"Node",
		"Object",
		"RefCounted",
		"Resource",
		"Sandbox",
	};
	return reserved_words;
}
bool SafeGDScriptLanguage::_is_control_flow_keyword(const String &p_keyword) const {
	static const std::unordered_set<std::string> control_flow_keywords{
		"break", "continue", "elif", "else", "if", "for", "match", "pass", "return", "switch", "when",
		"while"
	};
	return control_flow_keywords.find(p_keyword.utf8().get_data()) != control_flow_keywords.end();
}
PackedStringArray SafeGDScriptLanguage::_get_comment_delimiters() const {
	PackedStringArray comment_delimiters;
	comment_delimiters.push_back("#");
	return comment_delimiters;
}
PackedStringArray SafeGDScriptLanguage::_get_doc_comment_delimiters() const {
	PackedStringArray doc_comment_delimiters;
	doc_comment_delimiters.push_back("##");
	return doc_comment_delimiters;
}
PackedStringArray SafeGDScriptLanguage::_get_string_delimiters() const {
	PackedStringArray string_delimiters;
	string_delimiters.push_back("\" \"");
	string_delimiters.push_back("' '");
	string_delimiters.push_back("\"\"\" \"\"\"");
	string_delimiters.push_back("''' '''");
	return string_delimiters;
}
Ref<Script> SafeGDScriptLanguage::_make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	SafeGDScript *script = memnew(SafeGDScript);
	// A new file has to compile as it stands, or the editor greets it with an
	// error, so the template holds only what the compiler understands.
	String source;
	if (!p_base_class_name.is_empty()) {
		source += String("extends ") + p_base_class_name + String("\n\n");
	}
	source += "# SafeGDScript is compiled to RISC-V and runs inside a Sandbox.\n\n";
	source += "func _ready():\n\tprint(\"Hello from SafeGDScript\")\n";
	script->set_source_code(source);
	return Ref<Script>(script);
}
TypedArray<Dictionary> SafeGDScriptLanguage::_get_built_in_templates(const StringName &p_object) const {
	return TypedArray<Dictionary>();
}
bool SafeGDScriptLanguage::_is_using_templates() {
	return false;
}
Dictionary SafeGDScriptLanguage::_validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	Dictionary result;
	uint32_t flags = 0;
	if (p_validate_errors || p_validate_warnings) flags |= gdscript::ANALYZE_DIAGNOSTICS;
	if (p_validate_functions || p_validate_warnings) flags |= gdscript::ANALYZE_DECLARATIONS;
	if (p_validate_safe_lines) flags |= gdscript::ANALYZE_SAFE_LINES;
	gdscript::SourceModel model;
	const bool analyzed = analyze_with_compiler(p_script, p_path, flags, 0, 0, model);
	if (!analyzed) {
		const CharString source = p_script.utf8();
		const CharString path = p_path.utf8();
		model = gdscript::analyze_source(std::string(source.get_data(), source.length()),
				std::string(path.get_data(), path.length()), flags);
	}

	// Retain full compiler semantic validation while old and new compiler ELFs
	// coexist. The tolerant model contributes every recoverable syntax error;
	// this adds a semantic/codegen error the lightweight recovery pass cannot.
	ValidationResult validation;
	const bool validated = validate_with_compiler(p_script, p_path, validation);
	if (validated && !validation.valid) {
		bool duplicate = false;
		for (const gdscript::SourceDiagnostic &diagnostic : model.diagnostics) {
			duplicate |= int32_t(diagnostic.range.start_line) == validation.line &&
					int32_t(diagnostic.range.start_column) == validation.column;
		}
		if (!duplicate) {
			gdscript::SourceDiagnostic diagnostic;
			diagnostic.severity = gdscript::DiagnosticSeverity::ERROR;
			diagnostic.code = "COMPILER_ERROR";
			diagnostic.message = std::string(validation.message.utf8().get_data());
			diagnostic.path = std::string(p_path.utf8().get_data());
			diagnostic.range = {uint32_t(std::max(validation.line, 1)),
					uint32_t(std::max(validation.column, 1)), uint32_t(std::max(validation.line, 1)),
					uint32_t(std::max(validation.column + 1, 2))};
			model.diagnostics.push_back(std::move(diagnostic));
		}
	}

	bool valid = true;
	Array errors;
	Array warnings;
	for (const gdscript::SourceDiagnostic &diagnostic : model.diagnostics) {
		const String message = String::utf8(diagnostic.message.c_str(), diagnostic.message.size());
		if (diagnostic.severity == gdscript::DiagnosticSeverity::ERROR) {
			valid = false;
			if (p_validate_errors) {
				Dictionary error;
				error["line"] = int64_t(std::max(diagnostic.range.start_line, 1u));
				error["column"] = int64_t(std::max(diagnostic.range.start_column, 1u));
				error["message"] = message;
				error["path"] = diagnostic.path.empty() ? p_path :
						String::utf8(diagnostic.path.c_str(), diagnostic.path.size());
				errors.push_back(error);
			}
		} else if (diagnostic.severity == gdscript::DiagnosticSeverity::WARNING && p_validate_warnings) {
			const String code = String::utf8(diagnostic.code.c_str(), diagnostic.code.size());
			const StringName setting = String("debug/gdscript/warnings/") + code.to_lower();
			int32_t warning_mode = 1;
			if (ProjectSettings::get_singleton()->has_setting(setting)) {
				warning_mode = int32_t(ProjectSettings::get_singleton()->get_setting(setting));
			}
			if (warning_mode <= 0) continue;
			if (warning_mode >= 2) {
				valid = false;
				if (p_validate_errors) {
					Dictionary error;
					error["line"] = int64_t(std::max(diagnostic.range.start_line, 1u));
					error["column"] = int64_t(std::max(diagnostic.range.start_column, 1u));
					error["message"] = message;
					error["path"] = diagnostic.path.empty() ? p_path :
							String::utf8(diagnostic.path.c_str(), diagnostic.path.size());
					errors.push_back(error);
				}
				continue;
			}
			Dictionary warning;
			warning["line"] = int64_t(std::max(diagnostic.range.start_line, 1u));
			warning["code"] = code;
			warning["string"] = message;
			warnings.push_back(warning);
		}
	}
	result["valid"] = valid;
	if (p_validate_errors) result["errors"] = errors;
	if (p_validate_warnings) result["warnings"] = warnings;
	if (p_validate_safe_lines) {
		PackedInt32Array safe_lines;
		for (uint32_t line : model.safe_lines) safe_lines.push_back(int32_t(line));
		result["safe_lines"] = safe_lines;
	}
	if (p_validate_functions) {
		PackedStringArray functions;
		for (const gdscript::SourceDeclaration &declaration : model.declarations) {
			if (declaration.kind == gdscript::DeclarationKind::FUNCTION) {
				functions.push_back(String::utf8(declaration.name.c_str(), declaration.name.size()) +
						":" + itos(declaration.declaration.start_line));
			}
		}
		result["functions"] = functions;
	}
	return result;
}

String SafeGDScriptLanguage::_validate_path(const String &p_path) const {
	return String();
}
Object *SafeGDScriptLanguage::_create_script() const {
	SafeGDScript *script = memnew(SafeGDScript);
	return script;
}
bool SafeGDScriptLanguage::_has_named_classes() const {
	return false;
}
bool SafeGDScriptLanguage::_supports_builtin_mode() const {
	return true;
}
bool SafeGDScriptLanguage::_supports_documentation() const {
	return true;
}
bool SafeGDScriptLanguage::_can_inherit_from_file() const {
	return false;
}
bool SafeGDScriptLanguage::_can_make_function() const {
	return true;
}
int32_t SafeGDScriptLanguage::_find_function(const String &p_function, const String &p_code) const {
	for (const SourceSymbol &function : scan_source(p_code).functions) {
		if (function.name == p_function) {
			return function.line;
		}
	}
	return -1;
}
String SafeGDScriptLanguage::_make_function(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	String function = String("func ") + p_function_name + String("(");
	for (int i = 0; i < p_function_args.size(); i++) {
		if (i > 0) {
			function += ", ";
		}
		// Arguments arrive as "name:Type"; the compiler only understands the
		// simple built-in type hints, so the name alone is the safe choice.
		function += p_function_args[i].get_slice(":", 0);
	}
	function += String("):\n\tpass # Replace with function body.\n");
	return function;
}
Error SafeGDScriptLanguage::_open_in_external_editor(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) {
	return Error::OK;
}
bool SafeGDScriptLanguage::_overrides_external_editor() {
	return false;
}
Dictionary SafeGDScriptLanguage::_complete_code(const String &p_code, const String &p_path, Object *p_owner) const {
	Dictionary result;
	result["result"] = Error::OK;
	result["force"] = false;
	// Required by complete_code() even when there is nothing to hint at: it
	// fails the whole call with ERR_UNAVAILABLE when the key is missing.
	result["call_hint"] = String();

	Array options;
	const CompletionContext ctx = analyze_completion(p_code);
	if (!ctx.valid) {
		result["options"] = options;
		return result;
	}
	const CompletionColors colors = completion_colors();
	const int marker_at = p_code.find(String::chr(COMPLETION_MARKER));
	const int32_t caret_line = marker_at < 0 ? int32_t(p_code.count("\n") + 1)
			: int32_t(p_code.substr(0, marker_at).count("\n") + 1);
	gdscript::SourceModel semantic_model;
	const String semantic_source = p_code.replace(String::chr(COMPLETION_MARKER), String());
	const bool semantic = analyze_with_compiler(semantic_source, p_path,
			gdscript::ANALYZE_DECLARATIONS | gdscript::ANALYZE_CARET,
			caret_line, 0, semantic_model);

	// Symbols declared by the script itself. The caret marker is dropped so the
	// word being typed is not scanned as a declaration.
	const SourceSymbols symbols = scan_source(p_code.replace(String::chr(COMPLETION_MARKER), String()));
	const StringName owner = owner_class(p_owner, symbols);

	// The signature of the call being written, shown above the caret whether or
	// not there is anything to complete inside it.
	result["call_hint"] = call_hint_for(ctx, symbols, owner);
	if (ctx.scene_sugar != 0) {
		Node *scene_owner = Object::cast_to<Node>(p_owner);
		add_scene_sugar_options(options, scene_owner, scene_owner, ctx.scene_sugar,
				ctx.prefix, colors);
		result["force"] = true;
		result["options"] = options;
		return result;
	}
	if (!ctx.string_call.is_empty()) {
		add_string_context_options(options, ctx, symbols, owner, p_owner, colors);
		result["force"] = true;
		result["options"] = options;
		return result;
	}

	if (ctx.annotation) {
		static const char *annotations[] = {
			"export", "export_range", "export_enum", "export_exp_easing", "export_flags",
			"export_flags_2d_render", "export_flags_2d_physics", "export_flags_2d_navigation",
			"export_flags_3d_render", "export_flags_3d_physics", "export_flags_3d_navigation",
			"export_flags_avoidance", "export_file", "export_dir", "export_global_file",
			"export_global_dir", "export_multiline", "export_placeholder", "export_color_no_alpha",
			"export_node_path", "export_storage", "export_custom", "export_group", "export_subgroup",
			"export_category", "onready", "tool", "rpc", "icon", "warning_ignore",
			"warning_ignore_start", "warning_ignore_restore", "static_unload", "abstract", nullptr
		};
		for (const char **annotation = annotations; *annotation != nullptr; annotation++) {
			add_option(options, CODE_COMPLETION_KIND_PLAIN_TEXT, *annotation, *annotation, colors.keyword);
		}
		result["force"] = true;
	} else if (ctx.member) {
		if (ctx.receiver == "self" || ctx.receiver.is_empty()) {
			// Unqualified and 'self.' reach the same place: the node the script
			// is attached to, plus the script's own functions.
			add_class_members(options, owner, colors);
			add_script_functions(options, symbols, colors);
		} else if (find_struct(symbols, ctx.receiver) != nullptr) {
			// The struct itself: the only thing reachable through it is .new().
			add_option(options, CODE_COMPLETION_KIND_FUNCTION, "new(", "new(", colors.function, LOCATION_LOCAL);
		} else if (!add_type_members(options, declared_type_of(symbols, ctx.receiver), symbols, colors) &&
				!add_builtin_constants(options, ctx.receiver, colors) &&
				!add_global_enum_members(options, ctx.receiver, colors) &&
				!add_type_members(options, ctx.receiver, symbols, colors)) {
			// Neither a value whose type the source states nor a class name of
			// its own, so it is a Variant of a type only the compiler knows.
			// Member calls become vcalls, so offer the common Variant members.
			for (const char *const *name = variant_properties; *name != nullptr; name++) {
				add_option(options, CODE_COMPLETION_KIND_MEMBER, *name, *name, colors.member);
			}
			for (const char *const *name = variant_methods; *name != nullptr; name++) {
				add_option(options, CODE_COMPLETION_KIND_FUNCTION, String(*name) + String("("), String(*name) + String("("), colors.function);
			}
		}
		result["force"] = true;
	} else if (ctx.func_definition) {
		add_virtual_methods(options, owner, colors);
		result["force"] = true;
	} else if (ctx.type_position) {
		for (const char *const *name = builtin_types; *name != nullptr; name++) {
			add_option(options, CODE_COMPLETION_KIND_CLASS, *name, *name, colors.type);
		}
		ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
		if (class_db != nullptr) {
			const PackedStringArray classes = class_db->get_class_list();
			for (const String &name : classes) {
				add_option(options, CODE_COMPLETION_KIND_CLASS, name, name, colors.type);
			}
		}
		for (const SourceStruct &declaration : symbols.structs) {
			add_option(options, CODE_COMPLETION_KIND_CLASS, declaration.name, declaration.name,
					colors.type, LOCATION_LOCAL);
		}
		result["force"] = true;
	} else {
		for (const String &keyword : _get_reserved_words()) {
			const bool control_flow = _is_control_flow_keyword(keyword);
			add_option(options, CODE_COMPLETION_KIND_PLAIN_TEXT, keyword, keyword,
					control_flow ? colors.control_flow : colors.keyword);
		}
		for (const char *const *name = builtin_types; *name != nullptr; name++) {
			add_option(options, CODE_COMPLETION_KIND_CLASS, *name, *name, colors.type);
		}
		for (const char *const *name = constructible_types; *name != nullptr; name++) {
			add_option(options, CODE_COMPLETION_KIND_CLASS, String(*name) + String("("), String(*name) + String("("), colors.type);
		}
		add_global_constants(options, colors);
		add_global_enum_names(options, colors);
		add_global_functions(options, colors);
		ProjectSettings *project = ProjectSettings::get_singleton();
		if (project != nullptr) {
			for (const Dictionary &entry : project->get_global_class_list()) {
				const String name = entry.get("class", String());
				if (!name.is_empty()) add_option(options, CODE_COMPLETION_KIND_CLASS,
						name, name, colors.type);
			}
			for (const Dictionary &entry : project->get_property_list()) {
				const String setting = entry.get("name", String());
				if (!setting.begins_with("autoload/")) continue;
				const String name = setting.trim_prefix("autoload/");
				add_option(options, CODE_COMPLETION_KIND_VARIABLE, name, name, colors.member);
			}
		}

		// A struct name is both a type hint and its own constructor.
		for (const SourceStruct &declaration : symbols.structs) {
			add_option(options, CODE_COMPLETION_KIND_CLASS, declaration.name, declaration.name, colors.type, LOCATION_LOCAL);
			add_option(options, CODE_COMPLETION_KIND_CLASS, declaration.name + String("("), declaration.name + String("("), colors.type, LOCATION_LOCAL);
		}
		add_script_functions(options, symbols, colors);
		if (semantic) {
			EditorSymbolResolver resolver(semantic_model, p_path, p_owner);
			for (const gdscript::SourceDeclaration *declaration : resolver.visible_declarations(caret_line)) {
				const String name = String::utf8(declaration->name.c_str(), declaration->name.size());
				if (declaration->kind == gdscript::DeclarationKind::VARIABLE ||
						declaration->kind == gdscript::DeclarationKind::PARAMETER) {
					add_option(options, CODE_COMPLETION_KIND_VARIABLE, name, name, colors.member, LOCATION_LOCAL);
				} else if (declaration->kind == gdscript::DeclarationKind::CONSTANT) {
					add_option(options, CODE_COMPLETION_KIND_CONSTANT, name, name, colors.member, LOCATION_LOCAL);
				}
			}
		} else {
			for (const SourceSymbol &variable : symbols.variables) {
				add_option(options, CODE_COMPLETION_KIND_VARIABLE, variable.name, variable.name, colors.member, LOCATION_LOCAL);
			}
			for (const SourceSymbol &constant : symbols.constants) {
				add_option(options, CODE_COMPLETION_KIND_CONSTANT, constant.name, constant.name, colors.member, LOCATION_LOCAL);
			}
		}

		// Unqualified calls compile to self.<name>(), so the base class members
		// are reachable without writing self.
		add_class_members(options, owner, colors);
	}

	result["options"] = options;
	return result;
}
Dictionary SafeGDScriptLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	// Both "result" and "type" required: ERR_FAIL_COND_V on either missing.
	// Called on every hover; missing key → two errors per mouse move.
	Dictionary result;
	result["result"] = Error::ERR_CANT_RESOLVE;
	result["type"] = LOOKUP_RESULT_MAX;
	const SourceSymbols symbols = scan_source(p_code);
	gdscript::SourceModel semantic_model;
	if (analyze_with_compiler(p_code, p_path, gdscript::ANALYZE_DECLARATIONS,
			int32_t(p_code.count("\n") + 1), 0, semantic_model)) {
		EditorSymbolResolver resolver(semantic_model, p_path, p_owner);
		const EditorResolvedSymbol resolved = resolver.resolve(p_symbol,
				uint32_t(p_code.count("\n") + 1));
		if (resolved.declaration != nullptr) {
			result["result"] = Error::OK;
			result["type"] = LOOKUP_RESULT_SCRIPT_LOCATION;
			result["location"] = resolved.line;
			result["script_path"] = resolved.script_path;
			return result;
		}
	}

	// Something the file itself declares wins: ctrl-clicking a name in a script
	// should land on the line that declares it, not on the engine class that
	// happens to share its name.
	int line = 0;
	for (const SourceSymbol &function : symbols.functions) {
		if (function.name == p_symbol) {
			line = function.line;
		}
	}
	for (const SourceStruct &declaration : symbols.structs) {
		if (declaration.name == p_symbol) {
			line = declaration.line;
		}
	}
	for (const SourceSymbol &variable : symbols.variables) {
		if (variable.name == p_symbol) {
			line = variable.line;
		}
	}
	for (const SourceSymbol &constant : symbols.constants) {
		if (constant.name == p_symbol) {
			line = constant.line;
		}
	}
	if (line > 0) {
		result["result"] = Error::OK;
		result["type"] = LOOKUP_RESULT_SCRIPT_LOCATION;
		result["location"] = line;
		result["script_path"] = p_path;
		return result;
	}

	// Compiler-known globals; doc page is Godot's.
	const std::string symbol = std::string(p_symbol.utf8().get_data());
	if (gdscript::find_global_constant(symbol) != nullptr) {
		result["result"] = Error::OK;
		result["type"] = LOOKUP_RESULT_CLASS_CONSTANT;
		// PI/TAU/INF/NAN → @GDScript; rest → @GlobalScope (matches engine lookup).
		const bool gdscript_constant = p_symbol == "PI" || p_symbol == "TAU" ||
				p_symbol == "INF" || p_symbol == "NAN";
		result["class_name"] = gdscript_constant ? "@GDScript" : "@GlobalScope";
		result["class_member"] = p_symbol;
		return result;
	}
	if (gdscript::find_global_function(symbol) != nullptr) {
		result["result"] = Error::OK;
		result["type"] = LOOKUP_RESULT_CLASS_METHOD;
		result["class_name"] = "@GlobalScope";
		result["class_member"] = p_symbol;
		return result;
	}
	// A @GlobalScope enum (Side, Error, Key). Its members answer above, as the
	// constants they also are; the enum name itself is the enum page.
	if (gdscript::is_global_enum(symbol)) {
		result["result"] = Error::OK;
		result["type"] = LOOKUP_RESULT_CLASS_ENUM;
		result["class_name"] = "@GlobalScope";
		result["class_member"] = p_symbol;
		return result;
	}

	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db == nullptr) {
		return result;
	}
	if (class_db->class_exists(p_symbol)) {
		result["result"] = Error::OK;
		result["type"] = LOOKUP_RESULT_CLASS;
		result["class_name"] = p_symbol;
		return result;
	}

	// Otherwise it may be a member of the class the script is attached to, since
	// that is where an unqualified name compiles to.
	const StringName owner = owner_class(p_owner, symbols);
	if (class_db->class_exists(owner)) {
		if (class_db->class_has_method(owner, p_symbol, false)) {
			result["result"] = Error::OK;
			result["type"] = LOOKUP_RESULT_CLASS_METHOD;
			result["class_name"] = owner;
			result["class_member"] = p_symbol;
			return result;
		}
		if (class_db->class_has_signal(owner, p_symbol)) {
			result["result"] = Error::OK;
			result["type"] = LOOKUP_RESULT_CLASS_SIGNAL;
			result["class_name"] = owner;
			result["class_member"] = p_symbol;
			return result;
		}
		if (class_db->class_has_integer_constant(owner, p_symbol)) {
			result["result"] = Error::OK;
			result["type"] = LOOKUP_RESULT_CLASS_CONSTANT;
			result["class_name"] = owner;
			result["class_member"] = p_symbol;
			return result;
		}
		const TypedArray<Dictionary> properties = class_db->class_get_property_list(owner, false);
		for (int i = 0; i < properties.size(); i++) {
			const Dictionary property = properties[i];
			if (String(property["name"]) == p_symbol) {
				result["result"] = Error::OK;
				result["type"] = LOOKUP_RESULT_CLASS_PROPERTY;
				result["class_name"] = owner;
				result["class_member"] = p_symbol;
				return result;
			}
		}
	}

	// No match: result still holds the initial ERR_CANT_RESOLVE.
	return result;
}
String SafeGDScriptLanguage::_auto_indent_code(const String &p_code, int32_t p_from_line, int32_t p_to_line) const {
	// Same block-tracking indent as GDScript uses, with tabs.
	PackedStringArray lines = p_code.split("\n");
	std::vector<int> indent_stack;

	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i];
		int indent_chars = 0;
		while (indent_chars < line.length() && (line[indent_chars] == ' ' || line[indent_chars] == '\t')) {
			indent_chars++;
		}
		const String stripped = line.substr(indent_chars).strip_edges();
		if (stripped.is_empty() || stripped.begins_with("#")) {
			continue; // Blank lines and comments keep whatever the user wrote.
		}

		const int current_level = indent_stack.empty() ? 0 : indent_stack.back();
		if (indent_chars > current_level) {
			indent_stack.push_back(indent_chars);
		} else if (indent_chars < current_level) {
			while (!indent_stack.empty() && indent_stack.back() > indent_chars) {
				indent_stack.pop_back();
			}
			if (!indent_stack.empty() && indent_stack.back() != indent_chars) {
				indent_stack.push_back(indent_chars);
			}
		}

		if (i >= p_from_line) {
			if (i > p_to_line) {
				break;
			}
			line = String("\t").repeat(indent_stack.size()) + stripped;
			lines[i] = line;
		}
	}

	return String("\n").join(lines);
}
void SafeGDScriptLanguage::_add_global_constant(const StringName &p_name, const Variant &p_value) {}
void SafeGDScriptLanguage::_add_named_global_constant(const StringName &p_name, const Variant &p_value) {}
void SafeGDScriptLanguage::_remove_named_global_constant(const StringName &p_name) {}
void SafeGDScriptLanguage::_thread_enter() {}
void SafeGDScriptLanguage::_thread_exit() {}
void SafeGDScriptLanguage::_reload_all_scripts() {
	const std::vector<SafeGDScript *> scripts = SafeGDScript::live_script_snapshot();
	for (SafeGDScript *script : scripts) {
		if (script != nullptr) {
			script->_reload(true);
		}
	}
}

void SafeGDScriptLanguage::_reload_scripts(const Array &p_scripts, bool p_soft_reload) {
	HashSet<uint64_t> seen;
	for (int64_t i = 0; i < p_scripts.size(); i++) {
		const Variant value = p_scripts[i];
		if (value.get_type() != Variant::OBJECT) {
			continue;
		}
		SafeGDScript *script = Object::cast_to<SafeGDScript>(value.operator Object *());
		if (script == nullptr || seen.has(script->get_instance_id())) {
			continue;
		}
		seen.insert(script->get_instance_id());
		script->_reload(p_soft_reload);
	}
}

void SafeGDScriptLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {
	Ref<SafeGDScript> script = p_script;
	if (script.is_valid()) {
		script->_reload(p_soft_reload);
	}
}
PackedStringArray SafeGDScriptLanguage::_get_recognized_extensions() const {
	PackedStringArray array;
	array.push_back("sgd");
	return array;
}
TypedArray<Dictionary> SafeGDScriptLanguage::_get_public_functions() const {
	// print() is the one global the compiler lowers on its own; every other
	// unqualified call becomes a call on the node the script is attached to,
	// and those come from the class rather than from here.
	Dictionary argument;
	argument["name"] = "what";
	argument["type"] = Variant::NIL;
	argument["usage"] = PROPERTY_USAGE_NIL_IS_VARIANT;
	Array arguments;
	arguments.push_back(argument);

	Dictionary return_value;
	return_value["type"] = Variant::NIL;
	return_value["usage"] = PROPERTY_USAGE_NIL_IS_VARIANT;

	Dictionary print_info;
	print_info["name"] = "print";
	print_info["args"] = arguments;
	print_info["default_args"] = Array();
	print_info["return"] = return_value;
	print_info["flags"] = METHOD_FLAG_VARARG;

	TypedArray<Dictionary> functions;
	functions.push_back(print_info);
	return functions;
}
Dictionary SafeGDScriptLanguage::_get_public_constants() const {
	return Dictionary();
}
TypedArray<Dictionary> SafeGDScriptLanguage::_get_public_annotations() const {
	// @export is the only annotation the parser accepts (parser.cpp), and it
	// makes the variable it precedes a property of the sandboxed script.
	Dictionary export_info;
	export_info["name"] = "@export";
	export_info["args"] = Array();
	export_info["default_args"] = Array();
	export_info["flags"] = METHOD_FLAGS_DEFAULT;

	TypedArray<Dictionary> annotations;
	annotations.push_back(export_info);
	return annotations;
}
void SafeGDScriptLanguage::_frame() {
	static bool icon_registered = register_language_icons;
	if (!icon_registered && Engine::get_singleton()->is_editor_hint()) {
		icon_registered = true;
		load_icon();
		EditorInterface::get_singleton()->get_base_control()->connect("theme_changed", callable_mp(this, &SafeGDScriptLanguage::load_icon));
	}
	// Poll editor breakpoints (no GDExtension callback for toggle).
	safegdscript_sync_engine_breakpoints();

	static uint32_t base_poll = 0;
	if (Engine::get_singleton()->is_editor_hint() && ++base_poll % 30 == 0) {
		SafeGDScript::poll_base_sources();
	}
}
void SafeGDScriptLanguage::load_icon() {
	static bool reenter = false;
	if (reenter)
		return;
	reenter = true;
	if (Engine::get_singleton()->is_editor_hint() && FileAccess::file_exists(icon_path)) {
		Ref<Theme> editor_theme = EditorInterface::get_singleton()->get_editor_theme();
		if (editor_theme.is_valid() && !editor_theme->has_icon("SafeGDScript", "EditorIcons"))
		{
			ResourceLoader *resource_loader = ResourceLoader::get_singleton();
			Ref<Texture2D> tex = resource_loader->load(icon_path);
			editor_theme->set_icon("SafeGDScript", "EditorIcons", tex);
		}
	}
	reenter = false;
}
bool SafeGDScriptLanguage::_handles_global_class_type(const String &p_type) const {
	return p_type == "SafeGDScript" || p_type == "Sandbox";
}
Dictionary SafeGDScriptLanguage::_get_global_class_name(const String &p_path) const {
	Dictionary dict;
	if (p_path.is_empty()) {
		return dict;
	}
	// Godot queries this before anything is compiled, so class_name
	// comes from a text scan, not from SafeGDScript::class_name.
	String declared_name;
	String declared_base;
	if (FileAccess::file_exists(p_path)) {
		SafeGDScript::scan_class_header(FileAccess::get_file_as_string(p_path),
				&declared_name, &declared_base);
	}
	dict["name"] = declared_name.is_empty() ? SafeGDScript::PathToGlobalName(p_path) : declared_name;
	if (declared_base.is_empty() || declared_base.begins_with("res://") ||
			declared_base.begins_with("user://")) {
		dict["base_type"] = "Sandbox";
	} else {
		dict["base_type"] = declared_base;
	}
	dict["icon_path"] = String(icon_path);
	return dict;
}
