#include "editor_analysis_safegdscript.h"

#include "builtin_api_safegdscript.h"
#include "script_safegdscript.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

namespace {

String utf8_of(const std::string &p_text) {
	return String::utf8(p_text.c_str(), p_text.size());
}

String bare_type_name(const String &p_type) {
	String type = p_type.strip_edges();
	if (type.ends_with("?")) type = type.trim_suffix("?").strip_edges();
	const int bracket = type.find("[");
	if (bracket > 0) type = type.substr(0, bracket);
	const int bar = type.find("|");
	if (bar > 0) type = type.substr(0, bar).strip_edges();
	return type;
}

String segment_name(const String &p_segment, bool &r_is_call, String &r_first_argument) {
	const int open = p_segment.find("(");
	if (open < 0) {
		r_is_call = false;
		return p_segment.strip_edges();
	}
	r_is_call = true;
	const int close = p_segment.rfind(")");
	if (close > open) {
		const String inside = p_segment.substr(open + 1, close - open - 1).strip_edges();
		if (inside.length() >= 2 && (inside[0] == '"' || inside[0] == '\'')) {
			const int end = inside.rfind(String::chr(inside[0]));
			if (end > 0) r_first_argument = inside.substr(1, end - 1);
		}
	}
	return p_segment.substr(0, open).strip_edges();
}

String type_of_info(const Dictionary &p_info) {
	const String class_name = p_info.get("class_name", StringName());
	if (!class_name.is_empty()) return class_name;
	const int type = int(p_info.get("type", int(Variant::NIL)));
	if (type == int(Variant::NIL)) return String();
	return Variant::get_type_name(Variant::Type(type));
}

bool engine_class_exists(const String &p_name) {
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	return class_db != nullptr && !p_name.is_empty() && class_db->class_exists(p_name);
}

} // namespace

EditorSymbolResolver::EditorSymbolResolver(const gdscript::SourceModel &p_model,
		const String &p_source_path, Object *p_owner) :
		model(p_model), source_path(p_source_path), owner(p_owner) {}

int32_t EditorSymbolResolver::containing_function(uint32_t p_line) const {
	int32_t best = -1;
	for (size_t i = 0; i < model.declarations.size(); i++) {
		const gdscript::SourceDeclaration &declaration = model.declarations[i];
		if (declaration.kind == gdscript::DeclarationKind::FUNCTION &&
				declaration.lexical_scope.start_line <= p_line &&
				p_line <= declaration.lexical_scope.end_line) {
			best = int32_t(i);
		}
	}
	return best;
}

std::vector<const gdscript::SourceDeclaration *> EditorSymbolResolver::visible_declarations(
		uint32_t p_line) const {
	const int32_t function = containing_function(p_line);
	std::vector<const gdscript::SourceDeclaration *> result;
	for (const gdscript::SourceDeclaration &declaration : model.declarations) {
		const bool local = declaration.parent >= 0 &&
				(declaration.kind == gdscript::DeclarationKind::VARIABLE ||
				 declaration.kind == gdscript::DeclarationKind::CONSTANT ||
				 declaration.kind == gdscript::DeclarationKind::PARAMETER);
		if (local) {
			if (declaration.parent != function || declaration.declaration.start_line > p_line ||
					p_line > declaration.lexical_scope.end_line) {
				continue;
			}
		} else if (declaration.parent >= 0 && declaration.parent != function) {
			continue;
		}
		result.push_back(&declaration);
	}
	std::stable_sort(result.begin(), result.end(), [function](const auto *a, const auto *b) {
		const bool a_local = a->parent == function;
		const bool b_local = b->parent == function;
		if (a_local != b_local) return a_local;
		return a->declaration.start_line > b->declaration.start_line;
	});
	return result;
}

