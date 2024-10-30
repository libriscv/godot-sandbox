#include "sandbox.h"

void Sandbox::set_restrictions(bool enable) {
	// It is allowed to enable restrictions during a VM call, but not to disable them.
	if (enable) {
		if (!m_just_in_time_allowed_classes.is_valid()) {
			m_just_in_time_allowed_classes = Callable(this, "restrictive_callback_function");
		}
		if (!m_just_in_time_allowed_objects.is_valid()) {
			m_just_in_time_allowed_objects = Callable(this, "restrictive_callback_function");
		}
		if (!m_just_in_time_allowed_methods.is_valid()) {
			m_just_in_time_allowed_methods = Callable(this, "restrictive_callback_function");
		}
		if (!m_just_in_time_allowed_properties.is_valid()) {
			m_just_in_time_allowed_properties = Callable(this, "restrictive_callback_function");
		}
		if (!m_just_in_time_allowed_resources.is_valid()) {
			m_just_in_time_allowed_resources = Callable(this, "restrictive_callback_function");
		}
	} else {
		if (this->is_in_vmcall()) {
			// Somehow a VM call is being made to disable restrictions, directly or indirectly.
			// That is a security risk, so we will not allow it.
			ERR_PRINT("Cannot disable restrictions during a VM call.");
			return;
		}
		m_just_in_time_allowed_classes = Callable();
		m_just_in_time_allowed_objects = Callable();
		m_just_in_time_allowed_methods = Callable();
		m_just_in_time_allowed_properties = Callable();
		m_just_in_time_allowed_resources = Callable();
	}
}

// clang-format off
bool Sandbox::get_restrictions() const {
	return m_just_in_time_allowed_classes.is_valid()
		&& m_just_in_time_allowed_objects.is_valid()
		&& m_just_in_time_allowed_methods.is_valid()
		&& m_just_in_time_allowed_properties.is_valid()
		&& m_just_in_time_allowed_resources.is_valid();
}
// clang-format on

void Sandbox::add_allowed_object(Object *obj) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot add allowed objects during a VM call.");
		return;
	}
	m_allowed_objects.insert(obj);
}

void Sandbox::remove_allowed_object(Object *obj) {
	m_allowed_objects.erase(obj);
}

void Sandbox::clear_allowed_objects() {
	// Clearing all allowed objects effectively disables the allowed objects list.
	// This is not allowed during a VM call.
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot clear allowed objects during a VM call.");
		return;
	}
	m_allowed_objects.clear();
}

void Sandbox::set_object_allowed_callback(const Callable &callback) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot set object allowed callback during a VM call.");
		return;
	}
	m_just_in_time_allowed_objects = callback;
}

void Sandbox::set_class_allowed_callback(const Callable &callback) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot set class allowed callback during a VM call.");
		return;
	}
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
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot set resource allowed callback during a VM call.");
		return;
	}
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

bool Sandbox::is_allowed_method(Object *obj, const Variant &method) const {
	// If the callable is valid, call it to allow the user to decide
	if (m_just_in_time_allowed_methods.is_valid()) {
		return m_just_in_time_allowed_methods.call(this, obj, method);
	}
	// If the callable is not valid, allow all methods
	return true;
}

void Sandbox::set_method_allowed_callback(const Callable &callback) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot set method allowed callback during a VM call.");
		return;
	}
	m_just_in_time_allowed_methods = callback;
}

bool Sandbox::is_allowed_property(Object *obj, const Variant &property, bool is_set) const {
	// If the callable is valid, call it to allow the user to decide
	if (m_just_in_time_allowed_properties.is_valid()) {
		return m_just_in_time_allowed_properties.call(this, obj, property, is_set);
	}
	// If the callable is not valid, allow all properties
	return true;
}

void Sandbox::set_property_allowed_callback(const Callable &callback) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot set property allowed callback during a VM call.");
		return;
	}
	m_just_in_time_allowed_properties = callback;
}
