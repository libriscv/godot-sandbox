#pragma once

#include "../gdscript/compiler/function_signature.h"
#include <godot_cpp/core/object.hpp>

using namespace godot;

// Variant::Type. Godot spells "any Variant" as NIL plus NIL_IS_VARIANT.
inline Variant::Type variant_type_or_nil(int32_t p_type) {
	if (p_type < 0 || p_type >= Variant::Type::VARIANT_MAX) {
		return Variant::Type::NIL;
	}
	return Variant::Type(p_type);
}

// The value the host passes for an argument the caller left out. Only the kinds
// the compiler folds appear here; a parameter it could not fold is required, so
// nothing ever asks for its default.
inline Variant default_argument_value(const gdscript::FunctionParameter &p_param) {
	using DefaultKind = gdscript::FunctionParameter::DefaultKind;
	switch (p_param.default_kind) {
		case DefaultKind::INT:
			return int64_t(std::get<int64_t>(p_param.default_value));
		case DefaultKind::FLOAT:
			return std::get<double>(p_param.default_value);
		case DefaultKind::BOOL:
			return std::get<bool>(p_param.default_value);
		case DefaultKind::STRING: {
			const std::string &text = std::get<std::string>(p_param.default_value);
			return String::utf8(text.c_str(), int64_t(text.size()));
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

// Godot's convention: the defaults cover the last N arguments, which is exactly
// the run of parameters past the required ones.
inline MethodInfo method_info_from_signature(const gdscript::FunctionSignature &p_signature,
		const String &p_name) {
	MethodInfo method(p_name);
	method.flags = METHOD_FLAG_NORMAL | (p_signature.is_static ? METHOD_FLAG_STATIC : 0);
	method.return_val.usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT;
	method.return_val.type = variant_type_or_nil(p_signature.return_type);
	if (!p_signature.return_class_name.empty()) {
		method.return_val.class_name = StringName(String::utf8(
			p_signature.return_class_name.c_str(), p_signature.return_class_name.size()));
		method.return_val.hint_string = String::utf8(
			p_signature.return_class_name.c_str(), p_signature.return_class_name.size());
	}
	for (const gdscript::FunctionParameter &param : p_signature.parameters) {
		PropertyInfo argument;
		argument.name = String::utf8(param.name.c_str(), param.name.size());
		argument.type = variant_type_or_nil(param.type);
		if (!param.class_name.empty()) {
			argument.class_name = StringName(String::utf8(param.class_name.c_str(), param.class_name.size()));
			argument.hint_string = String::utf8(param.class_name.c_str(), param.class_name.size());
		}
		argument.usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT;
		method.arguments.push_back(std::move(argument));
	}
	for (size_t i = p_signature.required_arguments; i < p_signature.parameters.size(); i++) {
		method.default_arguments.push_back(default_argument_value(p_signature.parameters[i]));
	}
	return method;
}
