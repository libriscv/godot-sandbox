#include "script_safegdscript.h"

#include "compiler_backend.h"
#include "editor_analysis_safegdscript.h"
#include "sgd_timing.h"
#include "../elf/script_instance.h"
#include "script_class_safegdscript.h"
#include "script_instance_safegdscript.h"
#include "placeholder_instance_safegdscript.h"
#include "script_dicts.h"
#include "signature_info.h"
#include "script_language_safegdscript.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include "../gdscript/compiler/function_signature.h"
#include "../sandbox.h"
#include "../sandbox_project_settings.h"
#include "../fast_cast.hpp"
#include <unordered_set>
static constexpr bool VERBOSE_LOGGING = false;

bool safegdscript_is_stopped();
const SafeGDScript *safegdscript_stopped_script();
int64_t safegdscript_stopped_line();
PackedStringArray safegdscript_stopped_backtrace();
void safegdscript_debug_continue();
PackedInt32Array safegdscript_engine_breakpoints(const SafeGDScript &p_script);
Sandbox *sandbox_for_safegdscript(const SafeGDScript *p_script);
Sandbox *safegdscript_acquire_sandbox(Object *p_owner, const Ref<SafeGDScript> &p_script,
		bool p_restricted);
void safegdscript_release_sandbox(SafeGDScript *p_script, Object *p_owner);

bool SafeGDScript::_editor_can_reload_from_file() {
	return true;
}
void SafeGDScript::_placeholder_erased(void *p_placeholder) {
	// The engine owns and frees the native instance through ScriptInstanceInfo's
	// free callback. This hook only removes stale bookkeeping if it arrives first.
	placeholders.erase(static_cast<SafeGDScriptPlaceholderInstance *>(p_placeholder));
}
// Scoped by run_tests(): a test needs the live instance the editor would
// otherwise refuse a non-@tool script. Thread-local, RAII-scoped, and the only
// place the script answers set_script() differently than it normally would.
static thread_local bool instantiating_for_tests = false;

bool SafeGDScript::is_instantiating_for_tests() {
	return instantiating_for_tests;
}

bool SafeGDScript::_can_instantiate() const {
	bool is_trait = false;
	scan_class_header(source_code, nullptr, nullptr, &is_trait);
	if (is_trait) return false;
	// GDScript: in the editor only a @tool script gets a live instance; the
	// rest get a placeholder (Object::set_script). is_editor_hint() stands in
	// for ScriptServer::is_scripting_enabled(), which GDExtension cannot read.
	if (!tool_script && !instantiating_for_tests && Engine::get_singleton()->is_editor_hint()) {
		return false;
	}
	return !_is_abstract() && _is_valid();
}
static String script_class_path(const String &p_class_name) {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr || p_class_name.is_empty()) {
		return String();
	}
	const TypedArray<Dictionary> classes = settings->get_global_class_list();
	for (int i = 0; i < classes.size(); i++) {
		const Dictionary entry = classes[i];
		if (String(entry.get("class", String())) == p_class_name) {
			return entry.get("path", String());
		}
	}
	return String();
}

// Godot resolves a quoted script path from the file that contains the
// declaration.  Keeping this on the host is important for byte-exact converted
// projects: their .sgd peers intentionally still say `extends "base.gd"`.
static String resolve_script_path(const String &p_reference, const String &p_source_path) {
	if (p_reference.is_empty() || p_reference.begins_with("res://") ||
			p_reference.begins_with("user://") || p_reference.is_absolute_path()) {
		return p_reference;
	}
	if (p_source_path.is_empty()) {
		return p_reference;
	}
	return p_source_path.get_base_dir().path_join(p_reference).simplify_path();
}

static bool looks_like_script_path(const String &p_reference) {
	return p_reference.contains("/") || p_reference.begins_with(".") ||
			p_reference.ends_with(".gd") || p_reference.ends_with(".sgd");
}

// mtime has 1s resolution; length catches same-second rewrites.
static uint64_t file_stamp(const String &p_path) {
	uint64_t length = 0;
	const Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_valid()) {
		length = file->get_length();
	}
	return FileAccess::get_modified_time(p_path) * 1000003ull + length;
}

Ref<Script> SafeGDScript::_get_base_script() const {
	if (base_script_resolved) {
		return base_script;
	}
	base_script_resolved = true;
	if (base_class.is_empty() || base_class == native_base_class) {
		return base_script;
	}
	const String path = base_is_path
			? resolve_script_path(base_class, this->path)
			: script_class_path(base_class);
	if (path.is_empty() || !FileAccess::file_exists(path)) {
		return base_script;
	}
	base_script = ResourceLoader::get_singleton()->load(path, "Script");
	return base_script;
}
StringName SafeGDScript::_get_global_name() const {
	if (!this->class_name.is_empty()) {
		return StringName(this->class_name);
	}
	// Built-in scripts share no file path; returning a name would collide.
	if (is_built_in()) {
		return StringName();
	}
	return PathToGlobalName(this->path);
}
bool SafeGDScript::_inherits_script(const Ref<Script> &p_script) const {
	if (p_script.is_null()) {
		return false;
	}
	// The script itself counts, as it does for GDScript: typed containers
	// validate every element with inherits_script(), so a script that answers
	// false for itself cannot go into an Array of its own class_name.
	if (p_script.ptr() == static_cast<const Script *>(this)) {
		return true;
	}
	Ref<Script> base = _get_base_script();
	for (int depth = 0; base.is_valid() && depth < 64; depth++) {
		if (base == p_script) {
			return true;
		}
		base = base->get_base_script();
	}
	return false;
}
StringName SafeGDScript::_get_instance_base_type() const {
	if (!this->native_base_class.is_empty() && !this->native_base_is_path &&
			ClassDB::class_exists(StringName(this->native_base_class))) {
		return StringName(this->native_base_class);
	}
	return StringName("Sandbox");
}
void *SafeGDScript::_instance_create(Object *p_for_object) const {
	bool is_trait = false;
	String declared_name;
	scan_class_header(source_code, &declared_name, nullptr, &is_trait);
	if (is_trait) {
		ERR_PRINT("SafeGDScript: '" + (declared_name.is_empty() ? path : declared_name) +
				"' is a trait and cannot be attached to an object.");
		return nullptr;
	}
	// Wrong owner type → every unresolved bare name silently answers null.
	if (p_for_object != nullptr && !this->native_base_class.is_empty() && !this->native_base_is_path) {
		const StringName base(this->native_base_class);
		const StringName owner_class = p_for_object->get_class();
		if (ClassDB::class_exists(base) && !ClassDB::is_parent_class(owner_class, base)) {
			ERR_PRINT("SafeGDScript: script inherits from native type '" + this->native_base_class +
					"', so it can't be assigned to an object of type '" + String(owner_class) + "'.");
			return nullptr;
		}
	}
	SafeGDScriptInstance *instance = memnew(SafeGDScriptInstance(p_for_object, Ref<SafeGDScript>(this)));
	instances.insert(instance);
	return ScriptInstanceExtension::create_native_instance(instance);
}
void *SafeGDScript::_placeholder_instance_create(Object *p_for_object) const {
	SafeGDScriptPlaceholderInstance *placeholder = memnew(
			SafeGDScriptPlaceholderInstance(p_for_object, Ref<SafeGDScript>(this)));
	placeholders.insert(placeholder);
	return ScriptInstanceExtension::create_native_instance(placeholder);
}
bool SafeGDScript::_instance_has(Object *p_object) const {
	for (SafeGDScriptInstance *instance : instances) {
		if (instance != nullptr && instance->get_owner() == p_object) {
			return true;
		}
	}
	for (SafeGDScriptPlaceholderInstance *placeholder : placeholders) {
		if (placeholder != nullptr && placeholder->get_owner() == p_object) {
			return true;
		}
	}
	return false;
}
bool SafeGDScript::_has_source_code() const {
	return true;
}
String SafeGDScript::_get_source_code() const {
	return source_code;
}
void SafeGDScript::_set_source_code(const String &p_code) {
	source_code = p_code;
	// Keep the immediate compile expected by Resource/Script callers, but use
	// the soft transaction so editing a live resource does not discard state.
	source_compile_succeeded = compile_source_to_elf(this->profiled_build, this->debug_build,
			ReloadPolicy::KEEP_STATE);
	source_compile_pending_reload = source_compile_succeeded;
}
Error SafeGDScript::_reload(bool p_keep_state) {
	if (p_keep_state && source_compile_pending_reload) {
		source_compile_pending_reload = false;
		return source_compile_succeeded ? Error::OK : Error::ERR_PARSE_ERROR;
	}
	source_compile_pending_reload = false;
	return compile_source_to_elf(this->profiled_build, this->debug_build,
			p_keep_state ? ReloadPolicy::KEEP_STATE : ReloadPolicy::DISCARD_STATE)
			? Error::OK : Error::ERR_PARSE_ERROR;
}

// STORAGE without EDITOR: serialised into .tscn, hidden from inspector.
static const StringName &script_source_property() {
	static const StringName name("script/source");
	return name;
}
void SafeGDScript::_get_property_list(List<PropertyInfo> *p_list) const {
	p_list->push_back(PropertyInfo(Variant::STRING, script_source_property(),
			PROPERTY_HINT_NONE, String(), PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL));
}
bool SafeGDScript::_get(const StringName &p_name, Variant &r_ret) const {
	if (p_name == script_source_property()) {
		r_ret = this->source_code;
		return true;
	}
	return false;
}
bool SafeGDScript::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == script_source_property()) {
		_set_source_code(p_value);
		return true;
	}
	return false;
}
StringName SafeGDScript::_get_doc_class_name() const {
	// Help-page key; must match the global name the editor indexes.
	const StringName global = _get_global_name();
	if (!String(global).is_empty()) {
		return global;
	}
	// Built-in scripts have no global name; GDScript keys their pages by the
	// quoted path, and DocTools::add_doc() refuses an empty name.
	const String resource_path = this->path.is_empty() ? get_path() : this->path;
	return StringName("\"" + resource_path.trim_prefix("res://") + "\"");
}