EditorResolvedSymbol EditorSymbolResolver::resolve(const StringName &p_name, uint32_t p_line) const {
	const String wanted(p_name);
	for (const gdscript::SourceDeclaration *declaration : visible_declarations(p_line)) {
		if (utf8_of(declaration->name) != wanted) continue;
		EditorResolvedSymbol result;
		result.declaration = declaration;
		result.script_path = source_path;
		result.resolved_type = utf8_of(
				declaration->resolved_type.empty() ? declaration->declared_type : declaration->resolved_type);
		result.line = int32_t(declaration->declaration.start_line);
		return result;
	}
	return {};
}

int32_t EditorSymbolResolver::find_member(const String &p_name, gdscript::DeclarationKind p_kind) const {
	for (size_t i = 0; i < model.declarations.size(); i++) {
		const gdscript::SourceDeclaration &declaration = model.declarations[i];
		if (declaration.parent < 0 && declaration.kind == p_kind &&
				utf8_of(declaration.name) == p_name) {
			return int32_t(i);
		}
	}
	return -1;
}

PackedStringArray EditorSymbolResolver::split_chain(const String &p_chain) {
	PackedStringArray segments;
	int depth = 0;
	char32_t quote = 0;
	int start = 0;
	for (int i = 0; i < p_chain.length(); i++) {
		const char32_t c = p_chain[i];
		if (quote != 0) {
			if (c == '\\') i++;
			else if (c == quote) quote = 0;
			continue;
		}
		if (c == '"' || c == '\'') quote = c;
		else if (c == '(' || c == '[') depth++;
		else if (c == ')' || c == ']') depth--;
		else if (c == '.' && depth == 0) {
			segments.push_back(p_chain.substr(start, i - start));
			start = i + 1;
		}
	}
	segments.push_back(p_chain.substr(start));
	return segments;
}

Ref<Script> EditorSymbolResolver::script_for_global_class(const String &p_name) {
	ProjectSettings *project = ProjectSettings::get_singleton();
	if (project == nullptr || p_name.is_empty()) return Ref<Script>();
	for (const Dictionary &entry : project->get_global_class_list()) {
		if (String(entry.get("class", String())) != p_name) continue;
		const String path = entry.get("path", String());
		if (path.is_empty()) break;
		return Ref<Script>(ResourceLoader::get_singleton()->load(path));
	}
	return Ref<Script>();
}

Ref<Script> EditorSymbolResolver::script_for_autoload(const String &p_name) {
	ProjectSettings *project = ProjectSettings::get_singleton();
	if (project == nullptr || p_name.is_empty()) return Ref<Script>();
	const String setting = String("autoload/") + p_name;
	if (!project->has_setting(setting)) return Ref<Script>();
	String path = project->get_setting(setting, String());
	if (path.begins_with("*")) path = path.substr(1);
	if (!path.begins_with("res://") && !path.begins_with("user://")) return Ref<Script>();
	return Ref<Script>(ResourceLoader::get_singleton()->load(path));
}

EditorTypeRef EditorSymbolResolver::resolve_type_name(const String &p_name) const {
	EditorTypeRef result;
	const String name = bare_type_name(p_name);
	if (name.is_empty()) return result;
	for (size_t i = 0; i < model.declarations.size(); i++) {
		const gdscript::SourceDeclaration &declaration = model.declarations[i];
		if (declaration.parent >= 0 || utf8_of(declaration.name) != name) continue;
		if (declaration.kind == gdscript::DeclarationKind::NESTED_CLASS) {
			result.kind = EditorTypeRef::NESTED_CLASS;
		} else if (declaration.kind == gdscript::DeclarationKind::STRUCT) {
			result.kind = EditorTypeRef::STRUCT;
		} else if (declaration.kind == gdscript::DeclarationKind::TRAIT) {
			result.kind = EditorTypeRef::TRAIT;
		} else if (declaration.kind == gdscript::DeclarationKind::ENUM) {
			result.kind = EditorTypeRef::SCRIPT_ENUM;
		} else {
			continue;
		}
		result.name = name;
		result.declaration = int32_t(i);
		return result;
	}
	if (safegd_builtin::find_builtin_class(name) != nullptr) {
		result.kind = EditorTypeRef::BUILTIN;
		result.name = name;
		return result;
	}
	if (engine_class_exists(name)) {
		result.kind = EditorTypeRef::ENGINE_CLASS;
		result.name = name;
		return result;
	}
	const Ref<Script> script = script_for_global_class(name);
	if (script.is_valid()) {
		result.kind = EditorTypeRef::SCRIPT;
		result.name = name;
		result.script = script;
		return result;
	}
	return result;
}

