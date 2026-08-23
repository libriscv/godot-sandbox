#include "script_language_safegdscript.h"
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
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <string>
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
} // namespace gdscript
static constexpr const char *icon_path = "res://addons/godot_sandbox/SafeGDScript.svg";

static SafeGDScriptLanguage *safegdscript_language;

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
	bool valid = true; // False inside a comment or a string literal.
	// The call the caret sits inside, for the signature shown above the caret
	// while its arguments are typed. Empty name when it sits inside none.
	String call_name;
	String call_receiver; // What the call is made on, empty for a plain call.
	int call_argument = 0; // Which argument the caret is on, counting from zero.
};

// Fill in the call the caret sits inside. Only the caret's own line is
// considered: a call split across lines is rare enough not to be worth
// guessing at, and guessing wrong puts the wrong signature on screen.
void find_enclosing_call(const String &p_code, int p_line_start, int p_caret, CompletionContext &r_ctx) {
	struct OpenBracket {
		int offset; // Where the '(' is, or -1 for a bracket that opens no call.
		int argument; // Commas seen inside it so far.
	};
	std::vector<OpenBracket> open;
	bool in_string = false;
	char32_t quote = 0;
	for (int i = p_line_start; i < p_caret; i++) {
		const char32_t c = p_code[i];
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
		} else if (c == '(') {
			// A '(' straight after an identifier opens a call; one after an
			// operator only groups an expression.
			const bool is_call = i > p_line_start && is_identifier_char(p_code[i - 1]);
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
		while (name_start > p_line_start && is_identifier_char(p_code[name_start - 1])) {
			name_start--;
		}
		r_ctx.call_name = p_code.substr(name_start, name_end - name_start);
		r_ctx.call_argument = open[size_t(i)].argument;
		if (name_start > p_line_start && p_code[name_start - 1] == '.') {
			int receiver_end = name_start - 1;
			int receiver_start = receiver_end;
			while (receiver_start > p_line_start && is_identifier_char(p_code[receiver_start - 1])) {
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
		} else if (c == '#') {
			ctx.valid = false;
			return ctx;
		}
	}
	if (in_string) {
		ctx.valid = false;
		return ctx;
	}

	int start = caret;
	while (start > line_start && is_identifier_char(p_code[start - 1])) {
		start--;
	}
	ctx.prefix = p_code.substr(start, caret - start);
	find_enclosing_call(p_code, line_start, caret, ctx);

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

// -= Diagnostics =-
//
// The real parser lives inside the compiler sandbox, so the only way to know
// whether a .sgd file is well-formed is to ask it. validate() there compiles
// the source without emitting an ELF and reports the first error with its
// location, which is what the editor needs to underline the offending line.

struct ValidationResult {
	bool valid = true;
	int line = 0; // 1-based; 0 when the error carries no location.
	int column = 0;
	String message;
};

// Ask the compiler sandbox about this exact source. False when there is no
// compiler to ask, which leaves the editor with no errors rather than wrong
// ones.
bool validate_with_compiler(const String &p_source, ValidationResult &r_result) {
	// The editor validates on every idle tick and asks about identical text
	// more than once, so one remembered answer keeps the compiler from running
	// again for nothing.
	static String cached_source;
	static ValidationResult cached_result;
	static bool has_cached = false;
	if (has_cached && cached_source == p_source) {
		r_result = cached_result;
		return true;
	}

	Sandbox *compiler = SafeGDScript::get_compiler_sandbox();
	// An older gdscript.elf has no validate(), and guessing from a failed
	// compile() would put the error on a line we do not know.
	if (compiler == nullptr || !compiler->has_function("validate")) {
		return false;
	}

	GDExtensionCallError error;
	Variant source = p_source;
	const Variant *args[] = { &source };
	const Variant answer = compiler->vmcall_fn("validate", args, 1, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK || answer.get_type() != Variant::DICTIONARY) {
		return false;
	}

	const Dictionary reply = answer;
	ValidationResult result;
	result.valid = reply.get("valid", true);
	if (!result.valid) {
		result.line = reply.get("line", 0);
		result.column = reply.get("column", 0);
		result.message = reply.get("message", String());
		const String hint = reply.get("hint", String());
		if (!hint.is_empty()) {
			result.message += String(" (") + hint + String(")");
		}
		if (result.message.is_empty()) {
			result.message = "Compilation failed";
		}
	}

	cached_source = p_source;
	cached_result = result;
	has_cached = true;
	r_result = result;
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

	// The compiler sandbox is the only thing that can parse SafeGDScript, so a
	// missing one means no errors rather than false ones.
	ValidationResult validation;
	result["valid"] = !validate_with_compiler(p_script, validation) || validation.valid;

	if (p_validate_errors && !validation.valid) {
		Dictionary error;
		// The editor indexes lines and columns from 1, and an error the
		// compiler could not place would otherwise land outside the file.
		error["line"] = validation.line > 0 ? validation.line : 1;
		error["column"] = validation.column > 0 ? validation.column : 1;
		error["message"] = validation.message;
		error["path"] = p_path;
		Array errors;
		errors.push_back(error);
		result["errors"] = errors;
	}

	if (p_validate_functions) {
		// The members overview, which stays useful while the file is mid-edit
		// and does not parse, so it is scanned from the text rather than asked
		// of the compiler.
		PackedStringArray functions;
		for (const SourceSymbol &function : scan_source(p_script).functions) {
			functions.push_back(function.name + String(":") + itos(function.line));
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
	return false;
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

	// Symbols declared by the script itself. The caret marker is dropped so the
	// word being typed is not scanned as a declaration.
	const SourceSymbols symbols = scan_source(p_code.replace(String::chr(COMPLETION_MARKER), String()));
	const StringName owner = owner_class(p_owner, symbols);

	// The signature of the call being written, shown above the caret whether or
	// not there is anything to complete inside it.
	result["call_hint"] = call_hint_for(ctx, symbols, owner);

	if (ctx.annotation) {
		// @export is the only annotation the compiler understands (parser.cpp).
		add_option(options, CODE_COMPLETION_KIND_PLAIN_TEXT, "export", "export", colors.keyword);
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
		add_global_functions(options, colors);

		// A struct name is both a type hint and its own constructor.
		for (const SourceStruct &declaration : symbols.structs) {
			add_option(options, CODE_COMPLETION_KIND_CLASS, declaration.name, declaration.name, colors.type, LOCATION_LOCAL);
			add_option(options, CODE_COMPLETION_KIND_CLASS, declaration.name + String("("), declaration.name + String("("), colors.type, LOCATION_LOCAL);
		}
		add_script_functions(options, symbols, colors);
		for (const SourceSymbol &variable : symbols.variables) {
			add_option(options, CODE_COMPLETION_KIND_VARIABLE, variable.name, variable.name, colors.member, LOCATION_LOCAL);
		}
		for (const SourceSymbol &constant : symbols.constants) {
			add_option(options, CODE_COMPLETION_KIND_CONSTANT, constant.name, constant.name, colors.member, LOCATION_LOCAL);
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
void SafeGDScriptLanguage::_reload_all_scripts() {}
void SafeGDScriptLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {}
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
	if (!p_path.is_empty()) {
		dict["name"] = SafeGDScript::PathToGlobalName(p_path);
		dict["base_type"] = "Sandbox";
		dict["icon_path"] = String(icon_path);
	}
	return dict;
}
