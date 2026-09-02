#include "script_language_safegdscript.h"
#include "compiler_backend.h"
#include "builtin_api_safegdscript.h"
#include "editor_analysis_safegdscript.h"
#include "../script_language_common.h"
#include "script_safegdscript.h"
#include "../sandbox.h"
#include "../sandbox_project_settings.h"

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
#include <cstdint>
#include <iterator>
#include <list>
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
bool global_constant_is_float(size_t index);
int64_t global_constant_int_value(size_t index);
double global_constant_float_value(size_t index);
size_t global_function_count();
const char *global_function_name(size_t index);
uint8_t global_function_min_args(size_t index);
uint8_t global_function_max_args(size_t index);
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
	ClassDB::bind_method(D_METHOD("editor_make_template", "template", "class_name", "base_class_name"),
			&SafeGDScriptLanguage::editor_make_template);
	ClassDB::bind_method(D_METHOD("editor_built_in_templates", "base"),
			&SafeGDScriptLanguage::editor_built_in_templates);
	ClassDB::bind_method(D_METHOD("editor_public_functions"),
			&SafeGDScriptLanguage::editor_public_functions);
	ClassDB::bind_method(D_METHOD("editor_public_constants"),
			&SafeGDScriptLanguage::editor_public_constants);
	ClassDB::bind_method(D_METHOD("editor_public_annotations"),
			&SafeGDScriptLanguage::editor_public_annotations);
	ClassDB::bind_method(D_METHOD("bake_all_translations"),
			&SafeGDScriptLanguage::bake_all_translations);
	ClassDB::bind_static_method("SafeGDScriptLanguage", D_METHOD("converted_script_path", "path"),
			&SafeGDScriptLanguage::converted_script_path);
	ClassDB::bind_static_method("SafeGDScriptLanguage", D_METHOD("convert_script_path", "path"),
			&SafeGDScriptLanguage::convert_script_path);
	ClassDB::bind_static_method("SafeGDScriptLanguage", D_METHOD("editor_convert_scripts", "paths", "to_safe"),
			&SafeGDScriptLanguage::editor_convert_scripts);
}

Dictionary SafeGDScriptLanguage::editor_validate(const String &p_script, const String &p_path,
		bool p_functions, bool p_errors, bool p_warnings, bool p_safe_lines) const {
	return _validate(p_script, p_path, p_functions, p_errors, p_warnings, p_safe_lines);
}

Dictionary SafeGDScriptLanguage::editor_complete(const String &p_code, const String &p_path,
		Object *p_owner) const {
	return _complete_code(p_code, p_path, p_owner);
}

Dictionary SafeGDScriptLanguage::editor_lookup(const String &p_code, const String &p_symbol,
		const String &p_path, Object *p_owner) const {
	return _lookup_code(p_code, p_symbol, p_path, p_owner);
}

Ref<Script> SafeGDScriptLanguage::editor_make_template(const String &p_template,
		const String &p_class_name, const String &p_base_class_name) const {
	return _make_template(p_template, p_class_name, p_base_class_name);
}

TypedArray<Dictionary> SafeGDScriptLanguage::editor_built_in_templates(const StringName &p_object) const {
	return _get_built_in_templates(p_object);
}

TypedArray<Dictionary> SafeGDScriptLanguage::editor_public_functions() const {
	return _get_public_functions();
}

Dictionary SafeGDScriptLanguage::editor_public_constants() const {
	return _get_public_constants();
}

TypedArray<Dictionary> SafeGDScriptLanguage::editor_public_annotations() const {
	return _get_public_annotations();
}

namespace {

constexpr char32_t COMPLETION_MARKER = 0xFFFF;

bool is_identifier_char(char32_t c) {
	return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

void add_own_of_kind(Array &r_options, const gdscript::SourceModel &p_model,
		gdscript::DeclarationKind p_kind, int p_completion_kind, const String &p_suffix = String());

const gdscript::SourceDeclaration *find_own_declaration(const gdscript::SourceModel &p_model,
		const String &p_name) {
	for (const gdscript::SourceDeclaration &declaration : p_model.declarations) {
		if (declaration.parent >= 0) continue;
		if (String::utf8(declaration.name.c_str(), declaration.name.size()) == p_name) {
			return &declaration;
		}
	}
	return nullptr;
}

String model_base_class(const gdscript::SourceModel &p_model) {
	for (const gdscript::SourceDeclaration &declaration : p_model.declarations) {
		if (declaration.kind == gdscript::DeclarationKind::CLASS && declaration.parent < 0) {
			return String::utf8(declaration.base_type.c_str(), declaration.base_type.size());
		}
	}
	return String();
}

// What the caret is sitting on, derived from the code the editor hands us with
// COMPLETION_MARKER inserted at the caret position.
struct CompletionContext {
	String receiver; // Identifier in front of '.', empty when there is none.
	bool member = false; // Completing after a '.'.
	bool annotation = false; // Completing after a '@'.
	bool func_definition = false; // Completing the name in a 'func ' line.
	bool type_position = false; // After ':', '->', 'is', 'as', or 'extends'.
	char32_t scene_sugar = 0; // '$' or '%' immediately before the path prefix.
	String string_call; // Recognized call whose string argument owns the caret.
	bool valid = true; // False inside a comment or a string literal.
};

void caret_position(const String &p_code, int32_t &r_line, int32_t &r_column) {
	const int marker_at = p_code.find(String::chr(COMPLETION_MARKER));
	const int caret = marker_at < 0 ? p_code.length() : marker_at;
	const String head = p_code.substr(0, caret);
	r_line = int32_t(head.count("\n") + 1);
	r_column = int32_t(caret - (head.rfind("\n") + 1));
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
		int at = quote_start - 1;
		while (at >= line_start && (p_code[at] == ' ' || p_code[at] == '\t')) at--;
		if (at >= line_start && p_code[at] == '(') {
			int end = at--;
			while (at >= line_start && is_identifier_char(p_code[at])) at--;
			ctx.string_call = p_code.substr(at + 1, end - at - 1);
		}
		ctx.valid = !ctx.string_call.is_empty();
		return ctx;
	}

	int start = caret;
	while (start > line_start && is_identifier_char(p_code[start - 1])) {
		start--;
	}
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

// complete_code() drops options with missing keys.
constexpr int MAX_COMPLETION_OPTIONS = 4096;

void add_option(Array &r_options, int p_kind, const String &p_display, const String &p_insert, int p_location = ScriptLanguageExtension::LOCATION_OTHER) {
	if (r_options.size() >= MAX_COMPLETION_OPTIONS) {
		return;
	}
	Dictionary option;
	option["kind"] = p_kind;
	option["display"] = p_display;
	option["insert_text"] = p_insert;
	option["font_color"] = Color();
	option["icon"] = Variant();
	option["default_value"] = Variant();
	option["location"] = p_location;
	r_options.push_back(option);
}

void add_resource_paths(Array &r_options, const String &p_directory, int p_depth = 0) {
	if (p_depth > 16 || r_options.size() >= MAX_COMPLETION_OPTIONS) return;
	for (const String &file : DirAccess::get_files_at(p_directory)) {
		if (file.ends_with(".import") || file.begins_with(".")) continue;
		const String path = p_directory.path_join(file);
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FILE_PATH, path, path);
	}
	for (const String &directory : DirAccess::get_directories_at(p_directory)) {
		if (directory.begins_with(".")) continue;
		add_resource_paths(r_options, p_directory.path_join(directory), p_depth + 1);
	}
}

void add_node_paths(Array &r_options, Node *p_root, Node *p_node, int p_depth = 0) {
	if (p_root == nullptr || p_node == nullptr || p_depth > 64 ||
			r_options.size() >= MAX_COMPLETION_OPTIONS) return;
	if (p_node != p_root) {
		const String path = String(p_root->get_path_to(p_node));
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_NODE_PATH, path, path);
		if (p_node->is_unique_name_in_owner()) {
			const String unique = "%" + String(p_node->get_name());
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_NODE_PATH, unique, unique);
		}
	}
	const TypedArray<Node> children = p_node->get_children();
	for (int64_t i = 0; i < children.size(); i++) {
		add_node_paths(r_options, p_root, Object::cast_to<Node>(children[i]), p_depth + 1);
	}
}

void add_scene_sugar_options(Array &r_options, Node *p_root, Node *p_node,
		char32_t p_sugar, int p_depth = 0) {
	if (p_root == nullptr || p_node == nullptr || p_depth > 64 ||
			r_options.size() >= MAX_COMPLETION_OPTIONS) return;
	if (p_node != p_root) {
		if (p_sugar == '$') {
			const String path = String(p_root->get_path_to(p_node));
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_NODE_PATH,
					String("$") + path, path);
		} else if (p_sugar == '%' && p_node->is_unique_name_in_owner()) {
			const String name = String(p_node->get_name());
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_NODE_PATH,
					String("%") + name, name);
		}
	}
	const TypedArray<Node> children = p_node->get_children();
	for (int64_t i = 0; i < children.size(); i++) {
		add_scene_sugar_options(r_options, p_root, Object::cast_to<Node>(children[i]),
				p_sugar, p_depth + 1);
	}
}