// Documentation types are names, not Variant::Type: untyped is "Variant", and
// no return type at all is "void" -- never emitted here, since every exported
// function returns a Variant.
static String doc_type_name(const godot::PropertyInfo &p_info) {
	if (p_info.type == Variant::Type::NIL) {
		return (p_info.usage & PROPERTY_USAGE_NIL_IS_VARIANT) ? String("Variant") : String("void");
	}
	return Variant::get_type_name(p_info.type);
}

static String signature_doc_type(int32_t p_type, const std::string &p_class_name,
		bool p_return = false) {
	if (!p_class_name.empty()) {
		return String::utf8(p_class_name.c_str(), p_class_name.size());
	}
	if (p_type == gdscript::FunctionParameter::ANY_TYPE) return "Variant";
	if (p_return && p_type == int32_t(Variant::NIL)) return "void";
	return Variant::get_type_name(variant_type_or_nil(p_type));
}

static void apply_documentation_tags(Dictionary &r_doc, const String &p_description) {
	PackedStringArray kept;
	for (const String &raw : p_description.split("\n")) {
		const String line = raw.strip_edges();
		if (line.begins_with("@deprecated")) {
			r_doc["is_deprecated"] = true;
			r_doc["deprecated_message"] = line.trim_prefix("@deprecated").strip_edges();
		} else if (line.begins_with("@experimental")) {
			r_doc["is_experimental"] = true;
			r_doc["experimental_message"] = line.trim_prefix("@experimental").strip_edges();
		} else {
			kept.push_back(raw);
		}
	}
	r_doc["description"] = String("\n").join(kept);
}

