#include "sandbox.h"

#include "fast_cast.hpp"

#ifndef SAFEGDSCRIPT_DISABLED
void safegdscript_class_restrictions_changed(const Sandbox &p_sandbox);
#else
static void safegdscript_class_restrictions_changed(const Sandbox &) {}
#endif

Sandbox::Restrictions &Sandbox::restrictions() {
	if (m_restrictions == nullptr)
		m_restrictions = std::make_unique<Restrictions>();
	return *m_restrictions;
}

void Sandbox::set_restriction_callback(Callable Restrictions::*p_slot, RestrictionBit p_bit, const Callable &p_callable) {
	const bool valid = p_callable.is_valid();
	if (m_restrictions != nullptr || valid)
		restrictions().*p_slot = p_callable;
	set_restriction_flag(p_bit, valid);
}

void Sandbox::set_restrictions(bool enable) {
	if (this->is_in_vmcall()) {
		ERR_PRINT("Cannot change restrictions during a VM call.");
		return;
	}
	static constexpr std::pair<Callable Restrictions::*, RestrictionBit> slots[] = {
		{ &Restrictions::classes, RESTRICT_CLASSES },
		{ &Restrictions::objects, RESTRICT_OBJECTS },
		{ &Restrictions::methods, RESTRICT_METHODS },
		{ &Restrictions::properties, RESTRICT_PROPERTIES },
		{ &Restrictions::resources, RESTRICT_RESOURCES },
	};
	for (const auto &[slot, bit] : slots) {
		if (enable) {
			if ((m_restriction_flags & bit) == 0)
				set_restriction_callback(slot, bit, Callable(this, "restrictive_callback_function"));
		} else {
			set_restriction_callback(slot, bit, Callable());
		}
	}
	safegdscript_class_restrictions_changed(*this);
}

bool Sandbox::get_restrictions() const {
	static constexpr uint8_t all = RESTRICT_CLASSES | RESTRICT_OBJECTS | RESTRICT_METHODS | RESTRICT_PROPERTIES | RESTRICT_RESOURCES;
	return (m_restriction_flags & all) == all;
}

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
	Restrictions &r = restrictions();
	r.allowed_objects.insert(id);
	if (RefCounted *ref = fast_cast_to<RefCounted>(obj))
		r.allowed_object_refs.insert_or_assign(id, Ref<RefCounted>(ref));
	set_restriction_flag(RESTRICT_ALLOWED_OBJECTS, true);
	safegdscript_class_restrictions_changed(*this);
}

// Permitted mid-vmcall; scoped handles remain valid for the rest of the call.
void Sandbox::remove_allowed_object(godot::Object *obj) {
	const uint64_t id = engine_object_id(obj);
	if (id == 0 || m_restrictions == nullptr)
		return;
	m_restrictions->allowed_objects.erase(id);
	m_restrictions->allowed_object_refs.erase(id);
	set_restriction_flag(RESTRICT_ALLOWED_OBJECTS, !m_restrictions->allowed_objects.empty());
	safegdscript_class_restrictions_changed(*this);
}

void Sandbox::clear_allowed_objects() {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot clear allowed objects during a VM call.");
		return;
	}
	if (m_restrictions != nullptr) {
		m_restrictions->allowed_objects.clear();
		m_restrictions->allowed_object_refs.clear();
	}
	set_restriction_flag(RESTRICT_ALLOWED_OBJECTS, false);
	safegdscript_class_restrictions_changed(*this);
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
	set_restriction_callback(&Restrictions::objects, RESTRICT_OBJECTS, callback);
	safegdscript_class_restrictions_changed(*this);
}

void Sandbox::set_class_allowed_callback(const Callable &callback) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot set class allowed callback during a VM call.");
		return;
	}
	set_restriction_callback(&Restrictions::classes, RESTRICT_CLASSES, callback);
	safegdscript_class_restrictions_changed(*this);
}

bool Sandbox::is_allowed_class(const String &name) const {
	if ((m_restriction_flags & RESTRICT_CLASSES) != 0) {
		return m_restrictions->classes.call(this, name);
	}
	return true;
}

bool Sandbox::is_class_access_restricted() const {
	return (m_restriction_flags & RESTRICT_CLASSES) != 0 &&
			m_restrictions->classes == Callable(const_cast<Sandbox *>(this), "restrictive_callback_function");
}

void Sandbox::set_resource_allowed_callback(const Callable &callback) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot set resource allowed callback during a VM call.");
		return;
	}
	set_restriction_callback(&Restrictions::resources, RESTRICT_RESOURCES, callback);
	safegdscript_class_restrictions_changed(*this);
}

bool Sandbox::is_allowed_resource(const String &path) const {
	if ((m_restriction_flags & RESTRICT_RESOURCES) != 0) {
		return m_restrictions->resources.call(this, path);
	}
	return true;
}

void Sandbox::set_method_allowed_callback(const Callable &callback) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot set method allowed callback during a VM call.");
		return;
	}
	set_restriction_callback(&Restrictions::methods, RESTRICT_METHODS, callback);
	safegdscript_class_restrictions_changed(*this);
}

void Sandbox::set_property_allowed_callback(const Callable &callback) {
	if (is_in_vmcall()) {
		ERR_PRINT("Cannot set property allowed callback during a VM call.");
		return;
	}
	set_restriction_callback(&Restrictions::properties, RESTRICT_PROPERTIES, callback);
	safegdscript_class_restrictions_changed(*this);
}