void add_string_context_options(Array &r_options, const CompletionContext &p_ctx,
		const gdscript::SourceModel &p_model, const StringName &p_owner_class, Object *p_owner) {
	const String call = p_ctx.string_call;
	if (call == "preload" || call == "load" || call == "icon") {
		add_resource_paths(r_options, "res://");
		return;
	}
	if (call == "export_file" || call == "export_global_file") {
		static const char *filters[] = { "*.tscn", "*.tres", "*.gd", "*.sgd", "*.png",
			"*.svg", "*.wav", "*.ogg", "*.json", "*.*", nullptr };
		for (const char **filter = filters; *filter != nullptr; filter++) {
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_PLAIN_TEXT,
					*filter, *filter);
		}
		return;
	}
	if (call == "rpc") {
		static const char *rpc_options[] = { "authority", "any_peer", "call_local", "call_remote",
			"reliable", "unreliable", "unreliable_ordered", nullptr };
		for (const char **option = rpc_options; *option != nullptr; option++) {
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
					*option, *option);
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
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
					*code, *code);
		}
		return;
	}
	if (call == "export_node_path") {
		if (ClassDBSingleton *class_db = ClassDBSingleton::get_singleton()) {
			for (const String &name : class_db->get_class_list()) {
				if (class_db->is_parent_class(name, "Node")) {
					add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS,
							name, name);
				}
			}
		}
		return;
	}
	if (call == "get_node" || call == "has_node") {
		Node *owner = Object::cast_to<Node>(p_owner);
		add_node_paths(r_options, owner, owner);
		return;
	}
	if (call.begins_with("is_action_") || call == "get_action_strength" ||
			call == "get_axis" || call == "get_vector") {
		if (InputMap::get_singleton() != nullptr) {
			for (const StringName &action : InputMap::get_singleton()->get_actions()) {
				const String name(action);
				add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
						name, name);
			}
		}
		return;
	}
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db == nullptr || !class_db->class_exists(p_owner_class)) return;
	if (call == "connect" || call == "emit_signal") {
		add_own_of_kind(r_options, p_model, gdscript::DeclarationKind::SIGNAL,
				ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL);
		for (const Dictionary &signal : class_db->class_get_signal_list(p_owner_class, false)) {
			const String name = signal["name"];
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL, name, name);
		}
		return;
	}
	if (call == "call" || call == "has_method") {
		add_own_of_kind(r_options, p_model, gdscript::DeclarationKind::FUNCTION,
				ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION);
		for (const Dictionary &method : class_db->class_get_method_list(p_owner_class, false)) {
			const String name = method["name"];
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION, name, name);
		}
		return;
	}
	if (call == "get" || call == "set") {
		add_own_of_kind(r_options, p_model, gdscript::DeclarationKind::VARIABLE,
				ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER);
		for (const Dictionary &property : class_db->class_get_property_list(p_owner_class, false)) {
			const String name = property["name"];
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER, name, name);
		}
	}
}

const char *const annotation_names[] = {
	"export", "export_range", "export_enum", "export_exp_easing", "export_flags",
	"export_flags_2d_render", "export_flags_2d_physics", "export_flags_2d_navigation",
	"export_flags_3d_render", "export_flags_3d_physics", "export_flags_3d_navigation",
	"export_flags_avoidance", "export_file", "export_dir", "export_global_file",
	"export_global_dir", "export_multiline", "export_placeholder", "export_color_no_alpha",
	"export_node_path", "export_storage", "export_custom", "export_group", "export_subgroup",
	"export_category", "onready", "tool", "rpc", "icon", "warning_ignore",
	"warning_ignore_start", "warning_ignore_restore", "static_unload", "abstract",
	nullptr
};

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
void add_global_constants(Array &r_options) {
	for (size_t i = 0; i < gdscript::global_constant_count(); i++) {
		const char *name = gdscript::global_constant_name(i);
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
				name, name);
	}
}

// Members of a @GlobalScope enum (Side.SIDE_LEFT). Same shape as the built-in
// type constants above: false when the receiver names no such enum, so the
// caller can go on looking. Full scan; rows are grouped but not indexed.
bool add_global_enum_members(Array &r_options, const String &p_enum) {
	bool found = false;
	for (size_t i = 0; i < gdscript::global_enum_value_count(); i++) {
		if (p_enum != gdscript::global_enum_value_enum(i)) {
			continue;
		}
		const char *name = gdscript::global_enum_value_name(i);
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
				name, name);
		found = true;
	}
	return found;
}

// The enum names themselves, offered where a type or a qualifier can go.
void add_global_enum_names(Array &r_options) {
	String previous;
	for (size_t i = 0; i < gdscript::global_enum_value_count(); i++) {
		const String name = gdscript::global_enum_value_enum(i);
		if (name == previous) {
			continue; // Rows are grouped by enum; one option per group.
		}
		previous = name;
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_ENUM,
				name, name);
	}
}

void add_global_functions(Array &r_options) {
	for (size_t i = 0; i < gdscript::global_function_count(); i++) {
		const char *name = gdscript::global_function_name(i);
		if (name == nullptr) {
			continue;
		}
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION,
				String(name) + String("("), String(name) + String("("));
	}
}

// Virtuals only after 'self.' where overriding makes sense.
void add_class_members(Array &r_options, const StringName &p_class, bool p_meta = false,
		int p_location = ScriptLanguageExtension::LOCATION_OTHER, bool p_virtuals = false) {
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db == nullptr || !class_db->class_exists(p_class)) {
		return;
	}
	const TypedArray<Dictionary> methods = class_db->class_get_method_list(p_class, false);
	for (int i = 0; i < methods.size(); i++) {
		const Dictionary method = methods[i];
		const String name = method["name"];
		if (name.is_empty()) {
			continue;
		}
		if (p_meta && (int(method["flags"]) & METHOD_FLAG_STATIC) == 0) {
			continue;
		}
		if (name.begins_with("_") && !p_virtuals) {
			continue;
		}
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION,
				name + String("("), name + String("("), p_location);
	}
	if (!p_meta) {
		const TypedArray<Dictionary> properties = class_db->class_get_property_list(p_class, false);
		for (int i = 0; i < properties.size(); i++) {
			const Dictionary property = properties[i];
			const String name = property["name"];
			// Groups and categories have no type.
			if (name.is_empty() || int(property["type"]) == Variant::NIL) {
				continue;
			}
			if (name.contains("/") || name.contains(" ")) {
				continue;
			}
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER, name, name,
					p_location);
		}
		for (const Dictionary &signal : class_db->class_get_signal_list(p_class, false)) {
			const String name = signal["name"];
			if (!name.is_empty()) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL, name, name, p_location);
		}
	}
	for (const String &name : class_db->class_get_integer_constant_list(p_class, false)) {
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT, name, name,
				p_location);
	}
	for (const String &name : class_db->class_get_enum_list(p_class, false)) {
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_ENUM, name, name,
				p_location);
	}
}

void add_builtin_members(Array &r_options, const String &p_type, bool p_meta,
		int p_location = ScriptLanguageExtension::LOCATION_OTHER) {
	const safegd_builtin::BuiltinClassInfo *info = safegd_builtin::find_builtin_class(p_type);
	if (info == nullptr) {
		return;
	}
	for (const safegd_builtin::BuiltinMethod *method = info->methods; method->name != nullptr; method++) {
		if (method->is_static != p_meta) {
			continue;
		}
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION,
				String(method->name) + String("("), String(method->name) + String("("), p_location);
	}
	if (p_meta) {
		for (const safegd_builtin::BuiltinMember *constant = info->constants; constant->name != nullptr; constant++) {
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
					constant->name, constant->name, p_location);
		}
		return;
	}
	for (const safegd_builtin::BuiltinMember *member = info->members; member->name != nullptr; member++) {
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER,
				member->name, member->name, p_location);
	}
}

void add_script_members(Array &r_options, const Ref<Script> &p_script, bool p_meta, int p_location) {
	Ref<Script> script = p_script;
	if (script.is_null()) {
		return;
	}
	for (Ref<Script> at = script; at.is_valid(); at = at->get_base_script()) {
		if (!p_meta) {
			for (const Dictionary &method : at->get_script_method_list()) {
				const String name = method.get("name", String());
				if (!name.is_empty()) add_option(r_options,
						ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION,
						name + String("("), name + String("("), p_location);
			}
			for (const Dictionary &property : at->get_script_property_list()) {
				const String name = property.get("name", String());
				if (!name.is_empty()) add_option(r_options,
						ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER, name, name, p_location);
			}
			for (const Dictionary &signal : at->get_script_signal_list()) {
				const String name = signal.get("name", String());
				if (!name.is_empty()) add_option(r_options,
						ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL, name, name, p_location);
			}
		}
		for (const Variant &key : at->get_script_constant_map().keys()) {
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
					String(key), String(key), p_location);
		}
	}
	if (p_meta) {
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION, "new(", "new(",
				p_location);
	}
	add_class_members(r_options, script->get_instance_base_type(), p_meta, p_location);
}

void add_declaration_members(Array &r_options, const gdscript::SourceModel &p_model,
		int32_t p_index, bool p_meta) {
	if (p_index < 0 || size_t(p_index) >= p_model.declarations.size()) {
		return;
	}
	const gdscript::SourceDeclaration &declaration = p_model.declarations[size_t(p_index)];
	if (declaration.kind == gdscript::DeclarationKind::ENUM) {
		for (const gdscript::SourceEnumMember &member : declaration.enum_members) {
			const String name = String::utf8(member.name.c_str(), member.name.size());
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT, name, name,
					ScriptLanguageExtension::LOCATION_OTHER_USER_CODE);
		}
		return;
	}
	if (p_meta) {
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION, "new(", "new(",
				ScriptLanguageExtension::LOCATION_OTHER_USER_CODE);
	}
	for (int32_t child : declaration.children) {
		if (child < 0 || size_t(child) >= p_model.declarations.size()) continue;
		const gdscript::SourceDeclaration &member = p_model.declarations[size_t(child)];
		const String name = String::utf8(member.name.c_str(), member.name.size());
		const bool is_static = (member.flags & 1u) != 0;
		switch (member.kind) {
			case gdscript::DeclarationKind::FUNCTION:
				if (p_meta && !is_static) break;
				add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION,
						name + String("("), name + String("("),
						ScriptLanguageExtension::LOCATION_OTHER_USER_CODE);
				break;
			case gdscript::DeclarationKind::VARIABLE:
				if (p_meta) break;
				add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER, name, name,
						ScriptLanguageExtension::LOCATION_OTHER_USER_CODE);
				break;
			case gdscript::DeclarationKind::CONSTANT:
				add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT, name, name,
						ScriptLanguageExtension::LOCATION_OTHER_USER_CODE);
				break;
			case gdscript::DeclarationKind::SIGNAL:
				if (p_meta) break;
				add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL, name, name,
						ScriptLanguageExtension::LOCATION_OTHER_USER_CODE);
				break;
			default:
				break;
		}
	}
}