TypedArray<Dictionary> SafeGDScript::_get_documentation() const {
	// One page listing the exported functions. Keys are what
	// DocData::ClassDoc::from_dict() reads back; an unrecognised key is dropped
	// silently, yielding an empty page.
	String doc_name = String(_get_doc_class_name());
	Dictionary class_doc;
	class_doc["name"] = doc_name;
	String inherits = String(_get_instance_base_type());
	{
		const Ref<Script> base = _get_base_script();
		const String base_doc = EditorSymbolResolver::doc_name_of_script(base);
		if (!base_doc.is_empty()) {
			inherits = base_doc;
		}
	}
	class_doc["inherits"] = inherits;
	class_doc["is_script_doc"] = true;
	class_doc["script_path"] = path;

	Array method_docs;
	for (const godot::MethodInfo &method_info : methods_info) {
		Dictionary method_doc;
		method_doc["name"] = method_info.name;
		method_doc["return_type"] = doc_type_name(method_info.return_val);

		// Godot convention: defaults cover the trailing N arguments.
		const int64_t first_default = int64_t(method_info.arguments.size()) -
				int64_t(method_info.default_arguments.size());
		Array argument_docs;
		for (int64_t i = 0; i < int64_t(method_info.arguments.size()); i++) {
			const godot::PropertyInfo &argument = method_info.arguments[i];
			Dictionary argument_doc;
			argument_doc["name"] = argument.name;
			argument_doc["type"] = doc_type_name(argument);
			if (i >= first_default) {
				// Source spelling: "x" stays quoted, 2.5 stays 2.5.
				argument_doc["default_value"] = UtilityFunctions::var_to_str(
						method_info.default_arguments[i - first_default]);
			}
			argument_docs.push_back(argument_doc);
		}
		method_doc["arguments"] = argument_docs;

		if (const MethodDocumentation *documentation = methods_doc.getptr(method_info.name)) {
			apply_documentation_tags(method_doc, documentation->description);
		}
		method_docs.push_back(method_doc);
	}
	class_doc["methods"] = method_docs;

	Array signal_docs;
	for (const godot::MethodInfo &signal_info : signals_info) {
		Dictionary signal_doc;
		signal_doc["name"] = signal_info.name;
		Array argument_docs;
		for (const godot::PropertyInfo &argument : signal_info.arguments) {
			Dictionary argument_doc;
			argument_doc["name"] = argument.name;
			argument_doc["type"] = doc_type_name(argument);
			argument_docs.push_back(argument_doc);
		}
		signal_doc["arguments"] = argument_docs;
		if (const MethodDocumentation *documentation = methods_doc.getptr(signal_info.name)) {
			apply_documentation_tags(signal_doc, documentation->description);
		}
		signal_docs.push_back(signal_doc);
	}
	class_doc["signals"] = signal_docs;

	// Own enum types document as int.
	HashSet<String> own_enums;
	for (const gdscript::SourceDeclaration &declaration : source_model.declarations) {
		if (declaration.parent < 0 && declaration.kind == gdscript::DeclarationKind::ENUM) {
			own_enums.insert(String::utf8(declaration.name.c_str(), declaration.name.size()));
		}
	}

	Array property_docs;
	Array constant_docs;
	Dictionary enum_docs;
	std::vector<const gdscript::SourceDeclaration *> nested_declarations;
	for (const gdscript::SourceDeclaration &declaration : source_model.declarations) {
		if (declaration.parent >= 0) continue;
		const String name = String::utf8(declaration.name.c_str(), declaration.name.size());
		const String description = String::utf8(declaration.documentation.c_str(), declaration.documentation.size());
		if (declaration.kind == gdscript::DeclarationKind::CLASS) {
			apply_documentation_tags(class_doc, description);
			const String clean_description = class_doc.get("description", String());
			const int paragraph = clean_description.find("\n\n");
			class_doc["brief_description"] = paragraph < 0 ? clean_description :
					clean_description.substr(0, paragraph);
		} else if (declaration.kind == gdscript::DeclarationKind::VARIABLE) {
			Dictionary doc;
			doc["name"] = name;
			const String declared = String::utf8(declaration.declared_type.c_str(),
					declaration.declared_type.size());
			if (own_enums.has(declared)) {
				doc["type"] = "int";
				doc["enumeration"] = declared;
			} else if (!declared.is_empty()) {
				doc["type"] = declared;
			} else {
				const String resolved = String::utf8(declaration.resolved_type.c_str(),
						declaration.resolved_type.size());
				doc["type"] = resolved.is_empty() ? String("Variant") : resolved;
			}
			if (!declaration.setter.empty()) {
				doc["setter"] = String::utf8(declaration.setter.c_str(), declaration.setter.size());
			}
			if (!declaration.getter.empty()) {
				doc["getter"] = String::utf8(declaration.getter.c_str(), declaration.getter.size());
			}
			Variant default_value;
			if (property_default(StringName(name), default_value)) {
				doc["default_value"] = UtilityFunctions::var_to_str(default_value);
			} else if (!declaration.initializer_text.empty()) {
				doc["default_value"] = String::utf8(declaration.initializer_text.c_str(),
						declaration.initializer_text.size());
			}
			apply_documentation_tags(doc, description);
			property_docs.push_back(doc);
		} else if (declaration.kind == gdscript::DeclarationKind::CONSTANT) {
			Dictionary doc;
			doc["name"] = name;
			if (const Variant *value = constants.getptr(StringName(name))) {
				doc["value"] = UtilityFunctions::var_to_str(*value);
				doc["is_value_valid"] = true;
			} else if (!declaration.initializer_text.empty()) {
				doc["value"] = String::utf8(declaration.initializer_text.c_str(),
						declaration.initializer_text.size());
				doc["is_value_valid"] = true;
			}
			apply_documentation_tags(doc, description);
			constant_docs.push_back(doc);
		} else if (declaration.kind == gdscript::DeclarationKind::ENUM) {
			// Help viewer groups members by their parent enum.
			Dictionary doc;
			apply_documentation_tags(doc, description);
			enum_docs[name] = doc;
			for (const gdscript::SourceEnumMember &member : declaration.enum_members) {
				Dictionary value;
				value["name"] = String::utf8(member.name.c_str(), member.name.size());
				value["value"] = itos(member.value);
				value["is_value_valid"] = true;
				value["type"] = "int";
				value["enumeration"] = name;
				constant_docs.push_back(value);
			}
		} else if (declaration.kind == gdscript::DeclarationKind::NESTED_CLASS) {
			nested_declarations.push_back(&declaration);
		}
	}
	class_doc["properties"] = property_docs;
	class_doc["constants"] = constant_docs;
	class_doc["enums"] = enum_docs;

	TypedArray<Dictionary> documentation;
	documentation.push_back(class_doc);
	for (const gdscript::SourceDeclaration *declaration : nested_declarations) {
		Dictionary nested_doc;
		nested_doc["name"] = doc_name + "." +
				String::utf8(declaration->name.c_str(), declaration->name.size());
		nested_doc["inherits"] = declaration->base_type.empty() ? String("RefCounted") :
				String::utf8(declaration->base_type.c_str(), declaration->base_type.size());
		nested_doc["is_script_doc"] = true;
		nested_doc["script_path"] = path;
		apply_documentation_tags(nested_doc,
				String::utf8(declaration->documentation.c_str(), declaration->documentation.size()));
		Array methods;
		Array properties;
		Array signals;
		for (int32_t child : declaration->children) {
			if (child < 0 || size_t(child) >= source_model.declarations.size()) continue;
			const gdscript::SourceDeclaration &member = source_model.declarations[size_t(child)];
			Dictionary member_doc;
			member_doc["name"] = String::utf8(member.name.c_str(), member.name.size());
			apply_documentation_tags(member_doc,
					String::utf8(member.documentation.c_str(), member.documentation.size()));
			switch (member.kind) {
				case gdscript::DeclarationKind::FUNCTION: {
					member_doc["return_type"] = member.return_type.empty() ? String("Variant") :
							String::utf8(member.return_type.c_str(), member.return_type.size());
					Array arguments;
					for (const gdscript::SourceParameter &parameter : member.parameters) {
						Dictionary argument;
						argument["name"] = String::utf8(parameter.name.c_str(), parameter.name.size());
						argument["type"] = parameter.declared_type.empty() ? String("Variant") :
								String::utf8(parameter.declared_type.c_str(), parameter.declared_type.size());
						if (!parameter.default_text.empty()) {
							argument["default_value"] = String::utf8(parameter.default_text.c_str(),
									parameter.default_text.size());
						}
						arguments.push_back(argument);
					}
					member_doc["arguments"] = arguments;
					methods.push_back(member_doc);
					break;
				}
				case gdscript::DeclarationKind::VARIABLE: {
					const String declared = member.declared_type.empty() ?
							String::utf8(member.resolved_type.c_str(), member.resolved_type.size()) :
							String::utf8(member.declared_type.c_str(), member.declared_type.size());
					member_doc["type"] = declared.is_empty() ? String("Variant") : declared;
					if (!member.initializer_text.empty()) {
						member_doc["default_value"] = String::utf8(member.initializer_text.c_str(),
								member.initializer_text.size());
					}
					properties.push_back(member_doc);
					break;
				}
				case gdscript::DeclarationKind::SIGNAL:
					signals.push_back(member_doc);
					break;
				default:
					break;
			}
		}
		nested_doc["methods"] = methods;
		nested_doc["properties"] = properties;
		nested_doc["signals"] = signals;
		documentation.push_back(nested_doc);
	}
	for (const KeyValue<StringName, Ref<SafeGDScriptClass>> &entry : nested_classes) {
		const SafeGDScriptClass *nested = entry.value.ptr();
		if (nested == nullptr || !nested->get_is_struct()) continue;
		Dictionary struct_doc;
		struct_doc["name"] = doc_name + "." + String(entry.key);
		struct_doc["is_script_doc"] = true;
		struct_doc["script_path"] = path;
		apply_documentation_tags(struct_doc, nested->get_description());
		Array properties;
		for (const gdscript::ClassField &field : nested->get_fields()) {
			Dictionary property;
			property["name"] = String::utf8(field.name.c_str(), field.name.size());
			property["type"] = field.class_name.empty()
				? signature_doc_type(field.type, field.class_name)
				: String::utf8(field.class_name.c_str(), field.class_name.size());
			apply_documentation_tags(property,
				String::utf8(field.description.c_str(), field.description.size()));
			properties.push_back(property);
		}
		struct_doc["properties"] = properties;
		documentation.push_back(struct_doc);
	}
	for (const gdscript::ClassSignature &signature : trait_signatures) {
		Dictionary trait_doc;
		trait_doc["name"] = doc_name + "." +
				String::utf8(signature.name.c_str(), signature.name.size());
		trait_doc["is_script_doc"] = true;
		trait_doc["script_path"] = path;
		apply_documentation_tags(trait_doc,
				String::utf8(signature.description.c_str(), signature.description.size()));
		Array methods;
		for (const gdscript::FunctionSignature &method : signature.trait_methods) {
			Dictionary method_doc;
			method_doc["name"] = String::utf8(method.name.c_str(), method.name.size());
			method_doc["return_type"] = signature_doc_type(method.return_type,
					method.return_class_name, true);
			Array arguments;
			for (const gdscript::FunctionParameter &parameter : method.parameters) {
				Dictionary argument;
				argument["name"] = String::utf8(parameter.name.c_str(), parameter.name.size());
				argument["type"] = signature_doc_type(parameter.type, parameter.class_name);
				arguments.push_back(argument);
			}
			method_doc["arguments"] = arguments;
			apply_documentation_tags(method_doc,
					String::utf8(method.description.c_str(), method.description.size()));
			methods.push_back(method_doc);
		}
		trait_doc["methods"] = methods;
		Array properties;
		for (const gdscript::ClassField &field : signature.trait_fields) {
			Dictionary property;
			property["name"] = String::utf8(field.name.c_str(), field.name.size());
			property["type"] = signature_doc_type(field.type, field.class_name);
			apply_documentation_tags(property,
					String::utf8(field.description.c_str(), field.description.size()));
			properties.push_back(property);
		}
		trait_doc["properties"] = properties;
		Array signals;
		for (const gdscript::FunctionSignature &signal : signature.trait_signals) {
			Dictionary signal_doc;
			signal_doc["name"] = String::utf8(signal.name.c_str(), signal.name.size());
			signals.push_back(signal_doc);
		}
		trait_doc["signals"] = signals;
		documentation.push_back(trait_doc);
	}
	return documentation;
}
String SafeGDScript::_get_class_icon_path() const {
	return class_icon_path;
}
const godot::MethodInfo *SafeGDScript::find_method_info(const StringName &p_method) const {
	for (const godot::MethodInfo &method_info : methods_info) {
		if (method_info.name == p_method) {
			return &method_info;
		}
	}
	return nullptr;
}
bool SafeGDScript::_has_method(const StringName &p_method) const {
	if (p_method == StringName("_init"))
		return true;
	for (const godot::MethodInfo &method_info : methods_info) {
		if (method_info.name == p_method) {
			//WARN_PRINT("SafeGDScript::_has_method: found method " + p_method);
			return true;
		}
	}
	return false;
}
bool SafeGDScript::_has_static_method(const StringName &p_method) const {
	for (const gdscript::FunctionSignature &signature : signatures) {
		if (String::utf8(signature.name.c_str(), signature.name.size()) == p_method &&
				signature.is_static) {
			return true;
		}
	}
	return false;
}
Variant SafeGDScript::_get_script_method_argument_count(const StringName &p_method) const {
	if (const godot::MethodInfo *method = find_method_info(p_method)) {
		if ((method->flags & METHOD_FLAG_VARARG) == 0) {
			return int64_t(method->arguments.size());
		}
	}
	return Variant();
}
Dictionary SafeGDScript::_get_method_info(const StringName &p_method) const {
	if (const godot::MethodInfo *method_info = find_method_info(p_method)) {
		// Analyzer reads _init flags verbatim; force STATIC or Class.new() is refused.
		if (p_method == StringName("_init")) {
			godot::MethodInfo constructor = *method_info;
			constructor.flags |= METHOD_FLAG_STATIC;
			return method_dict(constructor);
		}
		return method_dict(*method_info);
	}
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScript::_get_method_info: Method " + String(p_method) + " not found.");
	}
	return Dictionary();
}
bool SafeGDScript::_is_tool() const {
	return tool_script;
}
bool SafeGDScript::_is_valid() const {
	return !elf_data.is_empty();
}
bool SafeGDScript::_is_abstract() const {
	return abstract_script;
}
ScriptLanguage *SafeGDScript::_get_language() const {
	return SafeGDScriptLanguage::get_singleton();
}
bool SafeGDScript::_has_script_signal(const StringName &p_signal) const {
	for (const godot::MethodInfo &signal_info : signals_info) {
		if (signal_info.name == p_signal) {
			return true;
		}
	}
	return false;
}
TypedArray<Dictionary> SafeGDScript::_get_script_signal_list() const {
	TypedArray<Dictionary> signals_array;
	for (const godot::MethodInfo &signal_info : signals_info) {
		signals_array.push_back(method_dict(signal_info, String()));
	}
	return signals_array;
}
bool SafeGDScript::_has_property_default_value(const StringName &p_property) const {
	return property_defaults.has(p_property);
}
Variant SafeGDScript::_get_property_default_value(const StringName &p_property) const {
	Variant value;
	property_default(p_property, value);
	return value;
}
void SafeGDScript::_update_exports() {
	for (SafeGDScriptPlaceholderInstance *placeholder : placeholders) {
		if (placeholder != nullptr) {
			placeholder->properties_changed(previous_properties_for_update, properties);
		}
	}
	previous_properties_for_update.clear();
}
TypedArray<Dictionary> SafeGDScript::_get_script_method_list() const {
	TypedArray<Dictionary> functions_array;
	for (const godot::MethodInfo &method_info : methods_info) {
		functions_array.push_back(method_dict(method_info));
	}
	return functions_array;
}
TypedArray<Dictionary> SafeGDScript::_get_script_property_list() const {
	TypedArray<Dictionary> result;
	for (const gdscript::PropertySignature &signature : properties) {
		if (signature.is_member) {
			result.push_back(property_dict(property_info(signature)));
		}
	}
	return result;
}
int32_t SafeGDScript::_get_member_line(const StringName &p_member) const {
	// 1-based, as the editor counts lines. Not-found is -1, not 0: a caller opens
	// the script at the returned line, so 0 would jump to the top of the wrong
	// file instead of letting the caller look elsewhere.
	if (const MethodDocumentation *documentation = methods_doc.getptr(p_member)) {
		return documentation->line;
	}
	if (const gdscript::PropertySignature *property = find_property_signature(p_member)) {
		return int32_t(property->declaration_line);
	}
	for (const gdscript::SourceDeclaration &declaration : source_model.declarations) {
		if (String::utf8(declaration.name.c_str(), declaration.name.size()) == p_member) {
			return int32_t(declaration.declaration.start_line);
		}
	}
	return -1;
}
Dictionary SafeGDScript::_get_constants() const {
	Dictionary out;
	for (const KeyValue<StringName, Variant> &constant : this->constants) {
		out[constant.key] = constant.value;
	}
	return out;
}
TypedArray<StringName> SafeGDScript::_get_members() const {
	TypedArray<StringName> members;
	for (const gdscript::PropertySignature &property : properties) {
		if (property.is_member) {
			members.push_back(StringName(String::utf8(property.name.c_str(), property.name.size())));
		}
	}
	return members;
}
bool SafeGDScript::_is_placeholder_fallback_enabled() const {
	return true;
}

const gdscript::PropertySignature *SafeGDScript::find_property_signature(
		const StringName &p_name) const {
	for (const gdscript::PropertySignature &property : properties) {
		if (String::utf8(property.name.c_str(), property.name.size()) == p_name) {
			return &property;
		}
	}
	return nullptr;
}

