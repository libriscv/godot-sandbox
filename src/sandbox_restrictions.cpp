#include "sandbox.h"

#include "fast_cast.hpp"

void safegdscript_class_restrictions_changed(const Sandbox &p_sandbox);

void Sandbox::set_restrictions(bool enable) {
	if (this->is_in_vmcall()) {
		ERR_PRINT("Cannot change restrictions during a VM call.");
		return;
	}
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
		m_just_in_time_allowed_classes = Callable();
		m_just_in_time_allowed_objects = Callable();
		m_just_in_time_allowed_methods = Callable();
		m_just_in_time_allowed_properties = Callable();
		m_just_in_time_allowed_resources = Callable();
	}
	safegdscript_class_restrictions_changed(*this);
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

// Permitted mid-vmcall: monotone, host-only, no iterator held across re-entrance.
void Sandbox::add_allowed_object(godot::Object *obj) {
	if (obj == nullptr) {
		ERR_PRINT("Cannot allow a null object.");
		return;
	}
	const uint64_t id = engine_object_id(obj);
	if (id == 0) {
		ERR_PRINT("Cannot allow an object with no engine instance.");
		return;
	}
	// First entry turns checking on; same mid-call transition set_restrictions() refuses.
	if (this->is_object_access_unrestricted() && this->is_in_vmcall()) {
		ERR_PRINT("Cannot begin restricting objects during a VM call: allow the first object before the call.");
		return;
	}
	m_allowed_objects.insert(id);
	if (RefCounted *ref = fast_cast_to<RefCounted>(obj))
		m_allowed_object_refs.insert_or_assign(id, Ref<RefCounted>(ref));
}

// Permitted mid-vmcall; scoped handles remain valid for the rest of the call.
void Sandbox::remove_allowed_object(godot::Object *obj) {
	const uint64_t id = engine_object_id(obj);
	if (id == 0)
		return;
	m_allowed_objects.erase(id);
	m_allowed_object_refs.erase(id);
}

void Sandbox::clear_allowed_objects() {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot clear allowed objects during a VM call.");
		return;
	}
	m_allowed_objects.clear();
	m_allowed_object_refs.clear();
}

godot::Object *Sandbox::get_explicitly_allowed_object(uint64_t object_id) const {
	if (!this->is_allowed_object_id(object_id))
		return nullptr;
	return this->resolve_live_object(object_id);
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
	safegdscript_class_restrictions_changed(*this);
}

bool Sandbox::is_allowed_class(const String &name) const {
	// If the callable is valid, call it to allow the user to decide
	if (m_just_in_time_allowed_classes.is_valid()) {
		return m_just_in_time_allowed_classes.call(this, name);
	}
	// If the callable is not valid, allow all classes
	return true;
}

bool Sandbox::is_class_access_restricted() const {
	return m_just_in_time_allowed_classes.callable ==
			Callable(const_cast<Sandbox *>(this), "restrictive_callback_function");
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

void Sandbox::set_method_allowed_callback(const Callable &callback) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot set method allowed callback during a VM call.");
		return;
	}
	m_just_in_time_allowed_methods = callback;
}

void Sandbox::set_property_allowed_callback(const Callable &callback) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot set property allowed callback during a VM call.");
		return;
	}
	m_just_in_time_allowed_properties = callback;
}