bool add_type_ref_members(Array &r_options, const EditorTypeRef &p_type,
		const gdscript::SourceModel &p_model,
		int p_location = ScriptLanguageExtension::LOCATION_OTHER) {
	switch (p_type.kind) {
		case EditorTypeRef::ENGINE_CLASS:
			add_class_members(r_options, p_type.name, p_type.meta, p_location);
			return true;
		case EditorTypeRef::BUILTIN:
			add_builtin_members(r_options, p_type.name, p_type.meta, p_location);
			return true;
		case EditorTypeRef::SCRIPT:
			add_script_members(r_options, p_type.script, p_type.meta, p_location);
			return true;
		case EditorTypeRef::STRUCT:
		case EditorTypeRef::NESTED_CLASS:
		case EditorTypeRef::TRAIT:
		case EditorTypeRef::SCRIPT_ENUM:
			add_declaration_members(r_options, p_model, p_type.declaration, p_type.meta);
			return true;
		default:
			return false;
	}
}

// The script's own functions, offered wherever a call on the script itself is
// being written: after 'self.' as well as unqualified.
void add_script_functions(Array &r_options, const gdscript::SourceModel &p_model) {
	add_own_of_kind(r_options, p_model, gdscript::DeclarationKind::FUNCTION,
			ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION, "(");
}

void add_own_of_kind(Array &r_options, const gdscript::SourceModel &p_model,
		gdscript::DeclarationKind p_kind, int p_completion_kind, const String &p_suffix) {
	for (const gdscript::SourceDeclaration &declaration : p_model.declarations) {
		if (declaration.parent >= 0 || declaration.kind != p_kind) continue;
		const String name = String::utf8(declaration.name.c_str(), declaration.name.size());
		if (name.is_empty()) continue;
		add_option(r_options, p_completion_kind, name + p_suffix, name + p_suffix,
				ScriptLanguageExtension::LOCATION_OTHER_USER_CODE);
	}
}

void add_own_declarations(Array &r_options, const gdscript::SourceModel &p_model) {
	for (const gdscript::SourceDeclaration &declaration : p_model.declarations) {
		if (declaration.parent >= 0) continue;
		const String name = String::utf8(declaration.name.c_str(), declaration.name.size());
		if (name.is_empty()) continue;
		switch (declaration.kind) {
			case gdscript::DeclarationKind::SIGNAL:
				add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL, name, name,
						ScriptLanguageExtension::LOCATION_OTHER_USER_CODE);
				break;
			case gdscript::DeclarationKind::ENUM:
			case gdscript::DeclarationKind::NESTED_CLASS:
			case gdscript::DeclarationKind::TRAIT:
			case gdscript::DeclarationKind::STRUCT:
				add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS, name, name,
						ScriptLanguageExtension::LOCATION_OTHER_USER_CODE);
				break;
			default:
				break;
		}
	}
}

void add_own_members(Array &r_options, const gdscript::SourceModel &p_model) {
	for (const gdscript::SourceDeclaration &declaration : p_model.declarations) {
		if (declaration.parent >= 0) continue;
		const String name = String::utf8(declaration.name.c_str(), declaration.name.size());
		if (name.is_empty()) continue;
		if (declaration.kind == gdscript::DeclarationKind::VARIABLE) {
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER, name, name,
					ScriptLanguageExtension::LOCATION_OTHER_USER_CODE);
		} else if (declaration.kind == gdscript::DeclarationKind::CONSTANT) {
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT, name, name,
					ScriptLanguageExtension::LOCATION_OTHER_USER_CODE);
		}
	}
}

void add_super_members(Array &r_options, const String &p_base, Object *p_owner) {
	const int location = ScriptLanguageExtension::LOCATION_PARENT_MASK + 1;
	Ref<Script> base;
	if (p_base.begins_with("res://") || p_base.begins_with("user://")) {
		base = Ref<Script>(ResourceLoader::get_singleton()->load(p_base));
	} else {
		base = EditorSymbolResolver::script_for_global_class(p_base);
	}
	if (base.is_valid()) {
		add_script_members(r_options, base, false, location);
		return;
	}
	if (!p_base.is_empty()) {
		add_class_members(r_options, p_base, false, location);
	} else if (p_owner != nullptr) {
		add_class_members(r_options, p_owner->get_class(), false, location);
	}
}

void add_any_builtin_members(Array &r_options) {
	std::unordered_set<std::string> seen;
	for (const char *const *type = builtin_types; *type != nullptr; type++) {
		const safegd_builtin::BuiltinClassInfo *info = safegd_builtin::find_builtin_class(*type);
		if (info == nullptr) continue;
		for (const safegd_builtin::BuiltinMember *member = info->members; member->name != nullptr; member++) {
			if (seen.insert(member->name).second) add_option(r_options,
					ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER, member->name, member->name);
		}
		for (const safegd_builtin::BuiltinMethod *method = info->methods; method->name != nullptr; method++) {
			if (method->is_static || !seen.insert(method->name).second) continue;
			add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION,
					String(method->name) + String("("), String(method->name) + String("("));
		}
	}
}

// -= Call hints =-

// A call's signature with the argument the caret is on wrapped in the marker
// character, which is how the editor knows which one to highlight. It is the
// same character it uses to tell us where the caret is.
String argument_hint(const String &p_return_type, const String &p_name,
		const PackedStringArray &p_arguments, int p_current) {
	String hint = (p_return_type.is_empty() ? String("void") : p_return_type) + String(" ") +
			p_name + String("(");
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

String literal_type(const String &p_text) {
	const String text = p_text.strip_edges();
	if (text.is_empty()) {
		return String();
	}
	if (text == "true" || text == "false") return "bool";
	if (text[0] == '"' || text[0] == '\'') return "String";
	if (text.is_valid_int()) return "int";
	if (text.is_valid_float()) return "float";
	const int open = text.find("(");
	if (open > 0 && text.ends_with(")") &&
			safegd_builtin::find_builtin_class(text.substr(0, open)) != nullptr) {
		return text.substr(0, open);
	}
	return String();
}

PackedStringArray declaration_arguments(const gdscript::SourceDeclaration &p_declaration) {
	PackedStringArray arguments;
	for (const gdscript::SourceParameter &parameter : p_declaration.parameters) {
		String text = String::utf8(parameter.name.c_str(), parameter.name.size());
		String type = String::utf8(parameter.declared_type.c_str(), parameter.declared_type.size());
		const String default_text = String::utf8(parameter.default_text.c_str(),
				parameter.default_text.size());
		if (type.is_empty()) type = literal_type(default_text);
		if (!type.is_empty()) text += String(": ") + type;
		if (!default_text.is_empty()) text += String(" = ") + default_text;
		arguments.push_back(text);
	}
	return arguments;
}

String info_type_name(const Dictionary &p_info) {
	const String class_name = p_info.get("class_name", StringName());
	if (!class_name.is_empty()) return class_name;
	const int type = int(p_info.get("type", int(Variant::NIL)));
	if (type != int(Variant::NIL)) return Variant::get_type_name(Variant::Type(type));
	if ((int(p_info.get("usage", 0)) & PROPERTY_USAGE_NIL_IS_VARIANT) != 0) return "Variant";
	return String();
}

// Defaults cover trailing arguments (engine convention).
PackedStringArray method_info_arguments(const Dictionary &p_method) {
	PackedStringArray arguments;
	const Array declared = p_method.get("args", Array());
	const Array defaults = p_method.get("default_args", Array());
	const int first_default = int(declared.size() - defaults.size());
	for (int i = 0; i < declared.size(); i++) {
		const Dictionary argument = declared[i];
		String text = argument.get("name", String());
		const String type = info_type_name(argument);
		if (!type.is_empty()) text += String(": ") + type;
		if (first_default >= 0 && i >= first_default) {
			text += String(" = ") + Variant(defaults[i - first_default]).stringify();
		}
		arguments.push_back(text);
	}
	if ((int(p_method.get("flags", 0)) & METHOD_FLAG_VARARG) != 0) {
		arguments.push_back("...");
	}
	return arguments;
}

String engine_method_hint(const StringName &p_class, const String &p_method, int p_argument) {
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db == nullptr || !class_db->class_exists(p_class) ||
			!class_db->class_has_method(p_class, p_method, false)) {
		return String();
	}
	for (const Dictionary &method : class_db->class_get_method_list(p_class, false)) {
		if (String(method["name"]) != p_method) continue;
		return argument_hint(info_type_name(method.get("return", Dictionary())), p_method,
				method_info_arguments(method), p_argument);
	}
	return String();
}

String script_method_hint(const Ref<Script> &p_script, const String &p_method, int p_argument) {
	for (Ref<Script> at = p_script; at.is_valid(); at = at->get_base_script()) {
		for (const Dictionary &method : at->get_script_method_list()) {
			if (String(method.get("name", String())) != p_method) continue;
			return argument_hint(info_type_name(method.get("return", Dictionary())), p_method,
					method_info_arguments(method), p_argument);
		}
	}
	if (p_script.is_valid()) {
		return engine_method_hint(p_script->get_instance_base_type(), p_method, p_argument);
	}
	return String();
}

String builtin_constructor_hint(const String &p_type, int p_argument) {
	const safegd_builtin::BuiltinClassInfo *info = safegd_builtin::find_builtin_class(p_type);
	if (info == nullptr) {
		return String();
	}
	String hint;
	for (const char *const *constructor = info->constructors; *constructor != nullptr; constructor++) {
		const PackedStringArray arguments = safegd_builtin::split_arguments(*constructor);
		if (arguments.size() <= p_argument) continue;
		if (!hint.is_empty()) hint += "\n";
		hint += argument_hint(p_type, p_type, arguments, p_argument);
	}
	return hint;
}

String builtin_method_hint(const String &p_type, const String &p_method, int p_argument) {
	const safegd_builtin::BuiltinClassInfo *info = safegd_builtin::find_builtin_class(p_type);
	if (info == nullptr) {
		return String();
	}
	for (const safegd_builtin::BuiltinMethod *method = info->methods; method->name != nullptr; method++) {
		if (p_method != method->name) continue;
		return argument_hint(method->return_type, p_method,
				safegd_builtin::split_arguments(method->arguments), p_argument);
	}
	return String();
}

const gdscript::SourceDeclaration *find_declared_function(const gdscript::SourceModel &p_model,
		int32_t p_parent, const String &p_name) {
	for (const gdscript::SourceDeclaration &declaration : p_model.declarations) {
		if (declaration.parent != p_parent ||
				declaration.kind != gdscript::DeclarationKind::FUNCTION) continue;
		if (String::utf8(declaration.name.c_str(), declaration.name.size()) == p_name) {
			return &declaration;
		}
	}
	return nullptr;
}

