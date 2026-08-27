#include "script_safegdscript.h"

#include "compiler_backend.h"
#include "sgd_timing.h"
#include "../elf/script_instance.h"
#include "script_class_safegdscript.h"
#include "script_instance_safegdscript.h"
#include "script_dicts.h"
#include "signature_info.h"
#include "script_language_safegdscript.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include "../gdscript/compiler/function_signature.h"
#include "../sandbox.h"
#include <unordered_set>
static constexpr bool VERBOSE_LOGGING = false;

bool safegdscript_is_stopped();
const SafeGDScript *safegdscript_stopped_script();
int64_t safegdscript_stopped_line();
PackedStringArray safegdscript_stopped_backtrace();
void safegdscript_debug_continue();
PackedInt32Array safegdscript_engine_breakpoints(const SafeGDScript &p_script);
Sandbox *sandbox_for_safegdscript(const SafeGDScript *p_script);

bool SafeGDScript::_editor_can_reload_from_file() {
	return true;
}
void SafeGDScript::_placeholder_erased(void *p_placeholder) {}
bool SafeGDScript::_can_instantiate() const {
	return true;
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
	const String path = base_is_path ? base_class : script_class_path(base_class);
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
	return _instance_create(p_for_object);
}
bool SafeGDScript::_instance_has(Object *p_object) const {
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
	compile_source_to_elf();
}
Error SafeGDScript::_reload(bool p_keep_state) {
	compile_source_to_elf();
	return Error::OK;
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
	return _get_global_name();
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

TypedArray<Dictionary> SafeGDScript::_get_documentation() const {
	// One page listing the exported functions. Keys are what
	// DocData::ClassDoc::from_dict() reads back; an unrecognised key is dropped
	// silently, yielding an empty page.
	Dictionary class_doc;
	class_doc["name"] = String(_get_doc_class_name());
	class_doc["inherits"] = String(_get_instance_base_type());

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
			method_doc["description"] = documentation->description;
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
			signal_doc["description"] = documentation->description;
		}
		signal_docs.push_back(signal_doc);
	}
	class_doc["signals"] = signal_docs;

	TypedArray<Dictionary> documentation;
	documentation.push_back(class_doc);
	return documentation;
}
String SafeGDScript::_get_class_icon_path() const {
	return String("res://addons/godot_sandbox/SafeGDScript.svg");
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
	return false;
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
	return false;
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
	return false;
}
Variant SafeGDScript::_get_property_default_value(const StringName &p_property) const {
	return Variant();
}
void SafeGDScript::_update_exports() {}
TypedArray<Dictionary> SafeGDScript::_get_script_method_list() const {
	TypedArray<Dictionary> functions_array;
	for (const godot::MethodInfo &method_info : methods_info) {
		functions_array.push_back(method_dict(method_info));
	}
	return functions_array;
}
TypedArray<Dictionary> SafeGDScript::_get_script_property_list() const {
	if (instances.is_empty()) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("SafeGDScript::_get_script_property_list: No instances available.");
		}
		return {};
	}
	SafeGDScriptInstance *instance = *instances.begin();
	if (instance == nullptr) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("SafeGDScript::_get_script_property_list: Instance is null.");
		}
		return {};
	}
	return {};
}
int32_t SafeGDScript::_get_member_line(const StringName &p_member) const {
	// 1-based, as the editor counts lines. Not-found is -1, not 0: a caller opens
	// the script at the returned line, so 0 would jump to the top of the wrong
	// file instead of letting the caller look elsewhere.
	if (const MethodDocumentation *documentation = methods_doc.getptr(p_member)) {
		return documentation->line;
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
	return TypedArray<StringName>();
}
bool SafeGDScript::_is_placeholder_fallback_enabled() const {
	return false;
}
Variant SafeGDScript::_get_rpc_config() const {
	return Variant();
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

bool SafeGDScript::compile_source_to_elf(bool p_profiling, bool p_debug) {
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

	const bool restricted = this->class_access_restricted();
	GDScriptCompilerBackend &compiler = gdscript_compiler::backend_for(restricted);
	if (!compiler.available()) {
		return fail_compile(String("failed to initialize the ") + compiler.name() +
				" GDScript compiler");
	}

	const bool profiling = p_profiling && compiler.can_build_profiled();
	if (p_profiling && !profiling) {
		ERR_PRINT("SafeGDScript: the GDScript compiler ELF is too old to build a profiled program.");
	}
	// Merge editor breakpoints into this build (adds only, never removes).
	for (const int32_t line : safegdscript_engine_breakpoints(*this)) {
		const int64_t at = this->breakpoints.bsearch(line, true);
		if (at >= this->breakpoints.size() || this->breakpoints[at] != line) {
			this->breakpoints.insert(at, line);
		}
	}
	// Non-empty breakpoints force a debug build.
	const bool wants_debug = p_debug || !this->breakpoints.is_empty();
	// Profiling and debug are mutually exclusive instrumentations.
	const bool debug = !profiling && wants_debug && compiler.can_build_debug();
	if (wants_debug && !profiling && !debug) {
		ERR_PRINT("SafeGDScript: the GDScript compiler ELF is too old to build a debuggable program.");
	}

	this->base_paths.clear();
	this->base_stamps.clear();
	PackedStringArray base_sources;
	if (!restricted) {
		String chain_error;
		base_sources = resolve_base_sources(this->source_code, this->path, &chain_error);
		if (!chain_error.is_empty()) {
			return fail_compile(chain_error);
		}
		for (int64_t i = 0; i + 1 < base_sources.size(); i += 3) {
			this->base_paths.push_back(base_sources[i + 1]);
			this->base_stamps.push_back(file_stamp(base_sources[i + 1]));
		}
	}
	gdscript_compiler::prepare(compiler, restricted, base_sources);

	GDScriptCompilerBackend::BuildOptions options;
	options.profiling = profiling;
	options.debug = debug;
	options.breakpoints = this->breakpoints;
	PackedByteArray new_elf;
	{
		SGD_TIME_COMPILE();
		new_elf = compiler.compile(this->source_code, options);
	}
	if (new_elf.is_empty()) {
		return fail_compile(compiler.error_message());
	}

	this->elf_data = new_elf;
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
	if (this->native_base_class.is_empty()) {
		this->native_base_class = this->base_class;
		this->native_base_is_path = this->base_is_path;
	}
	this->base_script = Ref<Script>();
	this->base_script_resolved = false;

	this->update_methods_info(compiler);

	// One reload for the Sandbox they share: reloading per instance would replace
	// the machine again under the instances that had already taken a record in
	// it. Each takes a fresh one on its next call.
	if (!instances.is_empty()) {
		(*instances.begin())->reset_to(this->elf_data);
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
	return this->breakpoints;
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

	ClassDB::bind_method(D_METHOD("get_compile_error"), &SafeGDScript::get_compile_error);

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

Variant SafeGDScript::new_instance(const Variant **p_args, GDExtensionInt p_argcount, GDExtensionCallError &r_error) {
	r_error.error = GDEXTENSION_CALL_OK;

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

void SafeGDScript::remove_instance(SafeGDScriptInstance *p_instance) {
	instances.erase(p_instance);
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
		String *r_base) {
	if (r_class_name != nullptr) {
		*r_class_name = String();
	}
	if (r_base != nullptr) {
		*r_base = String();
	}
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
		if (header_keyword(line, "extends")) {
			if (r_base != nullptr) {
				*r_base = scan_extends(p_source);
			}
			continue;
		}
		break;
	}
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

	HashSet<String> visited;
	if (!p_self_path.is_empty()) {
		visited.insert(p_self_path);
	}
	String next = scan_extends(p_source);
	while (!next.is_empty()) {
		String path;
		if (next.begins_with("res://") || next.begins_with("user://")) {
			path = next;
		} else if (HashMap<String, String>::Iterator it = classes.find(next); it != classes.end()) {
			path = it->value;
		} else {
			break;
		}
		if (visited.has(path)) {
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
		visited.insert(path);
		const String source = FileAccess::get_file_as_string(path);
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
		next = scan_extends(source);
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
	rebuild_nested_classes(p_compiler);

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScript::update_methods_info: Updated methods info with " + itos(methods_info.size()) + " methods.");
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

	HashMap<StringName, Ref<SafeGDScriptClass>> rebuilt;
	for (const gdscript::ClassSignature &signature : declared) {
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