PropertyInfo SafeGDScript::property_info(const gdscript::PropertySignature &p_signature) const {
	PropertyInfo info;
	info.name = String::utf8(p_signature.name.c_str(), p_signature.name.size());
	info.type = p_signature.type < 0 ? Variant::NIL : Variant::Type(p_signature.type);
	if ((info.type == Variant::OBJECT || info.type == Variant::DICTIONARY) &&
			!p_signature.class_name.empty()) {
		info.class_name = String::utf8(p_signature.class_name.c_str(), p_signature.class_name.size());
	}
	info.hint = PropertyHint(p_signature.hint);
	info.hint_string = String::utf8(p_signature.hint_string.c_str(), p_signature.hint_string.size());
	info.usage = p_signature.usage;
	if (p_signature.type < 0) {
		info.usage |= PROPERTY_USAGE_NIL_IS_VARIANT;
	} else {
		info.usage &= ~uint32_t(PROPERTY_USAGE_NIL_IS_VARIANT);
	}
	return info;
}

bool SafeGDScript::property_default(const StringName &p_name, Variant &r_value) const {
	if (const Variant *value = property_defaults.getptr(p_name)) {
		r_value = *value;
		return true;
	}
	return false;
}

static Variant property_default_value(const gdscript::PropertySignature &p_property) {
	using Kind = gdscript::PropertyDefaultKind;
	switch (p_property.default_kind) {
		case Kind::NIL:
			return Variant();
		case Kind::INT:
			return std::get<int64_t>(p_property.default_value);
		case Kind::FLOAT:
			return std::get<double>(p_property.default_value);
		case Kind::BOOL:
			return std::get<bool>(p_property.default_value);
		case Kind::STRING: {
			const std::string &text = std::get<std::string>(p_property.default_value);
			return String::utf8(text.c_str(), text.size());
		}
		case Kind::EMPTY_ARRAY:
			return Array();
		case Kind::EMPTY_DICTIONARY:
			return Dictionary();
		case Kind::NONE:
			return Variant();
	}
	return Variant();
}

Variant SafeGDScript::_get_rpc_config() const {
	const Sandbox *sandbox = sandbox_for_safegdscript(this);
	// RPC is an external, remotely-triggered entry into the guest. Publishing it
	// for a sandbox with any restriction would bypass the host's intended trust
	// boundary, so restricted instances expose no RPC methods at all.
	if (sandbox == nullptr || !sandbox->is_fully_unrestricted()) {
		return Variant();
	}
	return rpc_config;
}

static std::unordered_set<SafeGDScript *> live_scripts;

SafeGDScript::SafeGDScript() {
	live_scripts.insert(this);
	source_code = R"GDScript(# SafeGDScript example

func somefunction():
	var counter = 0
	while counter < 10:
		counter += 1
	return counter

)GDScript";
}
SafeGDScript::~SafeGDScript() {
	live_scripts.erase(this);
}

SafeGDScriptInstance *SafeGDScript::get_safegdscript_script_instance() const {
	return instances.is_empty() ? nullptr : *instances.begin();
}

Object *SafeGDScript::owner_for_instance_base(uint64_t p_instance_base) const {
	if (p_instance_base == 0) return nullptr;
	for (SafeGDScriptInstance *instance : instances) {
		if (instance != nullptr && instance->instance_base == p_instance_base) {
			return instance->get_owner();
		}
	}
	return nullptr;
}

void SafeGDScript::set_path(const String &p_path) {
	if (p_path.is_empty()) {
		WARN_PRINT("SafeGDScript::set_path: Empty resource path.");
		return;
	}
	this->path = p_path;
	// Built-in path: no file to read; source arrived via _set().
	if (!is_built_in()) {
		this->source_code = FileAccess::get_file_as_string(p_path);
	}
	this->compile_source_to_elf();
}

bool SafeGDScript::compile_source_to_elf(bool p_profiling, bool p_debug,
		ReloadPolicy p_reload_policy, bool p_shipping) {
	// Refuse rebuild while stopped at a breakpoint in this program.
	if (safegdscript_stopped_script() == this) {
		return fail_compile(this->path + " is stopped at a breakpoint and cannot be "
											"rebuilt until it continues.");
	}
	if (this->source_code.is_empty()) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("SafeGDScript::compile_source_to_elf: No source code to compile.");
		}
		this->last_error = String();
		return false;
	}

	struct InstanceSnapshot {
		SafeGDScriptInstance *instance = nullptr;
		HashMap<StringName, Variant> values;
	};
	std::vector<InstanceSnapshot> snapshots;
	if (p_reload_policy == ReloadPolicy::KEEP_STATE) {
		snapshots.reserve(instances.size());
		for (SafeGDScriptInstance *instance : instances) {
			if (instance == nullptr) {
				continue;
			}
			InstanceSnapshot snapshot;
			snapshot.instance = instance;
			for (const gdscript::PropertySignature &property : properties) {
				if (!property.is_member) {
					continue;
				}
				const StringName name(String::utf8(property.name.c_str(), property.name.size()));
				Variant value;
				if (instance->get(name, value)) {
					snapshot.values.insert(name, value);
				}
			}
			snapshots.push_back(std::move(snapshot));
		}
	}

	const bool restricted = this->class_access_restricted();
	GDScriptCompilerBackend &compiler = gdscript_compiler::backend_for(restricted);
	if (!compiler.available()) {
		return fail_compile(String("failed to initialize the ") + compiler.name() +
				" GDScript compiler");
	}

	const bool profiling = !p_shipping && p_profiling && compiler.can_build_profiled();
	if (p_profiling && !profiling) {
		ERR_PRINT("SafeGDScript: the GDScript compiler ELF is too old to build a profiled program.");
	}
	// Ours plus the editor's, for this build only: the editor owns its lines and
	// may clear them again, so they never enter the project-owned set.
	const PackedInt32Array build_breakpoints = p_shipping ? PackedInt32Array() : get_breakpoints();
	// Non-empty breakpoints force a debug build.
	const bool wants_debug = p_debug || !build_breakpoints.is_empty();
	// Profiling and debug are mutually exclusive instrumentations.
	const bool debug = !profiling && wants_debug && compiler.can_build_debug();
	if (wants_debug && !profiling && !debug) {
		ERR_PRINT("SafeGDScript: the GDScript compiler ELF is too old to build a debuggable program.");
	}

	PackedStringArray new_base_paths;
	Vector<uint64_t> new_base_stamps;
	PackedStringArray base_sources;
	if (!restricted) {
		String chain_error;
		base_sources = resolve_base_sources(this->source_code, this->path, &chain_error);
		if (!chain_error.is_empty()) {
			return fail_compile(chain_error);
		}
		for (int64_t i = 0; i + 1 < base_sources.size(); i += 3) {
			new_base_paths.push_back(base_sources[i + 1]);
			new_base_stamps.push_back(file_stamp(base_sources[i + 1]));
		}
	}
	// A shipping build carries no test cases: they never reach codegen.
	gdscript_compiler::prepare(compiler, restricted, base_sources, this->path, !p_shipping);

	GDScriptCompilerBackend::BuildOptions options;
	options.profiling = profiling;
	options.debug = debug;
	options.breakpoints = build_breakpoints;
	PackedByteArray new_elf;
	{
		SGD_TIME_COMPILE();
		new_elf = compiler.compile(this->source_code, options);
	}
	if (new_elf.is_empty()) {
		return fail_compile(compiler.error_message());
	}
	// Fetch every new metadata table before touching the last working program.
	// Sandboxed backends decode untrusted blobs here; older compiler ELFs simply
	// return an empty property table.
	std::vector<gdscript::PropertySignature> new_properties = compiler.property_signatures();
	std::vector<gdscript::DebugVariableRecord> new_debug_variables = compiler.debug_variables();
	// Force validation of every compiler-owned blob before committing the ELF.
	// Older guests simply lack the optional exports and remain valid.
	(void)compiler.function_signatures();
	(void)compiler.signal_signatures();
	(void)compiler.rpc_configs();
	(void)compiler.test_signatures();
	(void)compiler.class_signatures();
	(void)compiler.script_constants();
	(void)compiler.line_table();
	gdscript::SourceModel new_source_model;
	if (compiler.can_analyze()) {
		GDScriptCompilerBackend::AnalysisRequest request;
		request.source = source_code;
		request.path = path;
		request.flags = gdscript::ANALYZE_DECLARATIONS | gdscript::ANALYZE_DOCUMENTATION;
		const PackedByteArray model_bytes = compiler.analyze(request);
		if (!model_bytes.is_empty()) {
			std::string model_error;
			if (!gdscript::decode_source_model(model_bytes.ptr(), size_t(model_bytes.size()),
					new_source_model, model_error)) {
				return fail_compile("the compiler returned an invalid source model: " +
						String::utf8(model_error.c_str(), model_error.size()));
			}
		}
	}
	if (!compiler.metadata_valid()) {
		return fail_compile("the compiler returned malformed metadata");
	}
	HashMap<StringName, Variant> new_defaults;
	for (const gdscript::PropertySignature &property : new_properties) {
		if (property.default_kind == gdscript::PropertyDefaultKind::NONE) {
			continue;
		}
		const StringName name(String::utf8(property.name.c_str(), property.name.size()));
		new_defaults.insert(name, property_default_value(property));
	}

	this->previous_properties_for_update = this->properties;
	this->elf_data = new_elf;
	this->properties = std::move(new_properties);
	this->debug_variables = std::move(new_debug_variables);
	this->source_model = std::move(new_source_model);
	this->property_defaults = std::move(new_defaults);
	this->base_paths = std::move(new_base_paths);
	this->base_stamps = std::move(new_base_stamps);
	if (sgd_timing::enabled()) {
		sgd_timing::totals().elf_bytes += new_elf.size();
	}
	this->compiled_restricted = restricted;
	this->profiled_build = profiling;
	this->debug_build = debug;
	this->last_error = String();
	const GDScriptCompilerBackend::ScriptClass declared = compiler.script_class();
	this->class_name = declared.class_name;
	this->base_class = declared.base_class;
	this->base_is_path = declared.base_is_path;
	this->native_base_class = declared.native_base_class;
	this->native_base_is_path = declared.native_base_is_path;
	this->used_traits.clear();
	for (const std::string &name : compiler.script_uses()) {
		this->used_traits.insert(StringName(String::utf8(name.c_str(), name.size())));
	}
	if (this->native_base_class.is_empty()) {
		this->native_base_class = this->base_class;
		this->native_base_is_path = this->base_is_path;
	}
	this->base_script = Ref<Script>();
	this->base_script_resolved = false;
	bool is_file_trait = false;
	scan_class_header(source_code, nullptr, nullptr, &is_file_trait);
	this->abstract_script = is_file_trait;
	this->class_icon_path = "res://addons/godot_sandbox/SafeGDScript.svg";
	const PackedStringArray metadata_lines = source_code.split("\n");
	for (int64_t i = 0; i < metadata_lines.size(); i++) {
		const String line = metadata_lines[i].strip_edges();
		if (line.begins_with("@icon(")) {
			const int quote = line.find("\"");
			const int end = quote < 0 ? -1 : line.find("\"", quote + 1);
			if (quote >= 0 && end > quote + 1) {
				this->class_icon_path = line.substr(quote + 1, end - quote - 1);
			}
		}
	}

	this->update_methods_info(compiler);
	this->_update_exports();

	if (!p_shipping && Engine::get_singleton()->is_editor_hint() && OS::get_singleton()->has_feature("editor") &&
			SandboxProjectSettings::binary_translation_auto_bake() && !restricted &&
			!profiling && !debug && Sandbox::has_feature_binary_translation()) {
		Sandbox::queue_binary_translation_bake(this->elf_data, 32);
	}

	// One reload for the Sandbox they share: reloading per instance would replace
	// the machine again under the instances that had already taken a record in
	// it. Each takes a fresh one on its next call.
	if (this->static_instance != nullptr) {
		this->static_instance->release_sandbox();
	}
	if (!instances.is_empty()) {
		(*instances.begin())->reset_to(this->elf_data);
	}
	if (p_reload_policy == ReloadPolicy::KEEP_STATE) {
		int skipped = 0;
		for (InstanceSnapshot &snapshot : snapshots) {
			if (snapshot.instance == nullptr || !instances.has(snapshot.instance)) {
				continue;
			}
			for (const KeyValue<StringName, Variant> &entry : snapshot.values) {
				const gdscript::PropertySignature *destination = find_property_signature(entry.key);
				if (destination == nullptr || !destination->is_member) {
					skipped++;
					continue;
				}
				const Variant::Type to = destination->type < 0
						? Variant::NIL : Variant::Type(destination->type);
				if (destination->type >= 0 && entry.value.get_type() != to &&
						!Variant::can_convert(entry.value.get_type(), to)) {
					skipped++;
					continue;
				}
				if (!snapshot.instance->set(entry.key, entry.value)) {
					skipped++;
				}
			}
		}
		if constexpr (VERBOSE_LOGGING) if (skipped > 0) {
			WARN_PRINT("SafeGDScript: skipped " + itos(skipped) +
					" removed, incompatible, or read-only member value(s) while reloading " + path + ".");
		}
	}

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScript::compile_source_to_elf: Successfully compiled " + this->path + " to ELF (" + itos(this->elf_data.size()) + " bytes)");
	}

	return true;
}