String declared_function_hint(const gdscript::SourceDeclaration &p_function, const String &p_name,
		int p_argument) {
	return argument_hint(String::utf8(p_function.return_type.c_str(), p_function.return_type.size()),
			p_name, declaration_arguments(p_function), p_argument);
}

String struct_constructor_hint(const gdscript::SourceModel &p_model,
		const gdscript::SourceDeclaration &p_struct, int p_argument) {
	const String name = String::utf8(p_struct.name.c_str(), p_struct.name.size());
	PackedStringArray arguments;
	for (int32_t child : p_struct.children) {
		if (child < 0 || size_t(child) >= p_model.declarations.size()) continue;
		const gdscript::SourceDeclaration &field = p_model.declarations[size_t(child)];
		if (field.kind != gdscript::DeclarationKind::VARIABLE) continue;
		arguments.push_back(String::utf8(field.name.c_str(), field.name.size()));
	}
	return argument_hint(name, name, arguments, p_argument);
}

const gdscript::SourceDeclaration *find_own_struct(const gdscript::SourceModel &p_model,
		const String &p_name) {
	const gdscript::SourceDeclaration *found = find_own_declaration(p_model, p_name);
	return found != nullptr && found->kind == gdscript::DeclarationKind::STRUCT ? found : nullptr;
}

String call_hint_for(const String &p_callee, int p_argument, const gdscript::SourceModel &p_model,
		const EditorSymbolResolver &p_resolver,
		const StringName &p_owner_class, uint32_t p_line) {
	if (p_callee.is_empty()) {
		return String();
	}
	const PackedStringArray segments = EditorSymbolResolver::split_chain(p_callee);
	const String name = segments[segments.size() - 1].strip_edges();
	String receiver;
	for (int i = 0; i + 1 < segments.size(); i++) {
		if (i > 0) receiver += ".";
		receiver += segments[i];
	}

	if (receiver.is_empty() || receiver == "self") {
		if (const gdscript::SourceDeclaration *function = find_declared_function(p_model, -1, name)) {
			return declared_function_hint(*function, name, p_argument);
		}
		if (receiver.is_empty()) {
			if (const gdscript::SourceDeclaration *declaration = find_own_struct(p_model, name)) {
				return struct_constructor_hint(p_model, *declaration, p_argument);
			}
			if (safegd_builtin::find_builtin_class(name) != nullptr) {
				return builtin_constructor_hint(name, p_argument);
			}
			for (size_t i = 0; i < gdscript::global_function_count(); i++) {
				const char *entry = gdscript::global_function_name(i);
				if (entry == nullptr || name != entry) continue;
				PackedStringArray arguments;
				for (uint8_t at = 0; at < gdscript::global_function_min_args(i); at++) {
					arguments.push_back(String("arg") + itos(at));
				}
				if (gdscript::global_function_max_args(i) > gdscript::global_function_min_args(i)) {
					arguments.push_back("...");
				}
				return argument_hint("Variant", name, arguments, p_argument);
			}
		}
		return engine_method_hint(p_owner_class, name, p_argument);
	}

	if (segments.size() == 2 && name == "new") {
		if (const gdscript::SourceDeclaration *declaration = find_own_struct(p_model, receiver)) {
			return struct_constructor_hint(p_model, *declaration, p_argument);
		}
	}

	const EditorTypeRef type = p_resolver.resolve_receiver(receiver, p_line);
	switch (type.kind) {
		case EditorTypeRef::STRUCT:
			if (const gdscript::SourceDeclaration *declaration = find_own_struct(p_model, type.name)) {
				return struct_constructor_hint(p_model, *declaration, p_argument);
			}
			return String();
		case EditorTypeRef::ENGINE_CLASS:
			return engine_method_hint(type.name, name, p_argument);
		case EditorTypeRef::BUILTIN:
			return builtin_method_hint(type.name, name, p_argument);
		case EditorTypeRef::SCRIPT:
			return script_method_hint(type.script, type.meta && name == "new" ? String("_init") : name,
					p_argument);
		case EditorTypeRef::NESTED_CLASS:
		case EditorTypeRef::TRAIT: {
			const String wanted = type.meta && name == "new" ? String("_init") : name;
			if (const gdscript::SourceDeclaration *function =
					find_declared_function(p_model, type.declaration, wanted)) {
				return declared_function_hint(*function, name, p_argument);
			}
			return String();
		}
		default:
			return String();
	}
}


// Overridable methods of the base class, inserted as a complete signature.
void add_virtual_methods(Array &r_options, const StringName &p_class) {
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
		add_option(r_options, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION, signature, signature + String(":"));
	}
}

using ValidationResult = GDScriptCompilerBackend::Validation;

template <typename T>
class LruCache {
	size_t capacity;
	std::list<std::string> order;
	std::unordered_map<std::string, std::pair<T, std::list<std::string>::iterator>> entries;

public:
	explicit LruCache(size_t p_capacity) :
			capacity(p_capacity) {}

	const T *find(const std::string &p_key) {
		const auto it = entries.find(p_key);
		if (it == entries.end()) {
			return nullptr;
		}
		order.splice(order.begin(), order, it->second.second);
		return &it->second.first;
	}

	void insert(const std::string &p_key, T p_value) {
		if (const auto it = entries.find(p_key); it != entries.end()) {
			it->second.first = std::move(p_value);
			order.splice(order.begin(), order, it->second.second);
			return;
		}
		while (entries.size() >= capacity && !order.empty()) {
			entries.erase(order.back());
			order.pop_back();
		}
		order.push_front(p_key);
		entries.emplace(p_key, std::make_pair(std::move(p_value), order.begin()));
	}
};

std::string analysis_cache_key(const String &p_source, const String &p_path,
		const PackedStringArray &p_base_sources) {
	String context;
	for (const String &entry : p_base_sources) context += entry + String("\n");
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
	const CharString source_utf8 = p_source.utf8();
	const CharString context_utf8 = context.utf8();
	return std::string(p_path.utf8().get_data()) + "\n" +
			std::string(source_utf8.get_data(), size_t(source_utf8.length())) + "\nctx=" +
			std::string(context_utf8.get_data(), size_t(context_utf8.length())) + "\n" +
			gdscript_compiler::policy_name();
}

bool validate_with_compiler(const String &p_source, const String &p_path,
		ValidationResult &r_result) {
	const PackedStringArray base_sources = SafeGDScript::resolve_base_sources(p_source, p_path);
	const std::string key = analysis_cache_key(p_source, p_path, base_sources);
	static LruCache<ValidationResult> cache(16);
	if (const ValidationResult *cached = cache.find(key)) {
		r_result = *cached;
		return true;
	}

	GDScriptCompilerBackend &compiler = gdscript_compiler::backend_for(false);
	if (!compiler.available()) {
		return false;
	}
	// prepare() resets sticky inputs so validation inherits nothing from a prior compile.
	gdscript_compiler::prepare(compiler, false, base_sources, p_path);

	ValidationResult result;
	if (!compiler.validate(p_source, result)) {
		return false;
	}

	cache.insert(key, result);
	r_result = result;
	return true;
}

bool analyze_with_compiler(const String &p_source, const String &p_path, uint32_t p_flags,
		int32_t p_caret_line, int32_t p_caret_column, gdscript::SourceModel &r_model) {
	const String canonical_path = p_path.simplify_path();
	const PackedStringArray base_sources = SafeGDScript::resolve_base_sources(p_source, canonical_path);
	const bool wants_caret = (p_flags & gdscript::ANALYZE_CARET) != 0;
	const int32_t caret_line = wants_caret ? p_caret_line : 0;
	const int32_t caret_column = wants_caret ? p_caret_column : 0;
	const std::string key = analysis_cache_key(p_source, canonical_path, base_sources) + "\n" +
			std::to_string(p_flags) + ":" + std::to_string(caret_line) + ":" +
			std::to_string(caret_column);
	static LruCache<std::vector<uint8_t>> cache(64);
	if (const std::vector<uint8_t> *cached = cache.find(key)) {
		return gdscript::decode_source_model(cached->data(), cached->size(), r_model);
	}
	auto analyze_in_process = [&]() {
		const CharString source = p_source.utf8();
		const CharString path = canonical_path.utf8();
		r_model = gdscript::analyze_source(std::string(source.get_data(), source.length()),
				std::string(path.get_data(), path.length()), p_flags, caret_line, caret_column);
		return true;
	};
	GDScriptCompilerBackend &compiler = gdscript_compiler::backend_for(false);
	if (!compiler.available() || !compiler.can_analyze()) {
		return analyze_in_process();
	}
	gdscript_compiler::prepare(compiler, false,
			base_sources, canonical_path);
	GDScriptCompilerBackend::AnalysisRequest request;
	request.source = p_source;
	request.path = canonical_path;
	request.flags = p_flags;
	request.caret_line = caret_line;
	request.caret_column = caret_column;
	const PackedByteArray bytes = compiler.analyze(request);
	if (bytes.is_empty()) {
		return analyze_in_process();
	}
	std::string model_error;
	if (!gdscript::decode_source_model(bytes.ptr(), size_t(bytes.size()), r_model, model_error)) {
		ERR_PRINT("SafeGDScript: compiler analysis returned an invalid source model: " +
				String::utf8(model_error.c_str(), model_error.size()));
		return false;
	}
	cache.insert(key, std::vector<uint8_t>(bytes.ptr(), bytes.ptr() + bytes.size()));
	return true;
}

// The class free-standing calls end up on: every unqualified call in a
// SafeGDScript compiles to self.<name>() on the node the script is attached to.
StringName owner_class(Object *p_owner, const gdscript::SourceModel &p_model) {
	if (p_owner != nullptr) {
		return p_owner->get_class();
	}
	// With no instance to ask -- a script open in the editor but not attached
	// to anything -- the 'extends' line is what the script itself claims.
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	const String base = model_base_class(p_model);
	if (!base.is_empty() && class_db != nullptr && class_db->class_exists(base)) {
		return StringName(base);
	}
	return StringName("Sandbox");
}

Ref<Script> model_base_script(const gdscript::SourceModel &p_model) {
	const String base = model_base_class(p_model);
	if (base.is_empty()) {
		return Ref<Script>();
	}
	if (base.begins_with("res://") || base.begins_with("user://")) {
		return Ref<Script>(ResourceLoader::get_singleton()->load(base));
	}
	return EditorSymbolResolver::script_for_global_class(base);
}

