#include "script_cpp.h"

#include "../elf/script_instance.h"
#include "../sandbox_project_settings.h"
#include "script_language_cpp.h"
#include "script_cpp_instance.h"
#include "../elf/script_instance_helper.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
static constexpr bool VERBOSE_LOGGING = false;

bool CPPScript::DetectCMakeOrSConsProject() {
	static bool detected = false;
	static bool detected_value = false;
	// Avoid file system checks if the project type has already been detected
	if (detected) {
		return detected_value;
	}
	// If the project root contains a CMakeLists.txt file, or a cmake/CMakeLists.txt,
	// build the project using CMake
	// Get the project root using res://
	String project_root = "res://";
	detected = true;

	// Check for CMakeLists.txt in the project root
	const bool cmake_root = FileAccess::file_exists(project_root + "CMakeLists.txt");
	if (cmake_root) {
		detected_value = true;
		return true;
	}
	const bool cmake_dir = FileAccess::file_exists(project_root + "cmake/CMakeLists.txt");
	if (cmake_dir) {
		detected_value = true;
		return true;
	}
	const bool scons_root = FileAccess::file_exists(project_root + "SConstruct");
	if (scons_root) {
		detected_value = true;
		return true;
	}
	detected_value = false;
	return false;
}

bool CPPScript::_editor_can_reload_from_file() {
	return true;
}
void CPPScript::_placeholder_erased(void *p_placeholder) {}
bool CPPScript::_can_instantiate() const {
	return true;
}
Ref<Script> CPPScript::_get_base_script() const {
	return Ref<Script>();
}
StringName CPPScript::_get_global_name() const {
	return PathToGlobalName(this->path);
}
bool CPPScript::_inherits_script(const Ref<Script> &p_script) const {
	return false;
}
StringName CPPScript::_get_instance_base_type() const {
	return StringName("Sandbox");
}
void *CPPScript::_instance_create(Object *p_for_object) const {
	CPPScriptInstance *instance = memnew(CPPScriptInstance(p_for_object, Ref<CPPScript>(this)));
	instances.insert(instance);
	if (instances.size() == 1) {
		this->update_methods_info();
	}
	return ScriptInstanceExtension::create_native_instance(instance);
}
void *CPPScript::_placeholder_instance_create(Object *p_for_object) const {
	return _instance_create(p_for_object);
}
bool CPPScript::_instance_has(Object *p_object) const {
	return false;
}
bool CPPScript::_has_source_code() const {
	return true;
}
String CPPScript::_get_source_code() const {
	return source_code;
}
void CPPScript::_set_source_code(const String &p_code) {
	source_code = p_code;
}
Error CPPScript::_reload(bool p_keep_state) {
	this->set_file(this->path);
	return Error::OK;
}
TypedArray<Dictionary> CPPScript::_get_documentation() const {
	return TypedArray<Dictionary>();
}
String CPPScript::_get_class_icon_path() const {
	return String("res://addons/godot_sandbox/CPPScript.svg");
}
bool CPPScript::_has_method(const StringName &p_method) const {
	if (p_method == StringName("_init"))
		return true;
	for (const godot::MethodInfo &method_info : methods_info) {
		if (method_info.name == p_method) {
			return true;
		}
	}
	return false;
}
bool CPPScript::_has_static_method(const StringName &p_method) const {
	return false;
}
Dictionary CPPScript::_get_method_info(const StringName &p_method) const {
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
		ERR_PRINT("CPPScript::_get_method_info: Method " + String(p_method) + " not found.");
	}
	return Dictionary();
}
bool CPPScript::_is_tool() const {
	return true;
}
bool CPPScript::_is_valid() const {
	return elf_script.is_valid() && !elf_script->get_content().is_empty();
}
bool CPPScript::_is_abstract() const {
	return false;
}
ScriptLanguage *CPPScript::_get_language() const {
	return CPPScriptLanguage::get_singleton();
}
bool CPPScript::_has_script_signal(const StringName &p_signal) const {
	return false;
}
TypedArray<Dictionary> CPPScript::_get_script_signal_list() const {
	return TypedArray<Dictionary>();
}
bool CPPScript::_has_property_default_value(const StringName &p_property) const {
	return false;
}
Variant CPPScript::_get_property_default_value(const StringName &p_property) const {
	return Variant();
}
void CPPScript::_update_exports() {}
TypedArray<Dictionary> CPPScript::_get_script_method_list() const {
	TypedArray<Dictionary> functions_array;
	for (const godot::MethodInfo &method_info : methods_info) {
		Dictionary method;
		method["name"] = method_info.name;
		method["args"] = Array();
		method["default_args"] = Array();
		Dictionary type;
		type["name"] = "type";
		type["type"] = Variant::Type::NIL;
		type["hint"] = PropertyHint::PROPERTY_HINT_NONE;
		type["hint_string"] = String();
		type["usage"] = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT;
		method["return"] = type;
		method["flags"] = METHOD_FLAG_VARARG;
		functions_array.push_back(method);
	}
	return functions_array;
}
TypedArray<Dictionary> CPPScript::_get_script_property_list() const {
	if (instances.is_empty()) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("CPPScript::_get_script_property_list: No instances available.");
		}
		return {};
	}
	TypedArray<Dictionary> properties;
	Dictionary property;
	property["name"] = "associated_script";
	property["type"] = Variant::OBJECT;
	property["hint"] = PROPERTY_HINT_NODE_TYPE;
	property["hint_string"] = "Node";
	property["usage"] = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE;
	//property["default_value"] = source_code;
	properties.push_back(property);
	for (const PropertyInfo &prop : Sandbox::create_sandbox_property_list()) {
		properties.push_back(prop_to_dict(prop));
	}
	return properties;
}
int32_t CPPScript::_get_member_line(const StringName &p_member) const {
	return 0;
}
Dictionary CPPScript::_get_constants() const {
	return Dictionary();
}
TypedArray<StringName> CPPScript::_get_members() const {
	return TypedArray<StringName>();
}
bool CPPScript::_is_placeholder_fallback_enabled() const {
	return false;
}
Variant CPPScript::_get_rpc_config() const {
	return Variant();
}

