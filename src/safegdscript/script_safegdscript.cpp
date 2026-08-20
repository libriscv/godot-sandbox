#include "script_safegdscript.h"

#include "../elf/script_elf.h"
#include "../elf/script_instance.h"
#include "script_instance_safegdscript.h"
#include "script_language_safegdscript.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include "../gdscript/compiler/function_signature.h"
#include "../sandbox.h"
static constexpr bool VERBOSE_LOGGING = false;
static Sandbox* compiler = nullptr;

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
	return PathToGlobalName(this->path);
}
bool SafeGDScript::_inherits_script(const Ref<Script> &p_script) const {
	return false;
}
StringName SafeGDScript::_get_instance_base_type() const {
	return StringName("Sandbox");
}
void *SafeGDScript::_instance_create(Object *p_for_object) const {
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
TypedArray<Dictionary> SafeGDScript::_get_documentation() const {
	return TypedArray<Dictionary>();
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

static Dictionary method_dict(const godot::MethodInfo &p_method) {
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
	return_val.name = "type";
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
		return method_dict(*method_info);
	}
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScript::_get_method_info: Method " + String(p_method) + " not found.");
	}
	return Dictionary();
}
bool SafeGDScript::_is_tool() const {
	return true;
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
	return false;
}
TypedArray<Dictionary> SafeGDScript::_get_script_signal_list() const {
	return TypedArray<Dictionary>();
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
	return 0;
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
	if (!this->path.is_empty()) {
		this->source_code = FileAccess::get_file_as_string(p_path);
	}
	this->compile_source_to_elf();
}

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
	sandbox->set_program(compiler_script);
	if (!sandbox->has_program_loaded()) {
		ERR_PRINT("SafeGDScript: Failed to initialize GDScript compiler sandbox.");
		memdelete(sandbox);
		return nullptr;
	}

	compiler = sandbox;
	return compiler;
}

bool SafeGDScript::compile_source_to_elf() {
	if (this->source_code.is_empty()) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("SafeGDScript::compile_source_to_elf: No source code to compile.");
		}
		return false;
	}

	Sandbox *compiler = get_compiler_sandbox();
	if (compiler == nullptr) {
		return false;
	}

	// Compile the source code to ELF using the compiler sandbox
	GDExtensionCallError error;
	Variant src_code_var = this->source_code;
	const Variant* args[] = { &src_code_var };
	Variant result = compiler->vmcall_fn("compile", args, 1, error);
	if (error.error != GDExtensionCallErrorType::GDEXTENSION_CALL_OK) {
		ERR_PRINT("SafeGDScript::compile_source_to_elf: Compilation failed with error code " + itos(static_cast<int>(error.error)));
		return false;
	}
	// Expecting the result to be a PackedByteArray containing the ELF binary
	if (result.get_type() != Variant::Type::PACKED_BYTE_ARRAY) {
		ERR_PRINT("SafeGDScript::compile_source_to_elf: Compilation did not return a PackedByteArray.");
		return false;
	}

	this->elf_data = result;
	if (elf_data.is_empty()) {
		// The compiler kept the reason around; saying what is wrong with the
		// script beats saying that the result was empty.
		ERR_PRINT("SafeGDScript: " + this->path + ": " + get_compiler_error_message());
		return false;
	}

	this->update_methods_info();

	for (SafeGDScriptInstance *instance : instances) {
		instance->reset_to(this->elf_data);
	}

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScript::compile_source_to_elf: Successfully compiled " + this->path + " to ELF (" + itos(this->elf_data.size()) + " bytes)");
	}

	return true;
}

// The formatted message the compiler kept from the last failed compile, quoting
// the offending line. Only meaningful right after one has failed.
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

// The signatures compile() just published, decoded from the blob the compiler
// hands out. Empty when the compiler predates the call, which leaves every
// method a vararg, as it was before.
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

	// The symbol table names the functions; only the compiler knows what they
	// take. Without the parameter list Godot has nothing to reject a call
	// against, and a missing argument arrives in the guest as a null Variant
	// pointer, which faults the moment the function reads it.
	HashMap<String, const gdscript::FunctionSignature *> signatures;
	const std::vector<gdscript::FunctionSignature> table = get_compiler_function_signatures();
	for (const gdscript::FunctionSignature &signature : table) {
		signatures.insert(String::utf8(signature.name.c_str(), signature.name.size()), &signature);
	}

	for (const String &func_name : info.functions) {
		MethodInfo method(func_name);
		method.return_val.usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT;

		HashMap<String, const gdscript::FunctionSignature *>::Iterator it = signatures.find(func_name);
		if (it == signatures.end()) {
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
		methods_info.push_back(std::move(method));
	}

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScript::update_methods_info: Updated methods info with " + itos(methods_info.size()) + " methods.");
	}
}