// _get_global_name without a compiled script.
String editing_doc_class_name(const String &p_path, const gdscript::SourceModel &p_model) {
	for (const gdscript::SourceDeclaration &declaration : p_model.declarations) {
		if (declaration.kind == gdscript::DeclarationKind::CLASS && declaration.parent < 0 &&
				!declaration.name.empty()) {
			return String::utf8(declaration.name.c_str(), declaration.name.size());
		}
	}
	if (p_path.is_empty() || p_path.contains("::")) {
		return String();
	}
	return SafeGDScript::PathToGlobalName(p_path);
}

int lookup_kind_of(EditorMemberTarget::Kind p_kind) {
	switch (p_kind) {
		case EditorMemberTarget::CLASS: return ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
		case EditorMemberTarget::METHOD: return ScriptLanguageExtension::LOOKUP_RESULT_CLASS_METHOD;
		case EditorMemberTarget::PROPERTY: return ScriptLanguageExtension::LOOKUP_RESULT_CLASS_PROPERTY;
		case EditorMemberTarget::SIGNAL: return ScriptLanguageExtension::LOOKUP_RESULT_CLASS_SIGNAL;
		case EditorMemberTarget::CONSTANT: return ScriptLanguageExtension::LOOKUP_RESULT_CLASS_CONSTANT;
		case EditorMemberTarget::ENUM: return ScriptLanguageExtension::LOOKUP_RESULT_CLASS_ENUM;
		default: return -1;
	}
}

// Ctrl-click reads script_path/location, hover reads class_name.
bool fill_lookup(Dictionary &r_result, const EditorMemberTarget &p_target) {
	const int kind = lookup_kind_of(p_target.kind);
	if (kind < 0 || p_target.class_name.is_empty()) {
		return false;
	}
	r_result["result"] = Error::OK;
	r_result["type"] = kind;
	r_result["class_name"] = p_target.class_name;
	if (p_target.kind != EditorMemberTarget::CLASS) {
		r_result["class_member"] = p_target.class_member;
	}
	if (!p_target.script_path.is_empty()) {
		r_result["script_path"] = p_target.script_path;
	}
	if (p_target.line > 0) {
		r_result["location"] = p_target.line;
	}
	if (!p_target.doc_type.is_empty()) {
		r_result["doc_type"] = p_target.doc_type;
	}
	if (!p_target.description.is_empty()) {
		r_result["description"] = p_target.description;
	}
	return true;
}

EditorMemberTarget own_member_target(const gdscript::SourceDeclaration &p_declaration,
		const String &p_doc_class_name, const String &p_path) {
	EditorMemberTarget target;
	const String name = String::utf8(p_declaration.name.c_str(), p_declaration.name.size());
	switch (p_declaration.kind) {
		case gdscript::DeclarationKind::FUNCTION:
			target.kind = EditorMemberTarget::METHOD;
			target.doc_type = String::utf8(p_declaration.return_type.c_str(), p_declaration.return_type.size());
			break;
		case gdscript::DeclarationKind::VARIABLE:
			target.kind = EditorMemberTarget::PROPERTY;
			target.doc_type = String::utf8(p_declaration.declared_type.c_str(), p_declaration.declared_type.size());
			break;
		case gdscript::DeclarationKind::CONSTANT:
			target.kind = EditorMemberTarget::CONSTANT;
			break;
		case gdscript::DeclarationKind::SIGNAL:
			target.kind = EditorMemberTarget::SIGNAL;
			break;
		case gdscript::DeclarationKind::ENUM:
			target.kind = EditorMemberTarget::ENUM;
			break;
		case gdscript::DeclarationKind::NESTED_CLASS:
		case gdscript::DeclarationKind::STRUCT:
		case gdscript::DeclarationKind::TRAIT:
			target.kind = EditorMemberTarget::CLASS;
			break;
		default:
			return target;
	}
	if (target.kind == EditorMemberTarget::CLASS) {
		target.class_name = p_doc_class_name.is_empty() ? String() : p_doc_class_name + String(".") + name;
	} else {
		target.class_name = p_doc_class_name;
		target.class_member = name;
	}
	target.script_path = p_path;
	target.line = int32_t(p_declaration.declaration.start_line);
	target.description = String::utf8(p_declaration.documentation.c_str(),
			p_declaration.documentation.size());
	return target;
}

EditorMemberTarget own_enum_member_target(const gdscript::SourceModel &p_model,
		const String &p_symbol, const String &p_doc_class_name, const String &p_path) {
	EditorMemberTarget target;
	for (const gdscript::SourceDeclaration &declaration : p_model.declarations) {
		if (declaration.parent >= 0 || declaration.kind != gdscript::DeclarationKind::ENUM) continue;
		for (const gdscript::SourceEnumMember &member : declaration.enum_members) {
			if (String::utf8(member.name.c_str(), member.name.size()) != p_symbol) continue;
			target.kind = EditorMemberTarget::CONSTANT;
			target.class_name = p_doc_class_name;
			target.class_member = p_symbol;
			target.script_path = p_path;
			target.doc_type = "int";
			target.line = int32_t(member.declaration.start_line);
			return target;
		}
	}
	return target;
}

