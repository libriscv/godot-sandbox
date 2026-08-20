#include "script_language_safegdscript.h"
#include "../script_language_common.h"
#include "script_safegdscript.h"
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
#include <vector>
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
};

struct SourceSymbols {
	std::vector<SourceSymbol> functions;
	std::vector<SourceSymbol> variables;
	std::vector<SourceSymbol> constants;
};

SourceSymbols scan_source(const String &p_code) {
	SourceSymbols symbols;
	const PackedStringArray lines = p_code.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String line = strip_comment(lines[i]).strip_edges();
		if (line.is_empty()) {
			continue;
		}
		if (line.begins_with("func ")) {
			const String name = identifier_at(line, skip_spaces(line, 5));
			if (!name.is_empty()) {
				symbols.functions.push_back({ name, i + 1 });
			}
		} else if (line.begins_with("static func ")) {
			const String name = identifier_at(line, skip_spaces(line, 12));
			if (!name.is_empty()) {
				symbols.functions.push_back({ name, i + 1 });
			}
		} else if (line.begins_with("var ")) {
			const String name = identifier_at(line, skip_spaces(line, 4));
			if (!name.is_empty()) {
				symbols.variables.push_back({ name, i + 1 });
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

// What the caret is sitting on, derived from the code the editor hands us with
// COMPLETION_MARKER inserted at the caret position.
struct CompletionContext {
	String prefix; // Partial identifier being typed.
	String receiver; // Identifier in front of '.', empty when there is none.
	bool member = false; // Completing after a '.'.
	bool annotation = false; // Completing after a '@'.
	bool func_definition = false; // Completing the name in a 'func ' line.
	bool valid = true; // False inside a comment or a string literal.
};

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

// All five keys are mandatory: Godot skips options that are missing any of them.
void add_option(Array &r_options, int p_kind, const String &p_display, const String &p_insert, const Color &p_color, int p_location = ScriptLanguageExtension::LOCATION_OTHER) {
	Dictionary option;
	option["kind"] = p_kind;
	option["display"] = p_display;
	option["insert_text"] = p_insert;
	option["font_color"] = p_color;
	option["icon"] = Variant();
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

const char *const global_constants[] = { "PI", "TAU", "INF", "NAN", nullptr };

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

// The class free-standing calls end up on: every unqualified call in a
// SafeGDScript compiles to self.<name>() on the node the script is attached to.
StringName owner_class(Object *p_owner) {
	if (p_owner != nullptr) {
		return p_owner->get_class();
	}
	return StringName("Sandbox");
}

} // namespace

void SafeGDScriptLanguage::init() {
	safegdscript_language = memnew(SafeGDScriptLanguage);
	Engine::get_singleton()->register_script_language(safegdscript_language);
}
void SafeGDScriptLanguage::deinit() {
	if (safegdscript_language) {
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
		"break", "continue", "elif", "else", "if", "for", "match", "pass", "return", "when", "while"
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
	// The real parser runs inside the compiler sandbox and is only reached on
	// save, so nothing is reported here beyond the function list the editor
	// uses for the members overview.
	result["valid"] = true;
	if (p_validate_functions) {
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

	Array options;
	const CompletionContext ctx = analyze_completion(p_code);
	if (!ctx.valid) {
		result["options"] = options;
		return result;
	}
	const CompletionColors colors = completion_colors();

	if (ctx.annotation) {
		// @export is the only annotation the compiler understands (parser.cpp).
		add_option(options, CODE_COMPLETION_KIND_PLAIN_TEXT, "export", "export", colors.keyword);
		result["force"] = true;
	} else if (ctx.member) {
		if (ctx.receiver == "self" || ctx.receiver.is_empty()) {
			add_class_members(options, owner_class(p_owner), colors);
		} else {
			// Any other receiver is a Variant of a type only the compiler knows,
			// and member calls become vcalls, so offer the common Variant members.
			for (const char *const *name = variant_properties; *name != nullptr; name++) {
				add_option(options, CODE_COMPLETION_KIND_MEMBER, *name, *name, colors.member);
			}
			for (const char *const *name = variant_methods; *name != nullptr; name++) {
				add_option(options, CODE_COMPLETION_KIND_FUNCTION, String(*name) + String("("), String(*name) + String("("), colors.function);
			}
		}
		result["force"] = true;
	} else if (ctx.func_definition) {
		add_virtual_methods(options, owner_class(p_owner), colors);
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
		for (const char *const *name = global_constants; *name != nullptr; name++) {
			add_option(options, CODE_COMPLETION_KIND_CONSTANT, *name, *name, colors.text);
		}

		// Symbols declared by the script itself. The caret marker is dropped so
		// the word being typed is not scanned as a declaration.
		const SourceSymbols symbols = scan_source(p_code.replace(String::chr(COMPLETION_MARKER), String()));
		for (const SourceSymbol &function : symbols.functions) {
			add_option(options, CODE_COMPLETION_KIND_FUNCTION, function.name + String("("), function.name + String("("), colors.function, LOCATION_LOCAL);
		}
		for (const SourceSymbol &variable : symbols.variables) {
			add_option(options, CODE_COMPLETION_KIND_VARIABLE, variable.name, variable.name, colors.member, LOCATION_LOCAL);
		}
		for (const SourceSymbol &constant : symbols.constants) {
			add_option(options, CODE_COMPLETION_KIND_CONSTANT, constant.name, constant.name, colors.member, LOCATION_LOCAL);
		}

		// Unqualified calls compile to self.<name>(), so the base class members
		// are reachable without writing self.
		add_class_members(options, owner_class(p_owner), colors);
	}

	result["options"] = options;
	return result;
}
Dictionary SafeGDScriptLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	return Dictionary();
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
String SafeGDScriptLanguage::_debug_get_error() const {
	return String();
}
int32_t SafeGDScriptLanguage::_debug_get_stack_level_count() const {
	return 0;
}
int32_t SafeGDScriptLanguage::_debug_get_stack_level_line(int32_t p_level) const {
	return 0;
}
String SafeGDScriptLanguage::_debug_get_stack_level_function(int32_t p_level) const {
	return String();
}
Dictionary SafeGDScriptLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
Dictionary SafeGDScriptLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
void *SafeGDScriptLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	return nullptr;
}
Dictionary SafeGDScriptLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}
String SafeGDScriptLanguage::_debug_parse_stack_level_expression(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	return String();
}
TypedArray<Dictionary> SafeGDScriptLanguage::_debug_get_current_stack_info() {
	return TypedArray<Dictionary>();
}
void SafeGDScriptLanguage::_reload_all_scripts() {}
void SafeGDScriptLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {}
PackedStringArray SafeGDScriptLanguage::_get_recognized_extensions() const {
	PackedStringArray array;
	array.push_back("sgd");
	return array;
}
TypedArray<Dictionary> SafeGDScriptLanguage::_get_public_functions() const {
	return TypedArray<Dictionary>();
}
Dictionary SafeGDScriptLanguage::_get_public_constants() const {
	return Dictionary();
}
TypedArray<Dictionary> SafeGDScriptLanguage::_get_public_annotations() const {
	return TypedArray<Dictionary>();
}
void SafeGDScriptLanguage::_profiling_start() {}
void SafeGDScriptLanguage::_profiling_stop() {}
int32_t SafeGDScriptLanguage::_profiling_get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
int32_t SafeGDScriptLanguage::_profiling_get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}
void SafeGDScriptLanguage::_frame() {
	static bool icon_registered = register_language_icons;
	if (!icon_registered && Engine::get_singleton()->is_editor_hint()) {
		icon_registered = true;
		load_icon();
		EditorInterface::get_singleton()->get_base_control()->connect("theme_changed", callable_mp(this, &SafeGDScriptLanguage::load_icon));
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
	if (!p_path.is_empty()) {
		dict["name"] = SafeGDScript::PathToGlobalName(p_path);
		dict["base_type"] = "Sandbox";
		dict["icon_path"] = String(icon_path);
	}
	return dict;
}