// -= Breakpoints =-
//
// Setting/clearing recompiles. Set kept on failed rebuild; refused while stopped.
bool SafeGDScript::refuse_while_stopped() const {
	if (safegdscript_stopped_script() != this) {
		return false;
	}
	ERR_PRINT("SafeGDScript: " + this->path + " is stopped at a breakpoint; continue before "
			  "changing its breakpoints.");
	return true;
}

bool SafeGDScript::set_breakpoint(int32_t p_line, bool p_enabled) {
	if (refuse_while_stopped()) {
		return false;
	}
	if (p_line <= 0) {
		ERR_PRINT("SafeGDScript::set_breakpoint: a source line is 1-based.");
		return false;
	}
	const int64_t at = this->breakpoints.bsearch(p_line, true);
	const bool present = at < this->breakpoints.size() && this->breakpoints[at] == p_line;
	if (present == p_enabled) {
		return true;
	}
	if (p_enabled) {
		this->breakpoints.insert(at, p_line);
	} else {
		this->breakpoints.remove_at(at);
	}
	return compile_source_to_elf();
}

bool SafeGDScript::set_breakpoints(const PackedInt32Array &p_lines) {
	if (refuse_while_stopped()) {
		return false;
	}
	PackedInt32Array wanted;
	for (int i = 0; i < p_lines.size(); i++) {
		const int32_t line = p_lines[i];
		if (line <= 0) {
			continue;
		}
		const int64_t at = wanted.bsearch(line, true);
		if (at >= wanted.size() || wanted[at] != line) {
			wanted.insert(at, line);
		}
	}
	if (wanted == this->breakpoints) {
		return true;
	}
	this->breakpoints = wanted;
	return compile_source_to_elf();
}

bool SafeGDScript::clear_breakpoints() {
	if (refuse_while_stopped()) {
		return false;
	}
	if (this->breakpoints.is_empty()) {
		return true;
	}
	this->breakpoints.clear();
	return compile_source_to_elf();
}

PackedInt32Array SafeGDScript::get_breakpoints() const {
	PackedInt32Array result = this->breakpoints;
	const PackedInt32Array editor = safegdscript_engine_breakpoints(*this);
	for (const int32_t line : editor) {
		const int64_t at = result.bsearch(line, true);
		if (at >= result.size() || result[at] != line) {
			result.insert(at, line);
		}
	}
	return result;
}

void SafeGDScript::update_runtime_breakpoints(const PackedInt32Array &p_lines) {
	// A debug build polls at every executable line and a normal build polls
	// nowhere, so a set appearing or disappearing changes the program, not just
	// the host set. Rebuilding keeps state: this runs from the editor's poll.
	const bool wants_debug = !p_lines.is_empty();
	if (wants_debug != this->debug_build && safegdscript_stopped_script() != this) {
		compile_source_to_elf(this->profiled_build, wants_debug, ReloadPolicy::KEEP_STATE);
	}
	PackedInt32Array live;
	for (const int32_t line : p_lines) {
		bool executable = false;
		for (const gdscript::LineTableEntry &entry : line_table.entries) {
			if (entry.line == uint32_t(line)) { executable = true; break; }
		}
		if (executable) live.push_back(line);
	}
	active_breakpoints = live;
}

PackedInt32Array SafeGDScript::get_active_breakpoints() const {
	return this->active_breakpoints;
}

bool SafeGDScript::is_stopped() {
	return safegdscript_is_stopped();
}

int64_t SafeGDScript::get_stopped_line() {
	return safegdscript_stopped_line();
}

PackedStringArray SafeGDScript::get_stopped_backtrace() {
	return safegdscript_stopped_backtrace();
}

void SafeGDScript::debug_continue() {
	safegdscript_debug_continue();
}

void SafeGDScript::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_breakpoint", "line", "enabled"), &SafeGDScript::set_breakpoint);
	ClassDB::bind_method(D_METHOD("set_breakpoints", "lines"), &SafeGDScript::set_breakpoints);
	ClassDB::bind_method(D_METHOD("clear_breakpoints"), &SafeGDScript::clear_breakpoints);
	ClassDB::bind_method(D_METHOD("get_breakpoints"), &SafeGDScript::get_breakpoints);
	ClassDB::bind_method(D_METHOD("get_active_breakpoints"), &SafeGDScript::get_active_breakpoints);
	ClassDB::bind_method(D_METHOD("is_debug_build"), &SafeGDScript::is_debug_build);
	ClassDB::bind_method(D_METHOD("bake_translation"), &SafeGDScript::bake_translation);
	ClassDB::bind_method(D_METHOD("is_translation_baked"), &SafeGDScript::is_translation_baked);
	ClassDB::bind_method(D_METHOD("get_translation_hash"), &SafeGDScript::get_translation_hash);
	ClassDB::bind_method(D_METHOD("get_content"), &SafeGDScript::get_content);

	ClassDB::bind_method(D_METHOD("get_compile_error"), &SafeGDScript::get_compile_error);
	ClassDB::bind_method(D_METHOD("compile_shipping"), &SafeGDScript::compile_shipping);
	ClassDB::bind_method(D_METHOD("get_test_functions"), &SafeGDScript::get_test_functions);
	ClassDB::bind_method(D_METHOD("get_test_lines"), &SafeGDScript::get_test_lines);
	ClassDB::bind_method(D_METHOD("run_tests", "only"), &SafeGDScript::run_tests,
			DEFVAL(PackedStringArray()));
	ClassDB::bind_method(D_METHOD("uses_trait", "name"), &SafeGDScript::uses_trait);
	// Engine-internal; bound for tests.
	ClassDB::bind_method(D_METHOD("editor_documentation"), &SafeGDScript::editor_documentation);
	ClassDB::bind_method(D_METHOD("editor_member_line", "member"), &SafeGDScript::editor_member_line);

	ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "new", &SafeGDScript::new_instance,
			MethodInfo("new"));

	ClassDB::bind_static_method("SafeGDScript", D_METHOD("is_stopped"), &SafeGDScript::is_stopped);
	ClassDB::bind_static_method("SafeGDScript", D_METHOD("get_stopped_line"), &SafeGDScript::get_stopped_line);
	ClassDB::bind_static_method("SafeGDScript", D_METHOD("get_stopped_backtrace"), &SafeGDScript::get_stopped_backtrace);
	ClassDB::bind_static_method("SafeGDScript", D_METHOD("debug_continue"), &SafeGDScript::debug_continue);
	ClassDB::bind_static_method("SafeGDScript", D_METHOD("poll_base_sources"),
			&SafeGDScript::poll_base_sources);

	// Handler calls debug_continue() to resume; no listeners = no stop.
	ADD_SIGNAL(MethodInfo("breakpoint_hit",
			PropertyInfo(Variant::OBJECT, "script", PROPERTY_HINT_RESOURCE_TYPE, "SafeGDScript"),
			PropertyInfo(Variant::INT, "line")));
}