EditorTypeRef EditorSymbolResolver::resolve_head(const String &p_segment, uint32_t p_line) const {
	EditorTypeRef result;
	const String segment = p_segment.strip_edges();
	if (segment.is_empty()) return result;

	if (segment[0] == '$' || segment[0] == '%') {
		Node *node = Object::cast_to<Node>(owner);
		String path = segment.substr(1);
		if (segment[0] == '%') path = String("%") + path;
		if (path.length() >= 2 && (path[0] == '"' || path[0] == '\'')) {
			path = path.substr(1, path.length() - 2);
		}
		Node *found = node == nullptr ? nullptr : node->get_node_or_null(NodePath(path));
		if (found != nullptr) {
			result.kind = EditorTypeRef::ENGINE_CLASS;
			result.name = found->get_class();
		}
		return result;
	}

	bool is_call = false;
	String argument;
	const String name = segment_name(segment, is_call, argument);
	if (name.is_empty()) return result;

	if (is_call) {
		if ((name == "preload" || name == "load") && !argument.is_empty()) {
			const Ref<Script> script = ResourceLoader::get_singleton()->load(argument);
			if (script.is_valid()) {
				result.kind = EditorTypeRef::SCRIPT;
				result.name = argument.get_file().get_basename();
				result.script = script;
				result.meta = true;
			}
			return result;
		}
		if ((name == "get_node" || name == "get_node_or_null") && !argument.is_empty()) {
			Node *node = Object::cast_to<Node>(owner);
			Node *found = node == nullptr ? nullptr : node->get_node_or_null(NodePath(argument));
			if (found != nullptr) {
				result.kind = EditorTypeRef::ENGINE_CLASS;
				result.name = found->get_class();
			}
			return result;
		}
		EditorTypeRef constructed = resolve_type_name(name);
		if (constructed.is_valid() && constructed.kind != EditorTypeRef::SCRIPT_ENUM) {
			constructed.meta = false;
			return constructed;
		}
		const int32_t function = find_member(name, gdscript::DeclarationKind::FUNCTION);
		if (function >= 0) {
			return resolve_type_name(utf8_of(model.declarations[size_t(function)].return_type));
		}
		if (owner != nullptr) {
			EditorTypeRef self;
			self.kind = EditorTypeRef::ENGINE_CLASS;
			self.name = owner->get_class();
			return step(self, segment);
		}
		return result;
	}

	// Source-declared type names resolve as the type, not a value.
	const EditorResolvedSymbol resolved = resolve(StringName(name), p_line);
	if (resolved.declaration != nullptr &&
			(resolved.declaration->kind == gdscript::DeclarationKind::VARIABLE ||
			 resolved.declaration->kind == gdscript::DeclarationKind::PARAMETER ||
			 resolved.declaration->kind == gdscript::DeclarationKind::CONSTANT)) {
		return resolve_type_name(resolved.resolved_type);
	}
	// An autoload is an instance; everything else that names a type is the type.
	const Ref<Script> autoload = script_for_autoload(name);
	if (autoload.is_valid()) {
		result.kind = EditorTypeRef::SCRIPT;
		result.name = name;
		result.script = autoload;
		return result;
	}
	EditorTypeRef named = resolve_type_name(name);
	if (named.is_valid()) {
		named.meta = true;
		return named;
	}
	if (owner != nullptr) {
		EditorTypeRef self;
		self.kind = EditorTypeRef::ENGINE_CLASS;
		self.name = owner->get_class();
		return step(self, segment);
	}
	return result;
}

