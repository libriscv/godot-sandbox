#include "script_safegdscript.h"

#include "../elf/script_elf.h"
#include "../elf/script_instance.h"
#include "script_instance_safegdscript.h"
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
static constexpr bool VERBOSE_LOGGING = false;
static Sandbox* compiler = nullptr;

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
Ref<Script> SafeGDScript::_get_base_script() const {
	return Ref<Script>();
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
	return false;
}
StringName SafeGDScript::_get_instance_base_type() const {
	// Only return native types ClassDB can verify.
	if (!this->base_class.is_empty() && !this->base_is_path &&
			ClassDB::class_exists(StringName(this->base_class))) {
		return StringName(this->base_class);
	}
	return StringName("Sandbox");
}
void *SafeGDScript::_instance_create(Object *p_for_object) const {
	// GDScript refuses an owner that is not the declared base, and so must this:
	// every bare name the script did not declare is a property of that base, and
	// on the wrong owner they would all quietly answer null.
	if (p_for_object != nullptr && !this->base_class.is_empty() && !this->base_is_path) {
		const StringName base(this->base_class);
		const StringName owner_class = p_for_object->get_class();
		if (ClassDB::class_exists(base) && !ClassDB::is_parent_class(owner_class, base)) {
			ERR_PRINT("SafeGDScript: script inherits from native type '" + this->base_class +
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
// A property, then a method, in the shape Godot reads them back in:
// PropertyInfo::from_dict() and MethodInfo::from_dict() look for these exact
// keys, and quietly leave out anything spelled differently -- which is how a
// method list can look complete and still report no arguments.
static Dictionary property_dict(const godot::PropertyInfo &p_info) {
	Dictionary type;
	type["name"] = p_info.name;
	type["class_name"] = p_info.class_name;
	type["type"] = p_info.type;
	type["hint"] = PropertyHint::PROPERTY_HINT_NONE;
	type["hint_string"] = String();
	type["usage"] = p_info.usage;
	return type;
}

// Godot spells a method's return name as "type" and a signal's as empty.
static Dictionary method_dict(const godot::MethodInfo &p_method, const String &p_return_name = "type") {
	Dictionary method;
	method["name"] = p_method.name;
	method["flags"] = p_method.flags;
	method["id"] = p_method.id;

	// The argument list is what lets Godot refuse a call with the wrong number
	// of arguments -- the editor's analyzer statically, the runtime on the way
	// into the script instance.
	Array args;
	for (const godot::PropertyInfo &argument : p_method.arguments) {
		args.push_back(property_dict(argument));
	}
	method["args"] = args;
	Array default_args;
	for (const Variant &value : p_method.default_arguments) {
		default_args.push_back(value);
	}
	method["default_args"] = default_args;

	godot::PropertyInfo return_val = p_method.return_val;
	return_val.name = p_return_name;
	method["return"] = property_dict(return_val);
	return method;
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
	return Dictionary();
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

SafeGDScript::SafeGDScript() {
	source_code = R"GDScript(# SafeGDScript example

func somefunction():
	var counter = 0
	while counter < 10:
		counter += 1
	return counter

)GDScript";
}
SafeGDScript::~SafeGDScript() {
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

static constexpr uint32_t COMPILER_MEMORY_MAX = 256;
static constexpr uint32_t COMPILER_ALLOCATIONS_MAX = 64000;

Sandbox *SafeGDScript::get_compiler_sandbox() {
	if (compiler != nullptr) {
		return compiler;
	}

	// Check if "gdscript.elf" exists in the addons/godot_sandbox/ directory
	const String compiler_path = "res://addons/godot_sandbox/gdscript.elf";
	if (!FileAccess::file_exists(compiler_path)) {
		ERR_PRINT("SafeGDScript: GDScript compiler ELF not found at " + compiler_path);
		return nullptr;
	}
	Sandbox *sandbox = memnew(Sandbox);
	Ref<ELFScript> compiler_script = ResourceLoader::get_singleton()->load(compiler_path);
	if (!compiler_script.is_valid()) {
		ERR_PRINT("SafeGDScript: Failed to load GDScript compiler ELF resource.");
		memdelete(sandbox);
		return nullptr;
	}
	// Must be set before set_program(). Default arena is too small for the compiler
	// once the C++ unwinder has been set up (first error path).
	sandbox->set_memory_max(COMPILER_MEMORY_MAX);
	sandbox->set_allocations_max(COMPILER_ALLOCATIONS_MAX);
	sandbox->set_program(compiler_script);
	if (!sandbox->has_program_loaded()) {
		ERR_PRINT("SafeGDScript: Failed to initialize GDScript compiler sandbox.");
		memdelete(sandbox);
		return nullptr;
	}

	compiler = sandbox;
	return compiler;
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

	Sandbox *compiler = get_compiler_sandbox();
	if (compiler == nullptr) {
		return fail_compile("failed to initialize the GDScript compiler sandbox");
	}

	// Falls back to uninstrumented if compiler ELF predates compile_profiled.
	const bool profiling = p_profiling && compiler->has_function("compile_profiled");
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
	const bool debug = !profiling && wants_debug && compiler->has_function("compile_debug");
	if (wants_debug && !profiling && !debug) {
		ERR_PRINT("SafeGDScript: the GDScript compiler ELF is too old to build a debuggable program.");
	}
	const char *entry_point = profiling ? "compile_profiled" : (debug ? "compile_debug" : "compile");
	const bool restricted = this->class_access_restricted();
	set_compiler_restricted(restricted);
	set_compiler_project_context();

	GDExtensionCallError error;
	Variant src_code_var = this->source_code;
	// compile_debug() takes breakpoints as a second arg; others take source only.
	Variant breakpoints_var = this->breakpoints;
	const Variant* args[] = { &src_code_var, &breakpoints_var };
	Variant result = compiler->vmcall_fn(entry_point, args, debug ? 2 : 1, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK) {
		return fail_compile("the GDScript compiler sandbox failed with call error " + itos(static_cast<int>(error.error)));
	}
	// Expecting the result to be a PackedByteArray containing the ELF binary
	if (result.get_type() != Variant::Type::PACKED_BYTE_ARRAY) {
		return fail_compile("the GDScript compiler did not return a PackedByteArray");
	}
	const PackedByteArray new_elf = result;
	if (new_elf.is_empty()) {
		return fail_compile(get_compiler_error_message());
	}

	this->elf_data = new_elf;
	this->compiled_restricted = restricted;
	this->profiled_build = profiling;
	this->debug_build = debug;
	this->last_error = String();
	this->class_name = get_compiler_class_name();
	this->base_class = get_compiler_base_class();
	this->base_is_path = get_compiler_base_is_path();

	this->update_methods_info();

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

String SafeGDScript::get_compiler_error_message() {
	Sandbox *compiler = get_compiler_sandbox();
	if (compiler == nullptr || !compiler->has_function("get_compiler_error")) {
		return String("compilation failed");
	}
	GDExtensionCallError error;
	const Variant message = compiler->vmcall_fn("get_compiler_error", nullptr, 0, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK || message.get_type() != Variant::Type::STRING) {
		return String("compilation failed");
	}
	return message;
}

void SafeGDScript::remove_instance(SafeGDScriptInstance *p_instance) {
	instances.erase(p_instance);
}

// Crosses as a blob (one scoped variant) to stay within MAX_REFS.
gdscript::LineTable SafeGDScript::get_compiler_line_table() {
	gdscript::LineTable table;
	Sandbox *compiler = get_compiler_sandbox();
	// Absent from older compiler ELFs.
	if (compiler == nullptr || !compiler->has_function("get_line_table")) {
		return table;
	}
	GDExtensionCallError error;
	const Variant blob = compiler->vmcall_fn("get_line_table", nullptr, 0, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK || blob.get_type() != Variant::Type::PACKED_BYTE_ARRAY) {
		return table;
	}
	const PackedByteArray bytes = blob;
	if (!gdscript::decode_line_table(bytes.ptr(), size_t(bytes.size()), table)) {
		ERR_PRINT("SafeGDScript: the compiler returned a malformed line table.");
	}
	return table;
}

bool SafeGDScript::get_compiler_is_tool() {
	Sandbox *compiler = get_compiler_sandbox();
	if (compiler == nullptr || !compiler->has_function("is_tool")) {
		return true;
	}
	GDExtensionCallError error;
	const Variant answer = compiler->vmcall_fn("is_tool", nullptr, 0, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK) {
		return true;
	}
	return bool(answer);
}

static String compiler_string(const char *p_function) {
	Sandbox *compiler = SafeGDScript::get_compiler_sandbox();
	if (compiler == nullptr || !compiler->has_function(p_function)) {
		return String();
	}
	GDExtensionCallError error;
	const Variant answer = compiler->vmcall_fn(p_function, nullptr, 0, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK ||
			answer.get_type() != Variant::Type::STRING) {
		return String();
	}
	return answer;
}

String SafeGDScript::get_compiler_class_name() {
	return compiler_string("get_script_class_name");
}

String SafeGDScript::get_compiler_base_class() {
	return compiler_string("get_script_base_class");
}

bool SafeGDScript::get_compiler_base_is_path() {
	Sandbox *compiler = get_compiler_sandbox();
	if (compiler == nullptr || !compiler->has_function("get_script_base_is_path")) {
		return false;
	}
	GDExtensionCallError error;
	const Variant answer = compiler->vmcall_fn("get_script_base_is_path", nullptr, 0, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK) {
		return false;
	}
	return bool(answer);
}

static PackedStringArray project_autoload_names() {
	PackedStringArray names;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr) {
		return names;
	}
	const TypedArray<Dictionary> properties = settings->get_property_list();
	for (int i = 0; i < properties.size(); i++) {
		const Dictionary property = properties[i];
		const String name = property.get("name", String());
		if (name.begins_with("autoload/")) {
			names.push_back(name.substr(strlen("autoload/")));
		}
	}
	return names;
}

static PackedStringArray project_global_classes() {
	PackedStringArray pairs;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr) {
		return pairs;
	}
	const TypedArray<Dictionary> classes = settings->get_global_class_list();
	for (int i = 0; i < classes.size(); i++) {
		const Dictionary entry = classes[i];
		const String class_name = entry.get("class", String());
		const String path = entry.get("path", String());
		if (class_name.is_empty() || path.is_empty()) {
			continue;
		}
		pairs.push_back(class_name);
		pairs.push_back(path);
	}
	return pairs;
}

void SafeGDScript::set_compiler_project_context() {
	Sandbox *compiler = get_compiler_sandbox();
	if (compiler == nullptr) {
		return;
	}
	GDExtensionCallError error;
	if (compiler->has_function("set_autoloads")) {
		Variant names = project_autoload_names();
		const Variant *args[] = { &names };
		compiler->vmcall_fn("set_autoloads", args, 1, error);
		if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK) {
			ERR_PRINT("SafeGDScript: the compiler refused the autoload list.");
		}
	}
	if (compiler->has_function("set_global_classes")) {
		Variant pairs = project_global_classes();
		const Variant *args[] = { &pairs };
		compiler->vmcall_fn("set_global_classes", args, 1, error);
		if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK) {
			ERR_PRINT("SafeGDScript: the compiler refused the global class list.");
		}
	}
}

void SafeGDScript::set_compiler_restricted(bool p_restricted) {
	Sandbox *compiler = get_compiler_sandbox();
	if (compiler == nullptr || !compiler->has_function("set_restricted")) {
		return;
	}
	GDExtensionCallError error;
	Variant restricted = p_restricted;
	const Variant *args[] = { &restricted };
	compiler->vmcall_fn("set_restricted", args, 1, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK) {
		ERR_PRINT("SafeGDScript: the compiler refused the restriction flag.");
	}
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

PackedInt32Array SafeGDScript::get_compiler_breakpoint_lines() {
	Sandbox *compiler = get_compiler_sandbox();
	if (compiler == nullptr || !compiler->has_function("get_breakpoint_lines")) {
		return PackedInt32Array();
	}
	GDExtensionCallError error;
	const Variant lines = compiler->vmcall_fn("get_breakpoint_lines", nullptr, 0, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK || lines.get_type() != Variant::Type::PACKED_INT32_ARRAY) {
		return PackedInt32Array();
	}
	return lines;
}

std::vector<gdscript::FunctionSignature> SafeGDScript::get_compiler_signal_signatures() {
	std::vector<gdscript::FunctionSignature> signals;
	Sandbox *compiler = get_compiler_sandbox();
	if (compiler == nullptr || !compiler->has_function("get_signal_signatures")) {
		return signals;
	}
	GDExtensionCallError error;
	const Variant blob = compiler->vmcall_fn("get_signal_signatures", nullptr, 0, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK || blob.get_type() != Variant::Type::PACKED_BYTE_ARRAY) {
		return signals;
	}
	const PackedByteArray bytes = blob;
	if (!gdscript::decode_function_signatures(bytes.ptr(), size_t(bytes.size()), signals)) {
		ERR_PRINT("SafeGDScript: the compiler returned a malformed signal table.");
	}
	return signals;
}

std::vector<gdscript::FunctionSignature> SafeGDScript::get_compiler_function_signatures() {
	std::vector<gdscript::FunctionSignature> signatures;
	Sandbox *compiler = get_compiler_sandbox();
	if (compiler == nullptr || !compiler->has_function("get_function_signatures")) {
		return signatures;
	}
	GDExtensionCallError error;
	const Variant blob = compiler->vmcall_fn("get_function_signatures", nullptr, 0, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK || blob.get_type() != Variant::Type::PACKED_BYTE_ARRAY) {
		return signatures;
	}
	const PackedByteArray bytes = blob;
	if (!gdscript::decode_function_signatures(bytes.ptr(), size_t(bytes.size()), signatures)) {
		ERR_PRINT("SafeGDScript: the compiler returned a malformed function signature table.");
	}
	return signatures;
}

// The compiler reports an undeclared type as ANY_TYPE, which is not a
// Variant::Type. Godot spells "any Variant" as NIL plus NIL_IS_VARIANT.
static Variant::Type variant_type_or_nil(int32_t p_type) {
	if (p_type < 0 || p_type >= Variant::Type::VARIANT_MAX) {
		return Variant::Type::NIL;
	}
	return Variant::Type(p_type);
}

// The value the host passes for an argument the caller left out. Only the kinds
// the compiler folds appear here; a parameter it could not fold is required, so
// nothing ever asks for its default.
static Variant default_argument_value(const gdscript::FunctionParameter &p_param) {
	using DefaultKind = gdscript::FunctionParameter::DefaultKind;
	switch (p_param.default_kind) {
		case DefaultKind::INT:
			return std::get<int64_t>(p_param.default_value);
		case DefaultKind::FLOAT:
			return std::get<double>(p_param.default_value);
		case DefaultKind::BOOL:
			return std::get<bool>(p_param.default_value);
		case DefaultKind::STRING:
			return String::utf8(std::get<std::string>(p_param.default_value).c_str(), std::get<std::string>(p_param.default_value).size());
		case DefaultKind::EMPTY_ARRAY:
			return Array();
		case DefaultKind::EMPTY_DICT:
			return Dictionary();
		case DefaultKind::NONE:
		case DefaultKind::NIL:
			break;
	}
	return Variant();
}

void SafeGDScript::update_methods_info() {
	Sandbox::BinaryInfo info = Sandbox::get_program_info_from_binary(this->elf_data);
	this->methods_info.clear();
	this->methods_doc.clear();
	this->signals_info.clear();

	// Untyped parameter: NIL + NIL_IS_VARIANT; typed: no usage flags.
	for (const gdscript::FunctionSignature &declared : get_compiler_signal_signatures()) {
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

	this->tool_script = get_compiler_is_tool();

	// Profiling records are indexed by position in this table.
	this->signatures = get_compiler_function_signatures();
	this->line_table = get_compiler_line_table();
	this->active_breakpoints = this->debug_build ? get_compiler_breakpoint_lines() : PackedInt32Array();
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
		method.flags = METHOD_FLAG_NORMAL;
		method.return_val.type = variant_type_or_nil(signature.return_type);
		for (const gdscript::FunctionParameter &param : signature.parameters) {
			PropertyInfo argument;
			argument.name = String::utf8(param.name.c_str(), param.name.size());
			argument.type = variant_type_or_nil(param.type);
			argument.usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT;
			method.arguments.push_back(std::move(argument));
		}
		// Godot's convention: the defaults cover the last N arguments, which is
		// exactly the run of parameters past the required ones.
		for (size_t i = signature.required_arguments; i < signature.parameters.size(); i++) {
			method.default_arguments.push_back(default_argument_value(signature.parameters[i]));
		}

		// Editor metadata, keyed by name. Only declared functions get an entry:
		// a symbol without a signature has no source line here to point at.
		MethodDocumentation documentation;
		documentation.line = signature.line;
		documentation.description = String::utf8(signature.description.c_str(), signature.description.size());
		methods_doc.insert(method.name, std::move(documentation));

		methods_info.push_back(std::move(method));
	}

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScript::update_methods_info: Updated methods info with " + itos(methods_info.size()) + " methods.");
	}
}