String SafeGDScript::bake_translation() {
	Sandbox *sandbox = sandbox_for_safegdscript(this);
	return sandbox == nullptr ? String() : sandbox->bake_binary_translation();
}

bool SafeGDScript::is_translation_baked() const {
	const Sandbox *sandbox = sandbox_for_safegdscript(this);
	return sandbox != nullptr && sandbox->is_translation_baked();
}

int64_t SafeGDScript::get_translation_hash() const {
	const Sandbox *sandbox = sandbox_for_safegdscript(this);
	return sandbox == nullptr ? 0 : sandbox->get_translation_hash();
}

Variant SafeGDScript::new_instance(const Variant **p_args, GDExtensionInt p_argcount, GDExtensionCallError &r_error) {
	r_error.error = GDEXTENSION_CALL_OK;
	bool is_trait = false;
	String declared_name;
	scan_class_header(source_code, &declared_name, nullptr, &is_trait);
	if (is_trait) {
		ERR_PRINT("SafeGDScript: '" + (declared_name.is_empty() ? path : declared_name) +
				"' is a trait.");
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	// Declared base, not RefCounted -- guest reaches self through get_tree_base().
	const StringName base_type = _get_instance_base_type();
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	if (!class_db->can_instantiate(base_type)) {
		ERR_PRINT("SafeGDScript: " + this->path + ": cannot construct '" + String(base_type) + "'.");
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	Variant instance = class_db->instantiate(base_type);
	Object *obj = instance.operator Object *();
	if (obj == nullptr) {
		ERR_PRINT("SafeGDScript: " + this->path + ": failed to construct '" + String(base_type) + "'.");
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}

	this->pending_init_args = p_args;
	this->pending_init_argcount = int(p_argcount);
	obj->set_script(Variant(Ref<Script>(this)));
	this->pending_init_args = nullptr;
	this->pending_init_argcount = 0;

	return instance;
}

String SafeGDScript::get_compile_error() const {
	return last_error;
}

bool SafeGDScript::fail_compile(const String &p_message) {
	this->last_error = p_message;
	ERR_PRINT("SafeGDScript: " + this->path + ": " + p_message);
	return false;
}


// -= @test runner =-
//
// A test fails the way any SafeGDScript code fails: assert() throws, the
// Sandbox counts the exception, and the runner reads the counter. Nothing is
// parsed out of the output, and the guest gains no run-time surface.
//
// Each test gets its own script instance, so create_instance_record() lays the
// member defaults down and _init() runs again. Instances of one script share a
// machine, so that costs a record, not a program load. The owner is never added
// to a tree: _ready, _enter_tree and _process do not run, and $Child answers
// null. A test that needs a tree is an integration test and belongs in GUT.
static constexpr int64_t TEST_INSTRUCTION_CAP = 4000; // Millions

static Dictionary test_row(const String &p_name, int32_t p_line, const char *p_status,
		const String &p_message, const String &p_location, uint64_t p_usec) {
	Dictionary row;
	row["name"] = p_name;
	row["line"] = p_line;
	row["status"] = String(p_status);
	row["message"] = p_message;
	row["location"] = p_location;
	row["elapsed_usec"] = int64_t(p_usec);
	return row;
}

static Dictionary test_report(const String &p_path, const Array &p_rows, uint64_t p_usec) {
	int64_t passed = 0;
	int64_t failed = 0;
	int64_t errors = 0;
	for (int64_t i = 0; i < p_rows.size(); i++) {
		const String status = Dictionary(p_rows[i]).get("status", String());
		if (status == "passed") {
			passed++;
		} else if (status == "failed") {
			failed++;
		} else {
			errors++;
		}
	}
	Dictionary report;
	report["path"] = p_path;
	report["passed"] = passed;
	report["failed"] = failed;
	report["errors"] = errors;
	report["elapsed_usec"] = int64_t(p_usec);
	report["tests"] = p_rows;
	return report;
}

// One line per test, then a summary, so a headless run reads without every
// caller formatting the Dictionary itself.
static void print_test_report(const Dictionary &p_report) {
	const Array rows = p_report.get("tests", Array());
	UtilityFunctions::print_rich(String("[b]") + String(p_report.get("path", String())) + "[/b]");
	for (int64_t i = 0; i < rows.size(); i++) {
		const Dictionary row = rows[i];
		const String status = row.get("status", String());
		const String name = row.get("name", String());
		const String message = row.get("message", String());
		const String location = row.get("location", String());
		if (status == "passed") {
			UtilityFunctions::print_rich(String::utf8("  [color=green]\u2713[/color] ") + name);
			continue;
		}
		const String colour = status == "failed" ? String("red") : String("yellow");
		String line = "  [color=" + colour + String::utf8("]\u2717[/color] ") + name +
				" (" + status + ")";
		if (!location.is_empty()) {
			line += " at " + location;
		}
		UtilityFunctions::print_rich(line);
		if (!message.is_empty()) {
			UtilityFunctions::print_rich("      " + message);
		}
	}
	const double ms = double(int64_t(p_report.get("elapsed_usec", int64_t(0)))) / 1000.0;
	UtilityFunctions::print_rich(String("  ") + itos(int64_t(p_report.get("passed", int64_t(0)))) +
			" passed, " + itos(int64_t(p_report.get("failed", int64_t(0)))) + " failed, " +
			itos(int64_t(p_report.get("errors", int64_t(0)))) + " errors in " +
			String::num(ms, 1) + " ms");
}

Dictionary SafeGDScript::run_tests(const PackedStringArray &p_only) {
	const uint64_t run_start = Time::get_singleton()->get_ticks_usec();
	Array rows;

	if (!this->last_error.is_empty()) {
		rows.push_back(test_row("(compile)", 0, "error", this->last_error, this->path, 0));
		const Dictionary report = test_report(this->path, rows,
				Time::get_singleton()->get_ticks_usec() - run_start);
		print_test_report(report);
		return report;
	}

	// The exception counter is per machine, so an inner run would attribute its
	// own failures to whichever test is on the stack.
	Sandbox *shared = sandbox_for_safegdscript(this);
	if (shared != nullptr && shared->is_in_vmcall()) {
		rows.push_back(test_row("(runner)", 0, "error",
				"run_tests() cannot run from inside a guest call", this->path, 0));
		const Dictionary report = test_report(this->path, rows,
				Time::get_singleton()->get_ticks_usec() - run_start);
		print_test_report(report);
		return report;
	}

	// Names the caller asked for that this script does not declare are reported,
	// not skipped: a renamed test would otherwise pass by disappearing.
	PackedStringArray selected;
	for (int64_t i = 0; i < this->test_functions.size(); i++) {
		if (p_only.is_empty() || p_only.has(this->test_functions[i])) {
			selected.push_back(this->test_functions[i]);
		}
	}
	for (int64_t i = 0; i < p_only.size(); i++) {
		if (!this->test_functions.has(p_only[i])) {
			rows.push_back(test_row(p_only[i], 0, "error",
					this->path + " declares no @test function named '" + p_only[i] + "'",
					String(), 0));
		}
	}

	// Holds the shared machine for the whole run: restrictions belong on it
	// before the first instance's _init(), and it must not be rebuilt between
	// tests. A restricted script's base is always Sandbox (structural gate).
	const bool sandbox_existed = shared != nullptr;
	Sandbox *machine = safegdscript_acquire_sandbox(nullptr, Ref<SafeGDScript>(this),
			this->compiled_restricted && !sandbox_existed);
	if (machine != nullptr && !sandbox_existed && machine->get_exceptions() > 0) {
		String message = machine->get_last_exception();
		if (message.is_empty()) {
			message = "the program raised an exception while starting";
		}
		rows.push_back(test_row("(startup)", 0, "error", message,
				machine->get_last_exception_location(), 0));
		safegdscript_release_sandbox(this, nullptr);
		const Dictionary report = test_report(this->path, rows,
				Time::get_singleton()->get_ticks_usec() - run_start);
		print_test_report(report);
		return report;
	}
	const int64_t previous_budget = machine != nullptr ? machine->get_instructions_max() : 0;
	if (machine != nullptr && previous_budget <= 0) {
		// A runaway loop ends as a timeout the runner reports, not as a hung editor.
		machine->set_instructions_max(TEST_INSTRUCTION_CAP);
	}

	const StringName base_type = _get_instance_base_type();
	for (int64_t i = 0; i < selected.size(); i++) {
		const String name = selected[i];
		int32_t line = 0;
		for (int64_t j = 0; j < this->test_functions.size(); j++) {
			if (this->test_functions[j] == name) {
				line = this->test_lines[j];
				break;
			}
		}

		Variant owner_value = ClassDBSingleton::get_singleton()->instantiate(base_type);
		Object *owner = owner_value;
		if (owner == nullptr) {
			rows.push_back(test_row(name, line, "error",
					"could not instantiate '" + String(base_type) + "'", String(), 0));
			continue;
		}
		const bool owner_is_refcounted = Object::cast_to<RefCounted>(owner) != nullptr;

		Sandbox *sandbox = sandbox_for_safegdscript(this);
		const unsigned exceptions_before = sandbox != nullptr ? sandbox->get_exceptions() : 0;

		// The editor hands a non-@tool script a placeholder; a test needs the
		// real thing. Scoped to the attach, and restored before the call.
		{
			instantiating_for_tests = true;
			owner->set_script(Variant(Ref<SafeGDScript>(this)));
			instantiating_for_tests = false;
		}
		SafeGDScriptInstance *instance = nullptr;
		for (SafeGDScriptInstance *candidate : instances) {
			if (candidate != nullptr && candidate->get_owner() == owner) {
				instance = candidate;
				break;
			}
		}
		if (instance == nullptr) {
			rows.push_back(test_row(name, line, "error",
					"the script refused an instance on '" + String(base_type) + "'", String(), 0));
			owner->set_script(Variant());
			if (!owner_is_refcounted) {
				memdelete(owner);
			}
			continue;
		}

		GDExtensionCallError error;
		error.error = GDEXTENSION_CALL_OK;
		const uint64_t started = Time::get_singleton()->get_ticks_usec();
		// The script instance directly: Object has no callp in godot-cpp, and a
		// test is always a script method.
		instance->callp(StringName(name), nullptr, 0, error);
		const uint64_t elapsed = Time::get_singleton()->get_ticks_usec() - started;
		const unsigned exceptions_after = sandbox != nullptr ? sandbox->get_exceptions() : 0;

		if (exceptions_after > exceptions_before) {
			String message = sandbox != nullptr ? sandbox->get_last_exception() : String();
			if (message.is_empty()) {
				message = "the test raised an exception";
			}
			rows.push_back(test_row(name, line, "failed", message,
					sandbox != nullptr ? sandbox->get_last_exception_location() : String(), elapsed));
		} else if (error.error != GDEXTENSION_CALL_OK) {
			rows.push_back(test_row(name, line, "error",
					"call error " + itos(int(error.error)), String(), elapsed));
		} else {
			rows.push_back(test_row(name, line, "passed", String(), String(), elapsed));
		}

		owner->set_script(Variant());
		if (!owner_is_refcounted) {
			memdelete(owner);
		}
	}

	if (machine != nullptr && previous_budget <= 0) {
		machine->set_instructions_max(previous_budget);
	}
	safegdscript_release_sandbox(this, nullptr);

	const Dictionary report = test_report(this->path, rows,
			Time::get_singleton()->get_ticks_usec() - run_start);
	print_test_report(report);
	return report;
}

void SafeGDScript::remove_instance(SafeGDScriptInstance *p_instance) {
	instances.erase(p_instance);
}

void SafeGDScript::remove_placeholder(SafeGDScriptPlaceholderInstance *p_instance) {
	placeholders.erase(p_instance);
}

static constexpr int MAX_BASE_CHAIN = 16;

static bool header_keyword(const String &p_line, const char *p_keyword) {
	const int length = int(strlen(p_keyword));
	if (!p_line.begins_with(p_keyword)) {
		return false;
	}
	if (p_line.length() == length) {
		return true;
	}
	const char32_t next = p_line[length];
	return next == ' ' || next == '\t' || next == '"';
}

static String scan_extends(const String &p_source) {
	const PackedStringArray lines = p_source.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#") || line.begins_with("@")) {
			continue;
		}
		if (header_keyword(line, "class_name")) {
			continue;
		}
		if (!header_keyword(line, "extends")) {
			break;
		}
		String rest = line.substr(strlen("extends")).strip_edges();
		if (rest.begins_with("\"")) {
			const int end = rest.find("\"", 1);
			return end < 0 ? String() : rest.substr(1, end - 1);
		}
		for (const char *stop : { "#", " ", "\t" }) {
			const int at = rest.find(stop);
			if (at >= 0) {
				rest = rest.substr(0, at);
			}
		}
		return rest;
	}
	return String();
}