EditorTypeRef EditorSymbolResolver::step(const EditorTypeRef &p_current, const String &p_segment) const {
	EditorTypeRef result;
	if (!p_current.is_valid()) return result;
	bool is_call = false;
	String argument;
	const String name = segment_name(p_segment, is_call, argument);
	if (name.is_empty()) return result;
	if (p_current.meta && is_call && name == "new") {
		result = p_current;
		result.meta = false;
		return result;
	}

	switch (p_current.kind) {
		case EditorTypeRef::BUILTIN: {
			const safegd_builtin::BuiltinClassInfo *info = safegd_builtin::find_builtin_class(p_current.name);
			if (info == nullptr) break;
			for (const safegd_builtin::BuiltinMember *member = info->members; member->name != nullptr; member++) {
				if (name == member->name) return resolve_type_name(member->type);
			}
			for (const safegd_builtin::BuiltinMember *constant = info->constants; constant->name != nullptr; constant++) {
				if (name == constant->name) return resolve_type_name(constant->type);
			}
			for (const safegd_builtin::BuiltinMethod *method = info->methods; method->name != nullptr; method++) {
				if (name == method->name) return resolve_type_name(method->return_type);
			}
			break;
		}
		case EditorTypeRef::ENGINE_CLASS: {
			ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
			if (class_db == nullptr || !class_db->class_exists(p_current.name)) break;
			if (class_db->class_has_integer_constant(p_current.name, name)) {
				return resolve_type_name("int");
			}
			for (const Dictionary &property : class_db->class_get_property_list(p_current.name, false)) {
				if (String(property.get("name", String())) != name) continue;
				return resolve_type_name(type_of_info(property));
			}
			for (const Dictionary &method : class_db->class_get_method_list(p_current.name, false)) {
				if (String(method.get("name", String())) != name) continue;
				return resolve_type_name(type_of_info(method.get("return", Dictionary())));
			}
			break;
		}
		case EditorTypeRef::SCRIPT: {
			Ref<Script> script = p_current.script;
			if (script.is_null()) break;
			for (Ref<Script> at = script; at.is_valid(); at = at->get_base_script()) {
				for (const Dictionary &method : at->get_script_method_list()) {
					if (String(method.get("name", String())) != name) continue;
					return resolve_type_name(type_of_info(method.get("return", Dictionary())));
				}
				for (const Dictionary &property : at->get_script_property_list()) {
					if (String(property.get("name", String())) != name) continue;
					return resolve_type_name(type_of_info(property));
				}
			}
			EditorTypeRef native;
			native.kind = EditorTypeRef::ENGINE_CLASS;
			native.name = script->get_instance_base_type();
			if (engine_class_exists(native.name)) return step(native, p_segment);
			break;
		}
		case EditorTypeRef::STRUCT:
		case EditorTypeRef::NESTED_CLASS:
		case EditorTypeRef::TRAIT: {
			if (p_current.declaration < 0 ||
					size_t(p_current.declaration) >= model.declarations.size()) break;
			const gdscript::SourceDeclaration &declaration = model.declarations[size_t(p_current.declaration)];
			for (int32_t child : declaration.children) {
				if (child < 0 || size_t(child) >= model.declarations.size()) continue;
				const gdscript::SourceDeclaration &member = model.declarations[size_t(child)];
				if (utf8_of(member.name) != name) continue;
				return resolve_type_name(utf8_of(member.kind == gdscript::DeclarationKind::FUNCTION ?
						member.return_type : member.declared_type));
			}
			break;
		}
		case EditorTypeRef::SCRIPT_ENUM:
			return resolve_type_name("int");
		default:
			break;
	}
	return result;
}

String EditorSymbolResolver::doc_name_of_script(const Ref<Script> &p_script) {
	if (p_script.is_null()) return String();
	if (const SafeGDScript *script = Object::cast_to<SafeGDScript>(p_script.ptr())) {
		return String(script->_get_doc_class_name());
	}
	const String global(p_script->get_global_name());
	if (!global.is_empty()) return global;
	const String path = p_script->get_path();
	return path.is_empty() ? String() : String("\"") + path.trim_prefix("res://") + "\"";
}