// Locals have no help page; the editor shows only this.
void fill_local(Dictionary &r_result, const gdscript::SourceDeclaration &p_declaration,
		const String &p_resolved_type, const String &p_path, int32_t p_line) {
	r_result["result"] = Error::OK;
	r_result["type"] = p_declaration.kind == gdscript::DeclarationKind::CONSTANT ?
			ScriptLanguageExtension::LOOKUP_RESULT_LOCAL_CONSTANT :
			ScriptLanguageExtension::LOOKUP_RESULT_LOCAL_VARIABLE;
	r_result["doc_type"] = p_resolved_type.is_empty() ? String("Variant") : p_resolved_type;
	r_result["description"] = String::utf8(p_declaration.documentation.c_str(),
			p_declaration.documentation.size());
	r_result["value"] = String::utf8(p_declaration.initializer_text.c_str(),
			p_declaration.initializer_text.size());
	if (!p_path.is_empty()) {
		r_result["script_path"] = p_path;
	}
	r_result["location"] = p_line;
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

namespace {
void collect_sgd_paths(const String &directory, PackedStringArray &paths) {
	for (const String &file : DirAccess::get_files_at(directory)) {
		if (file.ends_with(".sgd"))
			paths.push_back(directory.path_join(file));
	}
	for (const String &child : DirAccess::get_directories_at(directory)) {
		if (!child.begins_with("."))
			collect_sgd_paths(directory.path_join(child), paths);
	}
}
}

Dictionary SafeGDScriptLanguage::bake_all_translations() {
	Dictionary result;
	Array baked;
	PackedStringArray failed;
	PackedStringArray paths;
	collect_sgd_paths("res://", paths);
	std::unordered_set<std::string> live_files;
	String cache_directory;

	for (const String &path : paths) {
		Ref<Resource> resource = ResourceLoader::get_singleton()->load(path, "SafeGDScript",
				ResourceLoader::CACHE_MODE_REUSE);
		Ref<SafeGDScript> script = resource;
		if (script.is_null() || !script->compile_source_to_elf(false, false,
				SafeGDScript::ReloadPolicy::DISCARD_STATE, true)) {
			failed.push_back(path + String(": ") + (script.is_valid() ? script->get_compile_error() :
					"could not load as SafeGDScript"));
			continue;
		}

		Sandbox *sandbox = memnew(Sandbox);
		sandbox->set_unboxed_arguments(false);
		sandbox->set_memory_max(32);
		sandbox->set_binary_translation_bg_compilation(false);
		sandbox->load_buffer(script->get_content());
		const int64_t hash = sandbox->get_translation_hash();
		const String output = hash == 0 ? String() : sandbox->bake_binary_translation();
		memdelete(sandbox);
		if (output.is_empty()) {
			failed.push_back(path + String(": translation bake failed"));
			continue;
		}

		Dictionary entry;
		entry["script"] = path;
		entry["hash"] = hash;
		entry["path"] = output;
		entry["size"] = FileAccess::get_file_as_bytes(output).size();
		baked.push_back(entry);
		live_files.insert(std::string(output.get_file().utf8().get_data()));
		cache_directory = output.get_base_dir();
		UtilityFunctions::print(path, ": ", vformat("%08X", uint32_t(hash)),
				" -> ", output, " (", entry["size"], " bytes)");
	}

	// A partial bake must not delete the last working object for a script that
	// currently fails to compile. Garbage collection is safe once every source
	// contributed its current hash.
	if (failed.is_empty() && !cache_directory.is_empty()) {
		for (const String &file : DirAccess::get_files_at(cache_directory)) {
			const std::string filename(file.utf8().get_data());
			if (file.begins_with("bintr-") && live_files.count(filename) == 0 &&
					(file.ends_with(".so") || file.ends_with(".dll") || file.ends_with(".dylib"))) {
				DirAccess::remove_absolute(cache_directory.path_join(file));
			}
		}
	}

	result["baked"] = baked;
	result["failed"] = failed;
	result["ok"] = failed.is_empty();
	return result;
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
		"uses",
		"trait",
		"trait_name",
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
namespace {

struct BuiltInTemplate {
	const char *inherit;
	const char *name;
	const char *description;
	const char *content;
};

// The first four are GDScript's own script_templates; the rest show SafeGDScript.
const BuiltInTemplate built_in_templates[] = {
	{ "Node", "Default", "Base template for Node with some default methods",
			"extends _BASE_\n"
			"\n"
			"\n"
			"# Called when the node enters the scene tree for the first time.\n"
			"func _ready() -> void:\n"
			"_TS_pass # Replace with function body.\n"
			"\n"
			"\n"
			"# Called every frame. 'delta' is time since the previous frame.\n"
			"func _process(delta: float) -> void:\n"
			"_TS_pass\n" },
	{ "Object", "Empty", "Empty template suitable for all Objects",
			"extends _BASE_\n" },
	{ "Node", "GDScript Tour", "Ordinary GDScript: signals, exports, enums, match and lambdas compile unchanged",
			"extends _BASE_\n"
			"\n"
			"# SafeGDScript is GDScript compiled to RISC-V and run inside a Sandbox.\n"
			"# Everything below is ordinary GDScript and compiles unchanged.\n"
			"\n"
			"signal score_changed(new_score: int)\n"
			"\n"
			"enum State { IDLE, RUNNING, DONE }\n"
			"\n"
			"const MAX_SCORE := 100\n"
			"\n"
			"@export var speed: float = 200.0\n"
			"@export var label_text: String = \"Ready\"\n"
			"\n"
			"var state := State.IDLE\n"
			"var score: int = 0:\n"
			"_TS_set(value):\n"
			"_TS__TS_score = clampi(value, 0, MAX_SCORE)\n"
			"_TS__TS_score_changed.emit(score)\n"
			"\n"
			"@onready var start_time := Time.get_ticks_msec()\n"
			"\n"
			"\n"
			"func _ready() -> void:\n"
			"_TS_print(\"%s started at %d ms\" % [name, start_time])\n"
			"_TS_state = State.RUNNING\n"
			"\n"
			"\n"
			"func _process(delta: float) -> void:\n"
			"_TS_match state:\n"
			"_TS__TS_State.RUNNING:\n"
			"_TS__TS__TS_score += int(speed * delta)\n"
			"_TS__TS__TS_if score >= MAX_SCORE:\n"
			"_TS__TS__TS__TS_state = State.DONE\n"
			"_TS__TS_State.DONE:\n"
			"_TS__TS__TS_set_process(false)\n"
			"\n"
			"\n"
			"# Containers, lambdas and string formatting work the same as in GDScript.\n"
			"func summary(values: Array[int]) -> String:\n"
			"_TS_var evens := values.filter(func(v: int) -> bool: return v % 2 == 0)\n"
			"_TS_var total := 0\n"
			"_TS_for v in evens:\n"
			"_TS__TS_total += v\n"
			"_TS_return \"%d even values, sum %d\" % [evens.size(), total]\n" },
	{ "Node", "Typed Struct", "A struct is a Dictionary with fixed, typed fields the compiler can check and compute on directly",
			"extends _BASE_\n"
			"\n"
			"# A struct is a Dictionary with a fixed set of typed fields. Godot still sees a\n"
			"# plain Dictionary, so it works with GDScript as-is.\n"
			"\n"
			"struct Particle:\n"
			"_TS_var position: Vector2 = Vector2.ZERO\n"
			"_TS_var velocity: Vector2 = Vector2.ZERO\n"
			"_TS_var lifetime: float = 1.0\n"
			"\n"
			"_TS_func advance(delta: float) -> bool:\n"
			"_TS__TS_self.position += self.velocity * delta\n"
			"_TS__TS_self.lifetime -= delta\n"
			"_TS__TS_return self.lifetime > 0.0\n"
			"\n"
			"\n"
			"var particles: Array[Particle] = []\n"
			"\n"
			"\n"
			"func _ready() -> void:\n"
			"_TS_# Construct positionally, by name, or mix the two. Anything that's skipped will\n"
			"_TS_# keep its default value.\n"
			"_TS_particles.append(Particle(Vector2(0, 0), Vector2(50, -80)))\n"
			"_TS_particles.append(Particle.new(lifetime = 2.5, velocity = Vector2(-30, -60)))\n"
			"\n"
			"\n"
			"func _process(delta: float) -> void:\n"
			"_TS_var alive: Array[Particle] = []\n"
			"_TS_for p in particles:\n"
			"_TS__TS_if p.advance(delta):\n"
			"_TS__TS__TS_alive.append(p)\n"
			"_TS_particles = alive\n"
			"\n"
			"\n"
			"func total_lifetime() -> float:\n"
			"_TS_var total := 0.0\n"
			"_TS_for p in particles:\n"
			"_TS__TS_total += p.lifetime\n"
			"_TS_return total\n" },
	{ "Node", "Nullable and Union Types", "T? and A | B type hints, checked at compile time and narrowed by null checks and 'is'",
			"extends _BASE_\n"
			"\n"
			"# `A | B` means a value is one of the listed types, and `T?` is shorthand for\n"
			"# `T | null`. The compiler checks every assignment it can see and guards the\n"
			"# rest at run time, so a slot never holds a type it did not declare.\n"
			"\n"
			"# Starts as null. Assigning anything but a Node or null is an error.\n"
			"var target: Node? = null\n"
			"\n"
			"# Assign an integer or a string:\n"
			"var lookup_key: int | String = 0\n"
			"\n"
			"\n"
			"func _ready() -> void:\n"
			"_TS_target = get_node_or_null(\"Player\")\n"
			"\n"
			"\n"
			"func _process(_delta: float) -> void:\n"
			"_TS_# A null check narrows `target` to Node for the rest of the branch:\n"
			"_TS_if target != null:\n"
			"_TS__TS_if not target.is_inside_tree():\n"
			"_TS__TS__TS_target = null\n"
			"\n"
			"\n"
			"func describe(key: int | String) -> String:\n"
			"_TS_# `is` narrows a union so branches work with the \"known\" type:\n"
			"_TS_if key is int:\n"
			"_TS__TS_return \"id #%d\" % key\n"
			"_TS_return \"named '%s'\" % key\n"
			"\n"
			"\n"
			"func find_by(key: int | String) -> Node?:\n"
			"_TS_match typeof(key):\n"
			"_TS__TS_TYPE_INT:\n"
			"_TS__TS__TS_return get_child(key) if key < get_child_count() else null\n"
			"_TS__TS_TYPE_STRING:\n"
			"_TS__TS__TS_return get_node_or_null(key)\n"
			"_TS_return null\n"
			"\n"
			"\n"
			"# Because `Node?` is `Node | null`, a nullable result is also a valid return value\n"
			"# for a union that has at least Node and null in it:\n"
			"func resolve(key: int | String) -> Node | String | null:\n"
			"_TS_if key is String and key.is_empty():\n"
			"_TS__TS_return \"no name given\"\n"
			"_TS_return find_by(key)\n" },
	{ "Node", "If Var", "'if var name = expression:' binds a non-null result for one block",
			"extends _BASE_\n"
			"\n"
			"# `if var name = expression:` evaluates the expression once and enters the\n"
			"# block only when the result is not null. Inside the block `name` is assumed\n"
			"# to be non-null as well as any known type, avoiding checks:\n"
			"\n"
			"@export var settings: Dictionary = {\"volume\": 0.8}\n"
			"\n"
			"\n"
			"func _ready() -> void:\n"
			"_TS_if var player := get_node_or_null(\"Player\"):\n"
			"_TS__TS_print(\"Found \", player.name)\n"
			"_TS_else:\n"
			"_TS__TS_print(\"No Player sibling yet\")\n"
			"\n"
			"_TS_# When the type is specified, the value must be of that type when present:\n"
			"_TS_if var volume: float = settings.get(\"volume\"):\n"
			"_TS__TS_print(\"Volume is %.1f\" % volume)\n"
			"\n"
			"_TS_# The binding is scoped to its block:\n"
			"_TS_if var parent_node := get_parent():\n"
			"_TS__TS_print(\"Parent is \", parent_node.name)\n"
			"\n"
			"\n"
			"func first_visible(nodes: Array) -> CanvasItem?:\n"
			"_TS_for candidate in nodes:\n"
			"_TS__TS_if var item := candidate as CanvasItem:\n"
			"_TS__TS__TS_if item.visible:\n"
			"_TS__TS__TS__TS_return item\n"
			"_TS_return null\n" },
	{ "Node", "Traits", "A trait shares state and methods between classes; abstract methods are enforced",
			"extends _BASE_\n"
			"uses Damageable\n"
			"\n"
			"# A trait holds state and behaviour that classes can share. A method\n"
			"# without a body is abstract: the compiler will fail any class that"
			"# uses the trait but does not implement all of it (with diagnostics)."
			"\n"
			"trait Damageable:\n"
			"_TS_var health: int = 100\n"
			"\n"
			"_TS_func take_damage(amount: int) -> void:\n"
			"_TS__TS_health -= amount\n"
			"_TS__TS_if health <= 0:\n"
			"_TS__TS__TS_on_death()\n"
			"\n"
			"_TS_func on_death() -> void\n"
			"\n"
			"\n"
			"# This script satisfies Damageable by providing on_death().\n"
			"func on_death() -> void:\n"
			"_TS_print(name, \" was destroyed\")\n"
			"_TS_queue_free()\n"
			"\n"
			"\n"
			"# A nested class can use the same trait.\n"
			"class Crate uses Damageable:\n"
			"_TS_func on_death() -> void:\n"
			"_TS__TS_print(\"crate splinters\")\n"
			"\n"
			"\n"
			"func _ready() -> void:\n"
			"_TS_take_damage(30)\n"
			"_TS_print(\"Health left: \", health)\n"
			"\n"
			"_TS_var crate := Crate.new()\n"
			"_TS_crate.take_damage(150)\n"
			"\n"
			"\n"
			"# A trait is a type. Anything that uses it can be passed here.\n"
			"# `is` can be used to check the trait at run time on unknown values.\n"
			"func hit_all(targets: Array, amount: int) -> void:\n"
			"_TS_for target in targets:\n"
			"_TS__TS_if target is Damageable:\n"
			"_TS__TS__TS_target.take_damage(amount)\n" },
	{ "CharacterBody2D", "Basic Movement", "Classic movement for gravity games (platformer, ...)",
			"extends _BASE_\n"
			"\n"
			"\n"
			"const SPEED = 300.0\n"
			"const JUMP_VELOCITY = -400.0\n"
			"\n"
			"\n"
			"func _physics_process(delta: float) -> void:\n"
			"_TS_# Add the gravity.\n"
			"_TS_if not is_on_floor():\n"
			"_TS__TS_velocity += get_gravity() * delta\n"
			"\n"
			"_TS_# Handle jump.\n"
			"_TS_if Input.is_action_just_pressed(\"ui_accept\") and is_on_floor():\n"
			"_TS__TS_velocity.y = JUMP_VELOCITY\n"
			"\n"
			"_TS_# Get the input direction and handle the movement/deceleration.\n"
			"_TS_# As good practice, you should replace UI actions with custom gameplay actions.\n"
			"_TS_var direction := Input.get_axis(\"ui_left\", \"ui_right\")\n"
			"_TS_if direction:\n"
			"_TS__TS_velocity.x = direction * SPEED\n"
			"_TS_else:\n"
			"_TS__TS_velocity.x = move_toward(velocity.x, 0, SPEED)\n"
			"\n"
			"_TS_move_and_slide()\n" },
	{ "CharacterBody3D", "Basic Movement", "Classic movement for gravity games (FPS, TPS, ...)",
			"extends _BASE_\n"
			"\n"
			"\n"
			"const SPEED = 5.0\n"
			"const JUMP_VELOCITY = 4.5\n"
			"\n"
			"\n"
			"func _physics_process(delta: float) -> void:\n"
			"_TS_# Add the gravity.\n"
			"_TS_if not is_on_floor():\n"
			"_TS__TS_velocity += get_gravity() * delta\n"
			"\n"
			"_TS_# Handle jump.\n"
			"_TS_if Input.is_action_just_pressed(\"ui_accept\") and is_on_floor():\n"
			"_TS__TS_velocity.y = JUMP_VELOCITY\n"
			"\n"
			"_TS_# Get the input direction and handle the movement/deceleration.\n"
			"_TS_# As good practice, you should replace UI actions with custom gameplay actions.\n"
			"_TS_var input_dir := Input.get_vector(\"ui_left\", \"ui_right\", \"ui_up\", \"ui_down\")\n"
			"_TS_var direction := (transform.basis * Vector3(input_dir.x, 0, input_dir.y)).normalized()\n"
			"_TS_if direction:\n"
			"_TS__TS_velocity.x = direction.x * SPEED\n"
			"_TS__TS_velocity.z = direction.z * SPEED\n"
			"_TS_else:\n"
			"_TS__TS_velocity.x = move_toward(velocity.x, 0, SPEED)\n"
			"_TS__TS_velocity.z = move_toward(velocity.z, 0, SPEED)\n"
			"\n"
			"_TS_move_and_slide()\n" },
};

String template_indentation() {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return String("\t");
	}
	EditorInterface *editor = EditorInterface::get_singleton();
	if (editor == nullptr) {
		return String("\t");
	}
	const Ref<EditorSettings> settings = editor->get_editor_settings();
	if (settings.is_null() || !settings->has_setting("text_editor/behavior/indent/type") ||
			int(settings->get_setting("text_editor/behavior/indent/type")) == 0) {
		return String("\t");
	}
	int size = 4;
	if (settings->has_setting("text_editor/behavior/indent/size")) {
		size = int(settings->get_setting("text_editor/behavior/indent/size"));
	}
	return String(" ").repeat(size < 1 ? 1 : size);
}

} // namespace

Ref<Script> SafeGDScriptLanguage::_make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	SafeGDScript *script = memnew(SafeGDScript);
	String source = p_template;
	if (source.is_empty()) {
		if (!p_base_class_name.is_empty()) {
			source += String("extends _BASE_\n\n");
		}
		source += "# SafeGDScript is compiled to RISC-V and runs inside a Sandbox.\n\n";
		source += "func _ready():\n_TS_print(\"Hello from SafeGDScript\")\n";
	}
	source = source.replace("_BASE_", p_base_class_name)
					.replace("_CLASS_SNAKE_CASE_", p_class_name.to_snake_case())
					.replace("_CLASS_", p_class_name.to_pascal_case())
					.replace("_TS_", template_indentation());
	script->set_source_code(source);
	return Ref<Script>(script);
}
TypedArray<Dictionary> SafeGDScriptLanguage::_get_built_in_templates(const StringName &p_object) const {
	TypedArray<Dictionary> templates;
	for (size_t i = 0; i < std::size(built_in_templates); i++) {
		const BuiltInTemplate &entry = built_in_templates[i];
		if (StringName(entry.inherit) != p_object) {
			continue;
		}
		Dictionary result;
		result["inherit"] = String(entry.inherit);
		result["name"] = String(entry.name);
		result["description"] = String(entry.description);
		result["content"] = String(entry.content);
		result["id"] = int(i);
		result["origin"] = 0; // TEMPLATE_BUILT_IN
		templates.push_back(result);
	}
	return templates;
}
bool SafeGDScriptLanguage::_is_using_templates() {
	return true;
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

	// Full compile catches errors the tolerant parse misses.
	const bool wants_diagnostics = p_validate_errors || p_validate_warnings;
	ValidationResult validation;
	const bool validated = wants_diagnostics && validate_with_compiler(p_script, p_path, validation);
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
			const int64_t start_line = int64_t(std::max(diagnostic.range.start_line, 1u));
			const int64_t end_line = int64_t(std::max(diagnostic.range.end_line,
					std::max(diagnostic.range.start_line, 1u)));
			// Godot 4.7 consumes the range-based names. Keep the older aliases as
			// well because the extension's declared compatibility starts at 4.3.
			warning["start_line"] = start_line;
			warning["end_line"] = end_line;
			warning["start_column"] = int64_t(std::max(diagnostic.range.start_column, 1u));
			warning["end_column"] = int64_t(std::max(diagnostic.range.end_column,
					std::max(diagnostic.range.start_column + 1u, 2u)));
			warning["string_code"] = code;
			warning["message"] = message;
			warning["line"] = start_line;
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
			if (declaration.kind != gdscript::DeclarationKind::FUNCTION) continue;
			// Outline nests on '.'; prefix with declaring class.
			String name = String::utf8(declaration.name.c_str(), declaration.name.size());
			for (int32_t parent = declaration.parent, depth = 0; parent >= 0 &&
					size_t(parent) < model.declarations.size() && depth < 16; depth++) {
				const gdscript::SourceDeclaration &enclosing = model.declarations[size_t(parent)];
				if (enclosing.kind == gdscript::DeclarationKind::NESTED_CLASS ||
						enclosing.kind == gdscript::DeclarationKind::TRAIT) {
					name = String::utf8(enclosing.name.c_str(), enclosing.name.size()) + "." + name;
				}
				parent = enclosing.parent;
			}
			functions.push_back(name + ":" + itos(declaration.declaration.start_line));
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
	return true;
}
bool SafeGDScriptLanguage::_can_make_function() const {
	return true;
}
int32_t SafeGDScriptLanguage::_find_function(const String &p_function, const String &p_code) const {
	gdscript::SourceModel model;
	if (!analyze_with_compiler(p_code, String(), gdscript::ANALYZE_DECLARATIONS, 0, 0, model)) {
		return -1;
	}
	for (const gdscript::SourceDeclaration &declaration : model.declarations) {
		if (declaration.kind != gdscript::DeclarationKind::FUNCTION) continue;
		if (String::utf8(declaration.name.c_str(), declaration.name.size()) == p_function) {
			return int32_t(declaration.declaration.start_line);
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
	int32_t caret_line = 0;
	int32_t caret_column = 0;
	caret_position(p_code, caret_line, caret_column);
	const String source = p_code.replace(String::chr(COMPLETION_MARKER), String());
	gdscript::SourceModel semantic_model;
	const bool semantic = analyze_with_compiler(source, p_path,
			gdscript::ANALYZE_DECLARATIONS | gdscript::ANALYZE_CARET,
			caret_line, caret_column, semantic_model);

	const StringName owner = owner_class(p_owner, semantic_model);
	const EditorSymbolResolver resolver(semantic_model, p_path, p_owner);
	const gdscript::CaretContext &caret = semantic_model.caret;
	const bool has_caret = semantic && caret.kind != gdscript::CaretKind::NONE;

	// The signature of the call being written, shown above the caret whether or
	// not there is anything to complete inside it.
	if (has_caret && !caret.callee.empty()) {
		result["call_hint"] = call_hint_for(String::utf8(caret.callee.c_str(), caret.callee.size()),
				caret.argument_index, semantic_model, resolver, owner, uint32_t(caret_line));
	}

	// Scene paths need the live tree, not the compiler's line scan.
	if (ctx.scene_sugar != 0) {
		Node *scene_owner = Object::cast_to<Node>(p_owner);
		add_scene_sugar_options(options, scene_owner, scene_owner, ctx.scene_sugar);
		result["force"] = true;
		result["options"] = options;
		return result;
	}
	if (!ctx.string_call.is_empty()) {
		add_string_context_options(options, ctx, semantic_model, owner, p_owner);
		result["force"] = true;
		result["options"] = options;
		return result;
	}

	enum Branch { BRANCH_IDENTIFIER, BRANCH_MEMBER, BRANCH_ANNOTATION, BRANCH_TYPE,
		BRANCH_FUNC_DEFINITION };
	Branch branch = BRANCH_IDENTIFIER;
	String receiver;
	if (has_caret) {
		switch (caret.kind) {
			case gdscript::CaretKind::MEMBER:
				branch = BRANCH_MEMBER;
				receiver = String::utf8(caret.receiver_text.c_str(), caret.receiver_text.size());
				break;
			case gdscript::CaretKind::ANNOTATION: branch = BRANCH_ANNOTATION; break;
			case gdscript::CaretKind::TYPE: branch = BRANCH_TYPE; break;
			default: branch = BRANCH_IDENTIFIER; break;
		}
	} else if (ctx.annotation) {
		branch = BRANCH_ANNOTATION;
	} else if (ctx.member) {
		branch = BRANCH_MEMBER;
		receiver = ctx.receiver;
	} else if (ctx.type_position) {
		branch = BRANCH_TYPE;
	}
	if (branch == BRANCH_IDENTIFIER && ctx.func_definition) {
		branch = BRANCH_FUNC_DEFINITION;
	}

	switch (branch) {
		case BRANCH_ANNOTATION:
			for (const char *const *annotation = annotation_names; *annotation != nullptr; annotation++) {
				add_option(options, CODE_COMPLETION_KIND_PLAIN_TEXT, *annotation, *annotation);
			}
			result["force"] = true;
			break;
		case BRANCH_MEMBER: {
			if (receiver.is_empty() || receiver == "self") {
				add_class_members(options, owner, false, LOCATION_OTHER, true);
				add_script_functions(options, semantic_model);
				add_own_declarations(options, semantic_model);
				add_own_members(options, semantic_model);
			} else if (receiver == "super") {
				add_super_members(options, model_base_class(semantic_model), p_owner);
			} else {
				EditorTypeRef type = resolver.resolve_receiver(receiver, uint32_t(caret_line));
				if (!type.is_valid() && !caret.receiver_type.empty()) {
					type = resolver.resolve_type_name(String::utf8(caret.receiver_type.c_str(),
							caret.receiver_type.size()));
				}
				if (!add_type_ref_members(options, type, semantic_model) &&
						!add_global_enum_members(options, receiver)) {
					// Unknown type; offer all members.
					add_any_builtin_members(options);
				}
			}
			result["force"] = true;
			break;
		}
		case BRANCH_FUNC_DEFINITION:
			add_virtual_methods(options, owner);
			result["force"] = true;
			break;
		case BRANCH_TYPE: {
			for (const char *const *name = builtin_types; *name != nullptr; name++) {
				add_option(options, CODE_COMPLETION_KIND_CLASS, *name, *name);
			}
			ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
			if (class_db != nullptr) {
				for (const String &name : class_db->get_class_list()) {
					add_option(options, CODE_COMPLETION_KIND_CLASS, name, name);
				}
			}
			add_own_declarations(options, semantic_model);
			result["force"] = true;
			break;
		}
		case BRANCH_IDENTIFIER: {
			for (const String &keyword : _get_reserved_words()) {
				add_option(options, CODE_COMPLETION_KIND_PLAIN_TEXT, keyword, keyword);
			}
			for (const char *const *name = builtin_types; *name != nullptr; name++) {
				add_option(options, CODE_COMPLETION_KIND_CLASS, *name, *name);
			}
			for (const char *const *name = constructible_types; *name != nullptr; name++) {
				add_option(options, CODE_COMPLETION_KIND_CLASS, String(*name) + String("("), String(*name) + String("("));
			}
			add_global_constants(options);
			add_global_enum_names(options);
			add_global_functions(options);
			ProjectSettings *project = ProjectSettings::get_singleton();
			if (project != nullptr) {
				for (const Dictionary &entry : project->get_global_class_list()) {
					const String name = entry.get("class", String());
					if (!name.is_empty()) add_option(options, CODE_COMPLETION_KIND_CLASS, name, name);
				}
				for (const Dictionary &entry : project->get_property_list()) {
					const String setting = entry.get("name", String());
					if (!setting.begins_with("autoload/")) continue;
					add_option(options, CODE_COMPLETION_KIND_VARIABLE, setting.trim_prefix("autoload/"),
							setting.trim_prefix("autoload/"));
				}
			}
			// Struct names double as constructors.
			add_own_of_kind(options, semantic_model, gdscript::DeclarationKind::STRUCT,
					CODE_COMPLETION_KIND_CLASS, "(");
			add_script_functions(options, semantic_model);
			add_own_declarations(options, semantic_model);
			if (semantic) {
				for (const gdscript::SourceDeclaration *declaration : resolver.visible_declarations(caret_line)) {
					const String name = String::utf8(declaration->name.c_str(), declaration->name.size());
					const int location = declaration->parent < 0 ? LOCATION_OTHER_USER_CODE : LOCATION_LOCAL;
					if (declaration->kind == gdscript::DeclarationKind::VARIABLE ||
							declaration->kind == gdscript::DeclarationKind::PARAMETER) {
						add_option(options, CODE_COMPLETION_KIND_VARIABLE, name, name, location);
					} else if (declaration->kind == gdscript::DeclarationKind::CONSTANT) {
						add_option(options, CODE_COMPLETION_KIND_CONSTANT, name, name, location);
					}
				}
			}

			// Unqualified calls resolve to self.<name>().
			add_class_members(options, owner);
			break;
		}
	}

	result["options"] = options;
	return result;
}

Dictionary SafeGDScriptLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	// Both "result" and "type" required: ERR_FAIL_COND_V on either missing.
	// Called on every hover; missing key → two errors per mouse move.
	Dictionary result;
	result["result"] = Error::ERR_CANT_RESOLVE;
	result["type"] = LOOKUP_RESULT_SCRIPT_LOCATION;
	int32_t caret_line = 0;
	int32_t caret_column = 0;
	caret_position(p_code, caret_line, caret_column);
	const String code = p_code.replace(String::chr(COMPLETION_MARKER), String());
	// Resolve from the word's first character.
	{
		const PackedStringArray lines = code.split("\n");
		if (caret_line >= 1 && caret_line <= lines.size()) {
			const String line = lines[caret_line - 1];
			while (caret_column > 0 && caret_column <= line.length() &&
					is_identifier_char(line[caret_column - 1])) {
				caret_column--;
			}
		}
	}
	gdscript::SourceModel semantic_model;
	const bool semantic = analyze_with_compiler(code, p_path,
			gdscript::ANALYZE_DECLARATIONS | gdscript::ANALYZE_CARET,
			caret_line, caret_column, semantic_model);
	if (semantic) {
		EditorSymbolResolver resolver(semantic_model, p_path, p_owner);
		const String doc_class = editing_doc_class_name(p_path, semantic_model);
		const String receiver = String::utf8(semantic_model.caret.receiver_text.c_str(),
				semantic_model.caret.receiver_text.size());
		const bool member_caret = semantic_model.caret.kind == gdscript::CaretKind::MEMBER &&
				!receiver.is_empty();
		const bool on_self = receiver == "self" || receiver == "super";

		if (member_caret && !on_self) {
			const EditorTypeRef type = resolver.resolve_receiver(receiver, uint32_t(caret_line));
			if (fill_lookup(result, resolver.member_of_type(type, p_symbol, doc_class))) {
				return result;
			}
		}

		if (!member_caret || on_self) {
			if (!member_caret) {
				const EditorResolvedSymbol resolved = resolver.resolve(p_symbol, uint32_t(caret_line));
				if (resolved.declaration != nullptr && resolved.declaration->parent >= 0) {
					fill_local(result, *resolved.declaration, resolved.resolved_type, p_path,
							resolved.line);
					return result;
				}
			}
			if (receiver != "super") {
				if (const gdscript::SourceDeclaration *own = find_own_declaration(semantic_model, p_symbol)) {
					if (fill_lookup(result, own_member_target(*own, doc_class, p_path))) {
						return result;
					}
					result["result"] = Error::OK;
					result["type"] = LOOKUP_RESULT_SCRIPT_LOCATION;
					result["location"] = int32_t(own->declaration.start_line);
					result["script_path"] = p_path;
					return result;
				}
				if (fill_lookup(result, own_enum_member_target(semantic_model, p_symbol, doc_class,
						p_path))) {
					return result;
				}
			}
			const Ref<Script> base = model_base_script(semantic_model);
			if (base.is_valid() &&
					fill_lookup(result, EditorSymbolResolver::member_of_script(base, p_symbol))) {
				return result;
			}
		}
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

	{
		Ref<Script> named = EditorSymbolResolver::script_for_autoload(p_symbol);
		if (named.is_null()) {
			named = EditorSymbolResolver::script_for_global_class(p_symbol);
		}
		if (named.is_valid()) {
			EditorMemberTarget target;
			target.kind = EditorMemberTarget::CLASS;
			target.class_name = EditorSymbolResolver::doc_name_of_script(named);
			target.script_path = named->get_path();
			target.line = 1;
			if (fill_lookup(result, target)) {
				return result;
			}
		}
	}

	const StringName owner = owner_class(p_owner, semantic_model);
	if (fill_lookup(result, EditorSymbolResolver::member_of_engine_class(String(owner), p_symbol))) {
		return result;
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
	array.push_back("safegd");
	return array;
}
TypedArray<Dictionary> SafeGDScriptLanguage::_get_public_functions() const {
	// Argument types are not in the table; published as Variant.
	Dictionary return_value;
	return_value["type"] = Variant::NIL;
	return_value["usage"] = PROPERTY_USAGE_NIL_IS_VARIANT;

	TypedArray<Dictionary> functions;
	for (size_t i = 0; i < gdscript::global_function_count(); i++) {
		const char *name = gdscript::global_function_name(i);
		if (name == nullptr) {
			continue;
		}
		const uint8_t min_args = gdscript::global_function_min_args(i);
		Array arguments;
		for (uint8_t argument = 0; argument < min_args; argument++) {
			Dictionary info;
			info["name"] = String("arg") + itos(argument);
			info["type"] = Variant::NIL;
			info["usage"] = PROPERTY_USAGE_NIL_IS_VARIANT;
			arguments.push_back(info);
		}
		Dictionary method;
		method["name"] = String(name);
		method["args"] = arguments;
		method["default_args"] = Array();
		method["return"] = return_value;
		method["flags"] = gdscript::global_function_max_args(i) > min_args ?
				int(METHOD_FLAG_VARARG) : int(METHOD_FLAGS_DEFAULT);
		functions.push_back(method);
	}
	return functions;
}
Dictionary SafeGDScriptLanguage::_get_public_constants() const {
	Dictionary constants;
	for (size_t i = 0; i < gdscript::global_constant_count(); i++) {
		Dictionary entry;
		entry["name"] = String(gdscript::global_constant_name(i));
		entry["value"] = gdscript::global_constant_is_float(i) ?
				Variant(gdscript::global_constant_float_value(i)) :
				Variant(gdscript::global_constant_int_value(i));
		constants[int(i)] = entry;
	}
	return constants;
}
TypedArray<Dictionary> SafeGDScriptLanguage::_get_public_annotations() const {
	TypedArray<Dictionary> annotations;
	for (const char *const *name = annotation_names; *name != nullptr; name++) {
		Dictionary info;
		info["name"] = String("@") + String(*name);
		info["args"] = Array();
		info["default_args"] = Array();
		info["flags"] = int(METHOD_FLAGS_DEFAULT);
		annotations.push_back(info);
	}
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
	bool is_trait = false;
	if (FileAccess::file_exists(p_path)) {
		SafeGDScript::scan_class_header(FileAccess::get_file_as_string(p_path),
				&declared_name, &declared_base, &is_trait);
	}
	dict["name"] = declared_name.is_empty() ? SafeGDScript::PathToGlobalName(p_path) : declared_name;
	if (is_trait && declared_base.is_empty()) {
		dict["base_type"] = "RefCounted";
	} else if (declared_base.is_empty() || declared_base.begins_with("res://") ||
			declared_base.begins_with("user://")) {
		dict["base_type"] = "Sandbox";
	} else {
		dict["base_type"] = declared_base;
	}
	dict["icon_path"] = String(icon_path);
	if (is_trait) dict["is_abstract"] = true;
	return dict;
}