// Compiler unavailable when Godot builds the global class list.
void SafeGDScript::scan_class_header(const String &p_source, String *r_class_name,
		String *r_base, bool *r_is_trait) {
	if (r_class_name != nullptr) {
		*r_class_name = String();
	}
	if (r_base != nullptr) {
		*r_base = String();
	}
	if (r_is_trait != nullptr) *r_is_trait = false;
	const PackedStringArray lines = p_source.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#") || line.begins_with("@")) {
			continue;
		}
		if (header_keyword(line, "class_name")) {
			if (r_class_name != nullptr) {
				String rest = line.substr(strlen("class_name")).strip_edges();
				for (const char *stop : { "#", " ", "\t", ":" }) {
					const int at = rest.find(stop);
					if (at >= 0) {
						rest = rest.substr(0, at);
					}
				}
				*r_class_name = rest;
			}
			continue;
		}
		if (header_keyword(line, "trait_name")) {
			if (r_is_trait != nullptr) *r_is_trait = true;
			if (r_class_name != nullptr) {
				String rest = line.substr(strlen("trait_name")).strip_edges();
				for (const char *stop : { "#", " ", "\t", ":" }) {
					const int at = rest.find(stop);
					if (at >= 0) rest = rest.substr(0, at);
				}
				*r_class_name = rest;
			}
			continue;
		}
		if (header_keyword(line, "extends")) {
			if (r_base != nullptr) {
				*r_base = scan_extends(p_source);
			}
			continue;
		}
		if (header_keyword(line, "uses")) continue;
		break;
	}
}

static void append_uses(PackedStringArray &r_names, const String &p_rest) {
	String rest = p_rest;
	const int comment = rest.find("#");
	if (comment >= 0) rest = rest.substr(0, comment);
	// 'class X uses A, B:' ends the list with the declaration's colon.
	const int colon = rest.find(":");
	if (colon >= 0) rest = rest.substr(0, colon);
	const PackedStringArray listed = rest.split(",", false);
	for (const String &entry : listed) {
		const String name = entry.strip_edges();
		if (!name.is_empty()) r_names.push_back(name);
	}
}

// 'uses' heads the file, trails a 'class'/'trait' declaration line, or opens
// either body, so every line is scanned: a header-only scan misses the nested
// forms and the compile then fails on an undeclared trait.
static PackedStringArray scan_trait_uses(const String &p_source) {
	PackedStringArray names;
	const PackedStringArray lines = p_source.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#")) continue;
		if (header_keyword(line, "uses")) {
			append_uses(names, line.substr(strlen("uses")));
			continue;
		}
		if (!header_keyword(line, "class") && !header_keyword(line, "trait")) continue;
		for (int at = line.find(" uses"); at >= 0; at = line.find(" uses", at + 1)) {
			const int after = at + 5;
			if (after >= line.length()) break;
			const char32_t next = line[after];
			if (next != ' ' && next != '\t') continue;
			append_uses(names, line.substr(after));
			break;
		}
	}
	return names;
}

PackedStringArray SafeGDScript::resolve_base_sources(const String &p_source,
		const String &p_self_path, String *r_error) {
	PackedStringArray triples;
	HashMap<String, String> classes;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr) {
		return triples;
	}
	const TypedArray<Dictionary> global_classes = settings->get_global_class_list();
	for (int i = 0; i < global_classes.size(); i++) {
		const Dictionary entry = global_classes[i];
		const String name = entry.get("class", String());
		const String path = entry.get("path", String());
		if (!name.is_empty() && !path.is_empty()) {
			classes.insert(name, path);
		}
	}

	HashSet<String> chain_paths;
	if (!p_self_path.is_empty()) {
		chain_paths.insert(p_self_path);
	}
	String source_path = p_self_path;
	String next = scan_extends(p_source);
	Vector<Pair<String, String>> scanned_sources;
	scanned_sources.push_back(Pair<String, String>(p_source, p_self_path));
	while (!next.is_empty()) {
		String path;
		if (next.begins_with("res://") || next.begins_with("user://")) {
			path = next;
		} else if (HashMap<String, String>::Iterator it = classes.find(next); it != classes.end()) {
			path = it->value;
		} else if (looks_like_script_path(next)) {
			path = resolve_script_path(next, source_path);
		} else {
			break; // Native engine class.
		}
		if (chain_paths.has(path)) {
			if (r_error != nullptr) {
				*r_error = "'" + path + "' appears twice in the 'extends' chain: a script "
						"cannot extend itself, directly or through a base.";
			}
			break;
		}
		if (!FileAccess::file_exists(path)) {
			if (r_error != nullptr) {
				*r_error = "'" + next + "' extends a script at '" + path +
						"', which does not exist.";
			}
			break;
		}
		chain_paths.insert(path);
		const String source = FileAccess::get_file_as_string(path);
		scanned_sources.push_back(Pair<String, String>(source, path));
		triples.push_back(next);
		triples.push_back(path);
		triples.push_back(source);
		if (triples.size() / 3 >= MAX_BASE_CHAIN) {
			if (r_error != nullptr) {
				*r_error = "the 'extends' chain below '" + path + "' is deeper than " +
						itos(MAX_BASE_CHAIN) + " scripts, and every one of them is compiled "
						"into this program.";
			}
			break;
		}
		source_path = path;
		next = scan_extends(source);
	}

	HashSet<String> requested_traits;
	HashSet<String> trait_paths;
	for (int source_index = 0; source_index < scanned_sources.size(); source_index++) {
		const String &source = scanned_sources[source_index].first;
		for (const String &used_name : scan_trait_uses(source)) {
			if (requested_traits.has(used_name)) continue;
			requested_traits.insert(used_name);
			String host_name = used_name;
			const int dot = host_name.find(".");
			if (dot >= 0) host_name = host_name.substr(0, dot);
			HashMap<String, String>::Iterator found = classes.find(host_name);
			if (found == classes.end()) continue;
			const String trait_path = found->value;
			// A trait declared by an executable link is already present when the
			// chain is merged. A trait-only provider, however, may be requested
			// more than once as Library.First and Library.Second.
			if (chain_paths.has(trait_path)) continue;
			if (!FileAccess::file_exists(trait_path)) {
				if (r_error != nullptr) *r_error = "Trait '" + used_name +
					"' resolves to missing file '" + trait_path + "'.";
				continue;
			}
			const String trait_source = FileAccess::get_file_as_string(trait_path);
			triples.push_back("trait:" + used_name);
			triples.push_back(trait_path);
			triples.push_back(trait_source);
			if (!trait_paths.has(trait_path)) {
				trait_paths.insert(trait_path);
				scanned_sources.push_back(Pair<String, String>(trait_source, trait_path));
			}
		}
	}
	return triples;
}

void SafeGDScript::poll_base_sources() {
	std::vector<SafeGDScript *> scripts(live_scripts.begin(), live_scripts.end());
	for (SafeGDScript *script : scripts) {
		if (live_scripts.count(script) != 0) {
			script->rebuild_if_a_base_changed();
		}
	}
}

std::vector<SafeGDScript *> SafeGDScript::live_script_snapshot() {
	return std::vector<SafeGDScript *>(live_scripts.begin(), live_scripts.end());
}

