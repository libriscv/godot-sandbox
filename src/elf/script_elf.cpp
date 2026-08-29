#include "script_elf.h"

#include "../cpp/script_cpp.h"
#include "../docker.h"
#include "../register_types.h"
#include "../sandbox.h"
#include "../sandbox_project_settings.h"
#include "script_instance.h"
#include "script_instance_helper.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
static constexpr bool VERBOSE_ELFSCRIPT = false;

static Variant::Type elf_variant_type_or_nil(int32_t p_type) {
	if (p_type < 0 || p_type >= Variant::Type::VARIANT_MAX) {
		return Variant::Type::NIL;
	}
	return Variant::Type(p_type);
}

static Variant elf_default_argument_value(const gdscript::FunctionParameter &p_param) {
	using DefaultKind = gdscript::FunctionParameter::DefaultKind;
	switch (p_param.default_kind) {
		case DefaultKind::INT:
			return std::get<int64_t>(p_param.default_value);
		case DefaultKind::FLOAT:
			return std::get<double>(p_param.default_value);
		case DefaultKind::BOOL:
			return std::get<bool>(p_param.default_value);
		case DefaultKind::STRING: {
			const std::string &s = std::get<std::string>(p_param.default_value);
			return String::utf8(s.c_str(), s.size());
		}
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

static Dictionary elf_arg_to_dict(const gdscript::FunctionParameter &p_param) {
	Dictionary arg;
	arg["name"] = String::utf8(p_param.name.c_str(), p_param.name.size());
	const Variant::Type type = elf_variant_type_or_nil(p_param.type);
	arg["type"] = type;
	arg["class_name"] = p_param.class_name.empty() ? StringName() :
		StringName(String::utf8(p_param.class_name.c_str(), p_param.class_name.size()));
	arg["hint"] = PropertyHint::PROPERTY_HINT_NONE;
	arg["hint_string"] = String();
	arg["usage"] = type == Variant::Type::NIL
			? PROPERTY_USAGE_NIL_IS_VARIANT
			: PROPERTY_USAGE_DEFAULT;
	return arg;
}

static Dictionary elf_signature_to_method_dict(const gdscript::FunctionSignature &p_sig) {
	Dictionary method;
	method["name"] = String::utf8(p_sig.name.c_str(), p_sig.name.size());
	method["flags"] = METHOD_FLAG_NORMAL;

	Array args;
	for (const gdscript::FunctionParameter &param : p_sig.parameters) {
		args.push_back(elf_arg_to_dict(param));
	}
	method["args"] = args;

	Array default_args;
	for (size_t i = p_sig.required_arguments; i < p_sig.parameters.size(); i++) {
		default_args.push_back(elf_default_argument_value(p_sig.parameters[i]));
	}
	method["default_args"] = default_args;

	Dictionary ret;
	const Variant::Type ret_type = elf_variant_type_or_nil(p_sig.return_type);
	ret["name"] = String();
	ret["type"] = ret_type;
	ret["class_name"] = p_sig.return_class_name.empty() ? StringName() :
		StringName(String::utf8(p_sig.return_class_name.c_str(), p_sig.return_class_name.size()));
	ret["hint"] = PropertyHint::PROPERTY_HINT_NONE;
	ret["hint_string"] = String();
	ret["usage"] = ret_type == Variant::Type::NIL
			? PROPERTY_USAGE_NIL_IS_VARIANT
			: PROPERTY_USAGE_DEFAULT;
	method["return"] = ret;
	return method;
}

const gdscript::FunctionSignature *ELFScript::find_signature(const StringName &p_name) const {
	if (!has_script_metadata) {
		return nullptr;
	}
	const CharString utf8 = String(p_name).utf8();
	const std::string name(utf8.ptr(), utf8.length());
	for (const gdscript::FunctionSignature &sig : script_metadata.functions) {
		if (sig.name == name) {
			return &sig;
		}
	}
	return nullptr;
}

void ELFScript::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_sandbox_for", "for_object"), &ELFScript::get_sandbox_for);
	ClassDB::bind_method(D_METHOD("get_sandbox_objects"), &ELFScript::get_sandbox_objects);
	ClassDB::bind_method(D_METHOD("get_content"), &ELFScript::get_content);
}

Sandbox *ELFScript::get_sandbox_for(Object *p_for_object) const {
	for (ELFScriptInstance *instance : this->instances) {
		if (instance->get_owner() == p_for_object) {
			auto [sandbox, auto_created] = instance->get_sandbox();
			return sandbox;
		}
	}
	ERR_PRINT("ELFScript::get_sandbox_for: Sandbox not found for object " + p_for_object->get_class());
	return nullptr;
}

Array ELFScript::get_sandbox_objects() const {
	Array result;
	for (ELFScriptInstance *instance : this->instances) {
		result.push_back(instance->get_owner());
	}
	return result;
}

ELFScriptInstance *ELFScript::get_script_instance(Object *p_for_object) const {
	for (ELFScriptInstance *instance : this->instances) {
		if (instance->get_owner() == p_for_object) {
			return instance;
		}
	}
	ERR_PRINT("ELFScript::get_script_instance: Script instance not found for object " + p_for_object->get_class());
	return nullptr;
}

bool ELFScript::_editor_can_reload_from_file() {
	return true;
}
void ELFScript::_placeholder_erased(void *p_placeholder) {}
bool ELFScript::_can_instantiate() const {
	return true;
}
Ref<Script> ELFScript::_get_base_script() const {
	return Ref<Script>();
}
StringName ELFScript::_get_global_name() const {
	if (has_script_metadata && !script_metadata.class_name.empty()) {
		return StringName(String::utf8(script_metadata.class_name.c_str(), script_metadata.class_name.size()));
	}
	if (SandboxProjectSettings::use_global_sandbox_names()) {
		return global_name;
	}
	return "ELFScript";
}
bool ELFScript::_inherits_script(const Ref<Script> &p_script) const {
	return p_script.ptr() == static_cast<const Script *>(this);
}
StringName ELFScript::_get_instance_base_type() const {
	if (has_script_metadata && !script_metadata.base_is_path && !script_metadata.base_class.empty()) {
		const StringName base(String::utf8(script_metadata.base_class.c_str(), script_metadata.base_class.size()));
		if (ClassDB::class_exists(base)) {
			return base;
		}
	}
	return StringName("Sandbox");
}
void *ELFScript::_instance_create(Object *p_for_object) const {
	ELFScriptInstance *instance = memnew(ELFScriptInstance(p_for_object, Ref<ELFScript>(this)));
	instances.insert(instance);
	return ScriptInstanceExtension::create_native_instance(instance);
}
void *ELFScript::_placeholder_instance_create(Object *p_for_object) const {
	return _instance_create(p_for_object);
}
bool ELFScript::_instance_has(Object *p_object) const {
	return false;
}
bool ELFScript::_has_source_code() const {
	return true;
}
String ELFScript::_get_source_code() const {
	if (source_code.is_empty()) {
		return String();
	}
	if (functions.is_empty()) {
		return JSON::stringify(function_names, "  ");
	} else {
		return JSON::stringify(functions, "  ");
	}
}
void ELFScript::_set_source_code(const String &p_code) {
}
Error ELFScript::_reload(bool p_keep_state) {
	this->source_version++;
	this->set_file(this->path);
	return Error::OK;
}
TypedArray<Dictionary> ELFScript::_get_documentation() const {
	if (!has_script_metadata || script_metadata.functions.empty()) {
		return TypedArray<Dictionary>();
	}

	Dictionary class_doc;
	class_doc["name"] = String(_get_global_name());
	class_doc["inherits"] = String(_get_instance_base_type());

	auto doc_type_name = [](Variant::Type type) -> String {
		return type == Variant::Type::NIL ? String("Variant") : Variant::get_type_name(type);
	};
	Array method_docs;
	for (const gdscript::FunctionSignature &sig : script_metadata.functions) {
		Dictionary method_doc;
		method_doc["name"] = String::utf8(sig.name.c_str(), sig.name.size());
		method_doc["return_type"] = doc_type_name(elf_variant_type_or_nil(sig.return_type));
		if (!sig.description.empty()) {
			method_doc["description"] = String::utf8(sig.description.c_str(), sig.description.size());
		}
		Array argument_docs;
		for (const gdscript::FunctionParameter &param : sig.parameters) {
			Dictionary argument_doc;
			argument_doc["name"] = String::utf8(param.name.c_str(), param.name.size());
			argument_doc["type"] = doc_type_name(elf_variant_type_or_nil(param.type));
			argument_docs.push_back(argument_doc);
		}
		method_doc["arguments"] = argument_docs;
		method_docs.push_back(method_doc);
	}
	class_doc["methods"] = method_docs;

	TypedArray<Dictionary> docs;
	docs.push_back(class_doc);
	return docs;
}
String ELFScript::_get_class_icon_path() const {
	return String("res://addons/godot_sandbox/Sandbox.svg");
}
bool ELFScript::_has_method(const StringName &p_method) const {
	bool result = has_function_name(p_method);
	if (!result) {
		static const StringName s_init("_init");
		result = stringname_equals(p_method, s_init);
	}
	if constexpr (VERBOSE_ELFSCRIPT) {
		printf("ELFScript::_has_method: method %s => %s\n",
			String(p_method).utf8().ptr(), result ? "true" : "false");
	}

	return result;
}
bool ELFScript::_has_static_method(const StringName &p_method) const {
	return false;
}
Dictionary ELFScript::_get_method_info(const StringName &p_method) const {
	if (const gdscript::FunctionSignature *sig = find_signature(p_method)) {
		return elf_signature_to_method_dict(*sig);
	}
	for (int i = 0; i < functions.size(); i++) {
		Dictionary function = functions[i];
		if (StringName(function.get("name", "")) == p_method) {
			return function;
		}
	}
	for (const String &function : function_names) {
		if (function == p_method) {
			if constexpr (VERBOSE_ELFSCRIPT) {
				printf("ELFScript::_get_method_info: method %s\n", p_method.to_ascii_buffer().ptr());
			}
			Dictionary method;
			method["name"] = function;
			method["args"] = Array();
			method["default_args"] = Array();
			Dictionary type;
			type["name"] = "type";
			type["type"] = Variant::Type::OBJECT;
			type["class_name"] = "Object";
			type["hint"] = PropertyHint::PROPERTY_HINT_NONE;
			type["hint_string"] = String("Return value");
			type["usage"] = PROPERTY_USAGE_DEFAULT;
			method["return"] = type;
			method["flags"] = METHOD_FLAG_VARARG;
			return method;
		}
	}
	return Dictionary();
}
bool ELFScript::_is_tool() const {
	if (has_script_metadata) {
		return script_metadata.is_tool;
	}
	return true;
}
bool ELFScript::_is_valid() const {
	return true;
}
bool ELFScript::_is_abstract() const {
	return false;
}
ScriptLanguage *ELFScript::_get_language() const {
	return get_elf_language();
}
bool ELFScript::_has_script_signal(const StringName &p_signal) const {
	return false;
}
TypedArray<Dictionary> ELFScript::_get_script_signal_list() const {
	return TypedArray<Dictionary>();
}

bool ELFScript::_has_property_default_value(const StringName &p_property) const {
	return false;
}
Variant ELFScript::_get_property_default_value(const StringName &p_property) const {
	return Variant();
}
TypedArray<Dictionary> ELFScript::_get_script_property_list() const {
	TypedArray<Dictionary> properties;
	for (const PropertyInfo &prop : Sandbox::create_sandbox_property_list()) {
		properties.push_back(prop_to_dict(prop));
	}
	return properties;
}

void ELFScript::_update_exports() {}
TypedArray<Dictionary> ELFScript::_get_script_method_list() const {
	if (has_script_metadata && !script_metadata.functions.empty()) {
		TypedArray<Dictionary> methods;
		for (const gdscript::FunctionSignature &sig : script_metadata.functions) {
			methods.push_back(elf_signature_to_method_dict(sig));
		}
		return methods;
	}
	if (!this->functions.is_empty()) {
		return functions;
	}
	TypedArray<Dictionary> functions_array;
	for (String function : function_names) {
		Dictionary method;
		method["name"] = function;
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
int32_t ELFScript::_get_member_line(const StringName &p_member) const {
	if (const gdscript::FunctionSignature *sig = find_signature(p_member)) {
		if (sig->line > 0) {
			return sig->line;
		}
	}
	PackedStringArray formatted_functions = _get_source_code().split("\n");
	for (int i = 0; i < formatted_functions.size(); i++) {
		if (formatted_functions[i].find(p_member) != -1) {
			return i + 1;
		}
	}
	return 0;
}
Dictionary ELFScript::_get_constants() const {
	return Dictionary();
}
TypedArray<StringName> ELFScript::_get_members() const {
	return TypedArray<StringName>();
}
bool ELFScript::_is_placeholder_fallback_enabled() const {
	return false;
}
Variant ELFScript::_get_rpc_config() const {
	return Variant();
}

const PackedByteArray &ELFScript::get_content() {
	return source_code;
}

String ELFScript::get_elf_programming_language() const {
	return elf_programming_language;
}

void ELFScript::set_file(const String &p_path) {
	if (p_path.is_empty()) {
		if constexpr (VERBOSE_ELFSCRIPT) {
			printf("ELFScript::set_file: Empty path provided, skipping.\n");
		}
		return;
	}
	// res://path/to/file.elf
	this->path = String(p_path);
	// path/to/file.elf as a C++ string
	CharString resless_path = p_path.replace("res://", "").utf8();
	this->std_path = std::string(resless_path.ptr(), resless_path.length());

	PackedByteArray new_source_code = FileAccess::get_file_as_bytes(this->path);
	if (new_source_code == source_code) {
		if constexpr (VERBOSE_ELFSCRIPT) {
			printf("ELFScript::set_file: No changes in %s\n", path.utf8().ptr());
		}
		return;
	} else if (new_source_code.is_empty()) {
		ERR_FAIL_MSG("ELFScript::set_file: Failed to load file '" + this->path + "'. The file is empty or does not exist.");
		return;
	}
	source_code = std::move(new_source_code);

	global_name = "Sandbox_" + path.get_basename().replace("res://", "").replace("/", "_").replace("-", "_").capitalize().replace(" ", "");
	Sandbox::BinaryInfo info = Sandbox::get_program_info_from_binary(source_code);
	this->function_names = std::move(info.functions);
	this->rebuild_function_name_set();
	this->functions.clear();

	this->elf_programming_language = info.language;
	this->elf_api_version = info.version;
	this->has_script_metadata = info.has_script_metadata;
	this->script_metadata = std::move(info.script_metadata);

	if constexpr (VERBOSE_ELFSCRIPT) {
		printf("ELFScript::set_file: %s Sandbox instances: %u\n", std_path.c_str(), sandbox_map[path].size());
	}
	for (Sandbox *sandbox : sandbox_map[path]) {
		sandbox->set_program(Ref<ELFScript>(this));
	}

	// Update the instance methods only if functions are still empty
	if (functions.is_empty()) {
		for (ELFScriptInstance *instance : this->instances) {
			instance->update_methods();
		}
	}
}

void ELFScript::set_public_api_functions(Array &&p_functions) {
	functions = std::move(p_functions);

	if constexpr (VERBOSE_ELFSCRIPT) {
		printf("ELFScript::set_public_api_functions: %s\n", path.utf8().ptr());
	}
	this->update_public_api_functions();
}

void ELFScript::rebuild_function_name_set() {
	function_name_set.clear();
	function_name_set.reserve(function_names.size());
	for (const String &function : function_names) {
		function_name_set.emplace(function);
	}
}

void ELFScript::update_public_api_functions() {
	// Update the function names
	function_names.clear();
	for (int i = 0; i < functions.size(); i++) {
		Dictionary func = functions[i];
		function_names.push_back(func["name"]);
	}
	this->rebuild_function_name_set();

	// Update the instance methods
	for (ELFScriptInstance *instance : this->instances) {
		instance->update_methods();
	}
}

String ELFScript::get_dockerized_program_path() const {
	// Get the absolute path without the file name
	String path = get_path().get_base_dir().replace("res://", "") + "/";
	String foldername = Docker::GetFolderName(get_path().get_base_dir());
	// Return the path to the folder with the name of the folder + .elf
	// Eg. res://foldername becomes foldername/foldername.elf
	return path + foldername + String(".elf");
}