int32_t EditorSymbolResolver::member_line_of_script(const Ref<Script> &p_script, const String &p_symbol) {
	if (p_script.is_null()) return -1;
	const SafeGDScript *script = Object::cast_to<SafeGDScript>(p_script.ptr());
	return script == nullptr ? -1 : script->_get_member_line(StringName(p_symbol));
}

EditorMemberTarget EditorSymbolResolver::member_of_script(const Ref<Script> &p_script,
		const String &p_symbol) {
	EditorMemberTarget target;
	int depth = 0;
	for (Ref<Script> at = p_script; at.is_valid() && depth < 64; at = at->get_base_script(), depth++) {
		EditorMemberTarget::Kind kind = EditorMemberTarget::NONE;
		for (const Dictionary &method : at->get_script_method_list()) {
			if (String(method.get("name", String())) == p_symbol) { kind = EditorMemberTarget::METHOD; break; }
		}
		if (kind == EditorMemberTarget::NONE) {
			for (const Dictionary &property : at->get_script_property_list()) {
				if (String(property.get("name", String())) == p_symbol) {
					kind = EditorMemberTarget::PROPERTY;
					target.doc_type = type_of_info(property);
					break;
				}
			}
		}
		if (kind == EditorMemberTarget::NONE) {
			for (const Dictionary &signal : at->get_script_signal_list()) {
				if (String(signal.get("name", String())) == p_symbol) { kind = EditorMemberTarget::SIGNAL; break; }
			}
		}
		if (kind == EditorMemberTarget::NONE) {
			for (const Variant &key : at->get_script_constant_map().keys()) {
				if (String(key) == p_symbol) { kind = EditorMemberTarget::CONSTANT; break; }
			}
		}
		if (kind == EditorMemberTarget::NONE) continue;
		target.kind = kind;
		target.class_name = doc_name_of_script(at);
		target.class_member = p_symbol;
		target.script_path = at->get_path();
		target.line = member_line_of_script(at, p_symbol);
		return target;
	}
	return target;
}

EditorMemberTarget EditorSymbolResolver::member_of_engine_class(const String &p_class,
		const String &p_symbol) {
	EditorMemberTarget target;
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (class_db == nullptr || !engine_class_exists(p_class) || p_symbol.is_empty()) return target;
	if (class_db->class_has_method(p_class, p_symbol)) {
		target.kind = EditorMemberTarget::METHOD;
	} else if (class_db->class_has_signal(p_class, p_symbol)) {
		target.kind = EditorMemberTarget::SIGNAL;
	} else if (class_db->class_has_enum(p_class, p_symbol)) {
		target.kind = EditorMemberTarget::ENUM;
	} else if (class_db->class_has_integer_constant(p_class, p_symbol)) {
		target.kind = EditorMemberTarget::CONSTANT;
	} else {
		for (const Dictionary &property : class_db->class_get_property_list(p_class)) {
			if (String(property.get("name", String())) != p_symbol) continue;
			target.kind = EditorMemberTarget::PROPERTY;
			target.doc_type = type_of_info(property);
			break;
		}
	}
	if (!target.is_valid()) return target;
	target.class_name = p_class;
	target.class_member = p_symbol;
	return target;
}

EditorMemberTarget EditorSymbolResolver::member_of_builtin(const String &p_type,
		const String &p_symbol) {
	EditorMemberTarget target;
	const safegd_builtin::BuiltinClassInfo *info = safegd_builtin::find_builtin_class(p_type);
	if (info == nullptr || p_symbol.is_empty()) return target;
	for (const safegd_builtin::BuiltinMember *member = info->members; member->name != nullptr; member++) {
		if (p_symbol != member->name) continue;
		target.kind = EditorMemberTarget::PROPERTY;
		target.doc_type = member->type;
		break;
	}
	if (!target.is_valid()) {
		for (const safegd_builtin::BuiltinMember *constant = info->constants; constant->name != nullptr; constant++) {
			if (p_symbol != constant->name) continue;
			target.kind = EditorMemberTarget::CONSTANT;
			target.doc_type = constant->type;
			break;
		}
	}
	if (!target.is_valid()) {
		for (const safegd_builtin::BuiltinMethod *method = info->methods; method->name != nullptr; method++) {
			if (p_symbol != method->name) continue;
			target.kind = EditorMemberTarget::METHOD;
			target.doc_type = method->return_type;
			break;
		}
	}
	if (!target.is_valid()) return target;
	target.class_name = p_type;
	target.class_member = p_symbol;
	return target;
}