void SafeGDScript::rebuild_if_a_base_changed() {
	if (this->base_paths.is_empty()) {
		return;
	}
	bool changed = false;
	for (int64_t i = 0; i < this->base_paths.size() && i < this->base_stamps.size(); i++) {
		if (file_stamp(this->base_paths[i]) != this->base_stamps[i]) {
			changed = true;
			break;
		}
	}
	if (!changed) {
		return;
	}
	this->compile_source_to_elf(this->profiled_build, this->debug_build);
}

bool SafeGDScript::class_access_restricted() const {
	const Sandbox *sandbox = sandbox_for_safegdscript(this);
	return sandbox != nullptr && sandbox->is_class_access_restricted();
}

void SafeGDScript::class_restrictions_changed() {
	if (this->class_access_restricted() != this->compiled_restricted || !this->last_error.is_empty()) {
		this->compile_source_to_elf(this->profiled_build, this->debug_build);
	}
	Sandbox *sandbox = sandbox_for_safegdscript(this);
	if (sandbox != nullptr && sandbox->unchecked_memory_is_stale() &&
			!instances.is_empty() && !this->elf_data.is_empty()) {
		(*instances.begin())->reset_to(this->elf_data);
	}
}

void SafeGDScript::update_methods_info(GDScriptCompilerBackend &p_compiler) {
	Sandbox::BinaryInfo info = Sandbox::get_program_info_from_binary(this->elf_data);
	this->methods_info.clear();
	this->methods_doc.clear();
	this->signals_info.clear();
	this->rpc_config.clear();
	for (const gdscript::RPCConfig &declared : p_compiler.rpc_configs()) {
		Dictionary method;
		method["rpc_mode"] = declared.rpc_mode;
		method["transfer_mode"] = declared.transfer_mode;
		method["call_local"] = declared.call_local;
		method["channel"] = declared.channel;
		this->rpc_config[StringName(String::utf8(declared.name.c_str(), declared.name.size()))] = method;
	}

	// Untyped parameter: NIL + NIL_IS_VARIANT; typed: no usage flags.
	for (const gdscript::FunctionSignature &declared : p_compiler.signal_signatures()) {
		MethodInfo signal_info(String::utf8(declared.name.c_str(), declared.name.size()));
		signal_info.flags = METHOD_FLAG_NORMAL;
		signal_info.return_val.usage = PROPERTY_USAGE_DEFAULT;
		for (const gdscript::FunctionParameter &param : declared.parameters) {
			PropertyInfo argument;
			argument.name = String::utf8(param.name.c_str(), param.name.size());
			argument.type = variant_type_or_nil(param.type);
			argument.usage = argument.type == Variant::Type::NIL
					? PROPERTY_USAGE_NIL_IS_VARIANT
					: PROPERTY_USAGE_NONE;
			signal_info.arguments.push_back(std::move(argument));
		}
		MethodDocumentation documentation;
		documentation.line = declared.line;
		documentation.description = String::utf8(declared.description.c_str(), declared.description.size());
		methods_doc.insert(signal_info.name, std::move(documentation));

		signals_info.push_back(std::move(signal_info));
	}

	this->tool_script = p_compiler.is_tool();

	this->test_functions.clear();
	this->test_lines.clear();
	for (const gdscript::FunctionSignature &declared : p_compiler.test_signatures()) {
		this->test_functions.push_back(String::utf8(declared.name.c_str(), declared.name.size()));
		this->test_lines.push_back(declared.line);
	}

	// Profiling records are indexed by position in this table.
	this->signatures = p_compiler.function_signatures();
	this->line_table = p_compiler.line_table();
	this->active_breakpoints = this->debug_build ? p_compiler.installed_breakpoints() : PackedInt32Array();
	HashMap<String, const gdscript::FunctionSignature *> by_name;
	for (const gdscript::FunctionSignature &signature : this->signatures) {
		by_name.insert(String::utf8(signature.name.c_str(), signature.name.size()), &signature);
	}

	for (const String &func_name : info.functions) {
		if (func_name.begins_with("@")) {
			continue;
		}

		MethodInfo method(func_name);
		method.return_val.usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT;

		HashMap<String, const gdscript::FunctionSignature *>::Iterator it = by_name.find(func_name);
		if (it == by_name.end()) {
			// Not a function the compiler declared: a runtime helper linked
			// into the program, say. Nothing is known about its arguments, so
			// nothing is claimed about them.
			method.flags = METHOD_FLAG_VARARG;
			methods_info.push_back(std::move(method));
			continue;
		}

		const gdscript::FunctionSignature &signature = *it->value;
		method = method_info_from_signature(signature, func_name);

		// Editor metadata, keyed by name. Only declared functions get an entry:
		// a symbol without a signature has no source line here to point at.
		MethodDocumentation documentation;
		documentation.line = signature.line;
		documentation.description = String::utf8(signature.description.c_str(), signature.description.size());
		methods_doc.insert(method.name, std::move(documentation));

		methods_info.push_back(std::move(method));
	}

	update_constants(p_compiler);
	update_static_dispatch();
	rebuild_nested_classes(p_compiler);

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScript::update_methods_info: Updated methods info with " + itos(methods_info.size()) + " methods.");
	}
}

void SafeGDScript::update_static_dispatch() {
	bool has_static = false;
	for (const gdscript::FunctionSignature &signature : this->signatures) {
		if (signature.is_static) {
			has_static = true;
			break;
		}
	}
	if (has_static == (this->static_instance != nullptr)) {
		return;
	}
	if (has_static) {
		this->static_instance = memnew(SafeGDScriptStaticInstance(this));
		internal::gdextension_interface_object_set_script_instance(this->_owner,
				ScriptInstanceExtension::create_native_instance(this->static_instance));
	} else {
		this->static_instance = nullptr;
		internal::gdextension_interface_object_set_script_instance(this->_owner, nullptr);
	}
}

// An enum arrives as its members; the Dictionary GDScript exposes is rebuilt
// here in declaration order, matching the one gen_enum_dictionary builds in the
// guest. A compiler that predates the table publishes nothing and the script
// keeps none, which is what every release before this one did.
void SafeGDScript::update_constants(GDScriptCompilerBackend &p_compiler) {
	this->constants.clear();
	for (const gdscript::ScriptConstant &declared : p_compiler.script_constants()) {
		const StringName name = String::utf8(declared.name.c_str(), declared.name.size());
		switch (declared.kind) {
			case gdscript::ScriptConstant::Kind::INT:
				this->constants.insert(name, std::get<int64_t>(declared.value));
				break;
			case gdscript::ScriptConstant::Kind::FLOAT:
				this->constants.insert(name, std::get<double>(declared.value));
				break;
			case gdscript::ScriptConstant::Kind::BOOL:
				this->constants.insert(name, std::get<bool>(declared.value));
				break;
			case gdscript::ScriptConstant::Kind::STRING: {
				const std::string &text = std::get<std::string>(declared.value);
				this->constants.insert(name, String::utf8(text.c_str(), text.size()));
				break;
			}
			case gdscript::ScriptConstant::Kind::ENUM: {
				Dictionary members;
				for (const gdscript::ScriptConstant::EnumMember &member : declared.members) {
					members[String::utf8(member.name.c_str(), member.name.size())] = member.value;
				}
				this->constants.insert(name, members);
				break;
			}
		}
	}
}

// Updated in place rather than replaced: a live instance holds a Ref to its
// class and get_script() has to answer the same object across a reload.
void SafeGDScript::rebuild_nested_classes(GDScriptCompilerBackend &p_compiler) {
	const std::vector<gdscript::ClassSignature> declared = p_compiler.class_signatures();
	trait_signatures.clear();
	for (const gdscript::ClassSignature &signature : declared) {
		if (signature.is_trait) trait_signatures.push_back(signature);
	}

	HashMap<StringName, Ref<SafeGDScriptClass>> rebuilt;
	for (const gdscript::ClassSignature &signature : declared) {
		if (signature.is_trait) continue;
		const StringName name(String::utf8(signature.name.c_str(), signature.name.size()));
		Ref<SafeGDScriptClass> nested;
		if (Ref<SafeGDScriptClass> *existing = nested_classes.getptr(name)) {
			nested = *existing;
		} else {
			nested = Ref<SafeGDScriptClass>(memnew(SafeGDScriptClass));
		}
		nested->configure(this, signature, this->signatures);
		rebuilt.insert(name, nested);
	}
	// After every class exists: a base may be declared below its derived class.
	for (const gdscript::ClassSignature &signature : declared) {
		if (signature.is_trait) continue;
		if (signature.base_name.empty()) {
			continue;
		}
		const StringName name(String::utf8(signature.name.c_str(), signature.name.size()));
		const StringName base(String::utf8(signature.base_name.c_str(), signature.base_name.size()));
		if (Ref<SafeGDScriptClass> *found = rebuilt.getptr(base)) {
			rebuilt[name]->set_base_class(*found);
		}
	}
	// A class the source no longer declares leaves its instances answering
	// INVALID_METHOD for everything, the way a removed top-level function does.
	for (const KeyValue<StringName, Ref<SafeGDScriptClass>> &entry : nested_classes) {
		if (!rebuilt.has(entry.key)) {
			entry.value->invalidate();
		}
	}
	nested_classes = rebuilt;
}

Ref<SafeGDScriptClass> SafeGDScript::find_nested_class(const StringName &p_name) const {
	if (const Ref<SafeGDScriptClass> *found = nested_classes.getptr(p_name)) {
		return *found;
	}
	return Ref<SafeGDScriptClass>();
}

bool SafeGDScript::uses_trait(const StringName &p_name) const {
	if (used_traits.has(p_name)) return true;
	Ref<Script> base = _get_base_script();
	while (base.is_valid()) {
		if (SafeGDScript *safe = fast_cast_to<SafeGDScript>(base.ptr())) {
			if (safe->used_traits.has(p_name)) return true;
		}
		base = base->get_base_script();
	}
	return false;
}