CPPScript::CPPScript() {
	source_code = R"C0D3(#include "api.hpp"

static Variant my_function(Vector4 v) {
	print("Arg: ", v);
	return 123;
}

static Variant _process() {
	static int counter = 0;
	if (++counter % 100 == 0) {
		print("Process called " + std::to_string(counter) + " times");
	}
	return Nil;
}

static Vector4 my_vector4(1.0f, 2.0f, 3.0f, 4.0f);
static String my_string("Hello, World!");
int main() {
	ADD_PROPERTY(my_vector4, Variant::VECTOR4);
	ADD_PROPERTY(my_string, Variant::STRING);

	ADD_API_FUNCTION(my_function, "int", "Vector4 v");
	ADD_API_FUNCTION(_process, "void");
}
)C0D3";
}
CPPScript::~CPPScript() {
}

void CPPScript::set_file(const String &p_path) {
	if (p_path.is_empty()) {
		WARN_PRINT("CPPScript::set_file: Empty resource path.");
		return;
	}
	this->path = p_path;
	this->source_code = FileAccess::get_file_as_string(p_path);
}

const PackedByteArray &CPPScript::get_elf_data() const {
	if (elf_script.is_valid()) {
		return elf_script->get_content();
	}
	static PackedByteArray empty;
	return empty;
}

bool CPPScript::detect_script_instance() {
	// It's possible to speculate that eg. a fitting ELFScript would be located at
	// "res://this/path.cpp" replacing the extension with ".elf".
	if (this->path.is_empty()) {
		WARN_PRINT("CPPScript::detect_script_instance: Empty resource path.");
		return false;
	}
	const String elf_path = this->path.get_basename() + ".elf";
	if (FileAccess::file_exists(elf_path)) {
		if constexpr (VERBOSE_LOGGING) {
			ERR_PRINT("CPPScript::detect_script_instance: Found ELF script at " + elf_path);
		}
		// Try to get the resource from the path
		Ref<ELFScript> res = ResourceLoader::get_singleton()->load(elf_path, "ELFScript");
		if (res.is_valid()) {
			if constexpr (VERBOSE_LOGGING) {
				ERR_PRINT("CPPScript::detect_script_instance: ELF script loaded successfully.");
			}
			this->elf_script = res;
			this->update_methods_info();
			return true;
		}
	}
	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScript::detect_script_instance: No ELF script found at " + elf_path);
	}
	return false;
}

void CPPScript::set_elf_script(const Ref<ELFScript> &p_elf_script) {
	this->elf_script = p_elf_script;

	for (CPPScriptInstance *instance : instances) {
		instance->reset_to(p_elf_script);
	}

	this->update_methods_info();
}

void CPPScript::remove_instance(CPPScriptInstance *p_instance) {
	instances.erase(p_instance);
	if (instances.is_empty()) {
		this->elf_script.unref();
	}
}

void CPPScript::update_methods_info() const {
	if (!elf_script.is_valid())
		return;

	this->methods_info.clear();
	if (elf_script.is_valid() && !elf_script->function_names.is_empty()) {
		const auto& methods = elf_script->function_names;
		for (int i = 0; i < methods.size(); i++) {
			godot::MethodInfo method_info = godot::MethodInfo(methods[i]);
			methods_info.push_back(method_info);
		}
	} else {
		const PackedByteArray &elf_data = elf_script->get_content();
		Sandbox::BinaryInfo info = Sandbox::get_program_info_from_binary(elf_data);
		for (const String &func_name : info.functions) {
			methods_info.push_back(MethodInfo(func_name));
		}
	}
	methods_info.push_back(MethodInfo("get_associated_script"));
	methods_info.push_back(MethodInfo("set_associated_script"));

	if constexpr (VERBOSE_LOGGING) {
		ERR_PRINT("CPPScript::update_methods_info: Updated methods info with " + itos(methods_info.size()) + " methods.");
	}
}
