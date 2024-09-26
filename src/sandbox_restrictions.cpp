#include "sandbox.h"

void Sandbox::enable_restrictions() {
	m_allowed_objects.insert(nullptr);
	m_allowed_classes.insert("Dummy");
}

void Sandbox::disable_all_restrictions() {
	m_allowed_objects.clear();
	m_allowed_classes.clear();
}

void Sandbox::allow_object(godot::Object *obj) {
	m_allowed_objects.insert(obj);
}

void Sandbox::remove_allowed_object(godot::Object *obj) {
	m_allowed_objects.erase(obj);
}

void Sandbox::allow_class(const String &name) {
	m_allowed_classes.insert(name);
}

void Sandbox::remove_allowed_class(const String &name) {
	m_allowed_classes.erase(name);
}
