#include "script_safegdscript.h"

#include "../elf/script_elf.h"
#include "../elf/script_instance.h"
#include "script_instance_safegdscript.h"
#include "script_language_safegdscript.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
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
bool SafeGDScript::_has_method(const StringName &p_method) const {
	if (p_method == StringName("_init"))
		return true;
	for (const godot::MethodInfo &method_info : methods_info) {
		if (method_info.name == p_method) {
			return true;
		}
	}
	return false;
}
bool SafeGDScript::_has_static_method(const StringName &p_method) const {
	return false;
}
Dictionary SafeGDScript::_get_method_info(const StringName &p_method) const {
	if (instances.is_empty()) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("SafeGDScript::_get_method_info: No instances available.");
		}
		return Dictionary();
	}
	Dictionary method_dict;
	for (const godot::MethodInfo &method_info : methods_info) {
		if (method_info.name == p_method) {
			method_dict["name"] = method_info.name;
			method_dict["flags"] = method_info.flags;
			method_dict["return_type"] = method_info.return_val.type;
			TypedArray<Dictionary> args;
			for (const godot::PropertyInfo &arg_info : method_info.arguments) {
				Dictionary arg_dict;
				arg_dict["name"] = arg_info.name;
				arg_dict["type"] = arg_info.type;
				arg_dict["usage"] = arg_info.usage;
				args.append(arg_dict);
			}
			method_dict["arguments"] = args;
			return method_dict;
		}
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
	if (instances.is_empty()) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("SafeGDScript::_get_script_method_list: No instances available.");
		}
		return {};
	}
	TypedArray<Dictionary> functions_array;
	for (const godot::MethodInfo &method_info : methods_info) {
		Dictionary method;
		method["name"] = method_info.name;
		method["args"] = Array();
		method["default_args"] = Array();
		Dictionary type;
		type["name"] = "type";
		type["type"] = Variant::Type::NIL;
		//type["class_name"] = "class";
		type["hint"] = PropertyHint::PROPERTY_HINT_NONE;
		type["hint_string"] = String();
		type["usage"] = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT;
		method["return"] = type;
		method["flags"] = METHOD_FLAG_VARARG;
		functions_array.push_back(method);
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

bool SafeGDScript::compile_source_to_elf() {
	if (this->source_code.is_empty()) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("SafeGDScript::compile_source_to_elf: No source code to compile.");
		}
		return false;
	}

	if (compiler == nullptr) {
		// Check if "gdscript.elf" exists in the addons/godot_sandbox/ directory
		String compiler_path = "res://addons/godot_sandbox/gdscript.elf";
		if (!FileAccess::file_exists(compiler_path)) {
			ERR_PRINT("SafeGDScript::compile_source_to_elf: GDScript compiler ELF not found at " + compiler_path);
			return false;
		}
		compiler = memnew(Sandbox);
		Ref<ELFScript> compiler_script = ResourceLoader::get_singleton()->load(compiler_path);
		if (!compiler_script.is_valid()) {
			ERR_PRINT("SafeGDScript::compile_source_to_elf: Failed to load GDScript compiler ELF resource.");
			memdelete(compiler);
			compiler = nullptr;
			return false;
		}
		compiler->set_program(compiler_script);
		if (!compiler->has_program_loaded()) {
			ERR_PRINT("SafeGDScript::compile_source_to_elf: Failed to initialize GDScript compiler sandbox.");
			memdelete(compiler);
			compiler = nullptr;
			return false;
		}
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
		ERR_PRINT("SafeGDScript::compile_source_to_elf: Compilation returned empty ELF data.");
		return false;
	}

	for (SafeGDScriptInstance *instance : instances) {
		instance->reset_to(this->elf_data);
	}

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScript::compile_source_to_elf: Successfully compiled " + this->path + " to ELF (" + itos(this->elf_data.size()) + " bytes)");
	}

	return true;
}

void SafeGDScript::remove_instance(SafeGDScriptInstance *p_instance) {
	instances.erase(p_instance);
}

void SafeGDScript::update_methods_info(Sandbox *p_sandbox) {
	if (!methods_info.empty())
		return;
	if (elf_data.is_empty() || p_sandbox == nullptr) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("SafeGDScript::update_methods_info: No ELF data available.");
		}
		return;
	}

	Sandbox::BinaryInfo info = Sandbox::get_program_info_from_binary(this->elf_data);
	for (const String &func_name : info.functions) {
		WARN_PRINT("Found function: " + func_name);
		methods_info.push_back(MethodInfo(func_name));
	}

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("SafeGDScript::update_methods_info: Updated methods info with " + itos(methods_info.size()) + " methods.");
	}
}
