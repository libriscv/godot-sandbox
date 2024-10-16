#include "sandbox.h"

void Sandbox::enable_restrictions() {
	m_allowed_objects.insert(nullptr);
	m_just_in_time_allowed_classes = Callable(this, "restrictive_callback_function");
	m_just_in_time_allowed_objects = Callable(this, "restrictive_callback_function");
	m_just_in_time_allowed_methods = Callable(this, "restrictive_callback_function");
	m_just_in_time_allowed_properties = Callable(this, "restrictive_callback_function");
	m_just_in_time_allowed_resources = Callable(this, "restrictive_callback_function");
}

void Sandbox::disable_all_restrictions() {
	m_allowed_objects.clear();
	m_just_in_time_allowed_classes = Callable();
	m_just_in_time_allowed_objects = Callable();
	m_just_in_time_allowed_methods = Callable();
	m_just_in_time_allowed_properties = Callable();
	m_just_in_time_allowed_resources = Callable();
}

void Sandbox::allow_object(godot::Object *obj) {
	m_allowed_objects.insert(obj);
}

void Sandbox::remove_allowed_object(godot::Object *obj) {
	m_allowed_objects.erase(obj);
}

void Sandbox::set_object_allowed_callback(const Callable &callback) {
	m_just_in_time_allowed_objects = callback;
}

void Sandbox::set_class_allowed_callback(const Callable &callback) {
	m_just_in_time_allowed_classes = callback;
}

bool Sandbox::is_allowed_class(const String &name) const {
	// If the callable is valid, call it to allow the user to decide
	if (m_just_in_time_allowed_classes.is_valid()) {
		return m_just_in_time_allowed_classes.call(this, name);
	}
	// If the callable is not valid, allow all classes
	return true;
}

void Sandbox::set_resource_allowed_callback(const Callable &callback) {
	this->m_just_in_time_allowed_resources = callback;
}

bool Sandbox::is_allowed_resource(const String &path) const {
	// If the callable is valid, call it to allow the user to decide
	if (this->m_just_in_time_allowed_resources.is_valid()) {
		return this->m_just_in_time_allowed_resources.call(this, path);
	}
	// If the callable is not valid, allow all resources
	return true;
}

bool Sandbox::is_allowed_method(godot::Object *obj, const Variant &method) const {
	// If the callable is valid, call it to allow the user to decide
	if (m_just_in_time_allowed_methods.is_valid()) {
		return m_just_in_time_allowed_methods.call(this, obj, method);
	}
	// If the callable is not valid, allow all methods
	return true;
}

void Sandbox::set_method_allowed_callback(const Callable &callback) {
	m_just_in_time_allowed_methods = callback;
}

bool Sandbox::is_allowed_property(godot::Object *obj, const Variant &property) const {
	// If the callable is valid, call it to allow the user to decide
	if (m_just_in_time_allowed_properties.is_valid()) {
		return m_just_in_time_allowed_properties.call(this, obj, property);
	}
	// If the callable is not valid, allow all properties
	return true;
}

void Sandbox::set_property_allowed_callback(const Callable &callback) {
	m_just_in_time_allowed_properties = callback;
}
