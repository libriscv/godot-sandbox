#include "sandbox.h"

#include "fast_cast.hpp"

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

void Sandbox::add_allowed_object(godot::Object *obj) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot add allowed objects during a VM call.");
		return;
	}
	if (obj == nullptr) {
		ERR_PRINT("Cannot allow a null object.");
		return;
	}
	const uint64_t id = engine_object_id(obj);
	if (id == 0) {
		ERR_PRINT("Cannot allow an object with no engine instance.");
		return;
	}
	m_allowed_objects.insert_or_assign(id, engine_ptr(obj));
	// Keep RefCounted entries alive. The list only stores an id and an address, neither of
	// which owns anything, so a Resource the caller stops holding would be freed and the
	// next object allocated could land on the same address.
	if (RefCounted *ref = fast_cast_to<RefCounted>(obj))
		m_allowed_object_refs.insert_or_assign(id, Ref<RefCounted>(ref));
}

void Sandbox::remove_allowed_object(godot::Object *obj) {
	const uint64_t id = engine_object_id(obj);
	if (id == 0)
		return;
	m_allowed_objects.erase(id);
	m_allowed_object_refs.erase(id);
}

void Sandbox::clear_allowed_objects() {
	// Clearing all allowed objects effectively disables the allowed objects list.
	// This is not allowed during a VM call.
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot clear allowed objects during a VM call.");
		return;
	}
	m_allowed_objects.clear();
	m_allowed_object_refs.clear();
}

godot::Object *Sandbox::get_explicitly_allowed_object(uintptr_t engine_object) const {
	if (engine_object == 0)
		return nullptr;
	for (const auto &[id, ptr] : m_allowed_objects) {
		if (ptr != engine_object)
			continue;
		// The address matched, but an address is only as good as the object that still
		// occupies it. Ask the engine for the object the id names: it answers null once
		// the object is gone, without anyone having to dereference a stale pointer. Keep
		// looking on a miss -- a freed entry can share its address with a live one.
		GDExtensionObjectPtr live = internal::gdextension_interface_object_get_instance_from_id(id);
		if (live == nullptr || uintptr_t(live) != engine_object)
			continue;
		return internal::get_object_instance_binding(static_cast<GodotObject *>(live));
	}
	return nullptr;
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
