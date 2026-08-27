#pragma once

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

// A property, then a method, in the shape Godot reads them back in:
// PropertyInfo::from_dict() and MethodInfo::from_dict() look for these exact
// keys, and quietly leave out anything spelled differently -- which is how a
// method list can look complete and still report no arguments.
inline Dictionary property_dict(const godot::PropertyInfo &p_info) {
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
inline Dictionary method_dict(const godot::MethodInfo &p_method, const String &p_return_name = "type") {
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