EditorMemberTarget EditorSymbolResolver::member_of_type(const EditorTypeRef &p_type,
		const String &p_symbol, const String &p_doc_class_name) const {
	EditorMemberTarget target;
	if (!p_type.is_valid() || p_symbol.is_empty()) return target;
	switch (p_type.kind) {
		case EditorTypeRef::ENGINE_CLASS:
			return member_of_engine_class(p_type.name, p_symbol);
		case EditorTypeRef::BUILTIN:
			return member_of_builtin(p_type.name, p_symbol);
		case EditorTypeRef::SCRIPT: {
			target = member_of_script(p_type.script, p_symbol);
			if (target.is_valid()) return target;
			if (p_type.script.is_valid()) {
				return member_of_engine_class(p_type.script->get_instance_base_type(), p_symbol);
			}
			return target;
		}
		case EditorTypeRef::SCRIPT_ENUM: {
			if (p_type.declaration < 0 || size_t(p_type.declaration) >= model.declarations.size()) break;
			const gdscript::SourceDeclaration &declaration = model.declarations[size_t(p_type.declaration)];
			for (const gdscript::SourceEnumMember &member : declaration.enum_members) {
				if (utf8_of(member.name) != p_symbol) continue;
				target.kind = EditorMemberTarget::CONSTANT;
				target.class_name = p_doc_class_name;
				target.class_member = p_symbol;
				target.script_path = source_path;
				target.doc_type = "int";
				target.line = int32_t(member.declaration.start_line);
				return target;
			}
			break;
		}
		case EditorTypeRef::STRUCT:
		case EditorTypeRef::NESTED_CLASS:
		case EditorTypeRef::TRAIT: {
			if (p_type.declaration < 0 || size_t(p_type.declaration) >= model.declarations.size()) break;
			const gdscript::SourceDeclaration &declaration = model.declarations[size_t(p_type.declaration)];
			for (int32_t child : declaration.children) {
				if (child < 0 || size_t(child) >= model.declarations.size()) continue;
				const gdscript::SourceDeclaration &member = model.declarations[size_t(child)];
				if (utf8_of(member.name) != p_symbol) continue;
				switch (member.kind) {
					case gdscript::DeclarationKind::FUNCTION:
						target.kind = EditorMemberTarget::METHOD;
						target.doc_type = utf8_of(member.return_type);
						break;
					case gdscript::DeclarationKind::VARIABLE:
						target.kind = EditorMemberTarget::PROPERTY;
						target.doc_type = utf8_of(member.declared_type);
						break;
					case gdscript::DeclarationKind::CONSTANT:
						target.kind = EditorMemberTarget::CONSTANT;
						break;
					case gdscript::DeclarationKind::SIGNAL:
						target.kind = EditorMemberTarget::SIGNAL;
						break;
					default:
						break;
				}
				if (!target.is_valid()) break;
				target.class_name = p_doc_class_name.is_empty() ? utf8_of(declaration.name) :
						p_doc_class_name + String(".") + utf8_of(declaration.name);
				target.class_member = p_symbol;
				target.script_path = source_path;
				target.description = utf8_of(member.documentation);
				target.line = int32_t(member.declaration.start_line);
				return target;
			}
			break;
		}
		default:
			break;
	}
	return target;
}

EditorTypeRef EditorSymbolResolver::resolve_receiver(const String &p_chain, uint32_t p_line) const {
	const PackedStringArray segments = split_chain(p_chain.strip_edges());
	if (segments.is_empty()) return EditorTypeRef();
	EditorTypeRef current = resolve_head(segments[0], p_line);
	for (int i = 1; i < segments.size() && current.is_valid(); i++) {
		current = step(current, segments[i]);
	}
	return current;
}
