#include "object.hpp"

#include "syscalls.h"
#include "variant.hpp"

MAKE_SYSCALL(ECALL_GET_OBJ, uint64_t, sys_get_obj, const char *, size_t);
MAKE_SYSCALL(ECALL_OBJ, void, sys_obj, Object_Op, uint64_t, Variant *);
MAKE_SYSCALL(ECALL_OBJ_CALLP, void, sys_obj_callp, uint64_t, const char *, size_t, bool, Variant *, const Variant *, unsigned);

static_assert(sizeof(std::vector<std::string>) == 24, "std::vector<std::string> is not 24 bytes");
static_assert(sizeof(std::string) == 32, "std::string is not 32 bytes");

Object::Object(const std::string &name) :
		m_address{ sys_get_obj(name.c_str(), name.size()) } {
}

std::vector<std::string> Object::get_method_list() const {
	std::vector<std::string> methods;
	sys_obj(Object_Op::GET_METHOD_LIST, address(), (Variant *)&methods);
	return methods;
}

Variant Object::get(const std::string &name) const {
	Variant vars[2];
	vars[0] = name;
	sys_obj(Object_Op::GET, address(), vars);
	return std::move(vars[1]);
}

void Object::set(const std::string &name, const Variant &value) {
	Variant vars[2];
	vars[0] = name;
	vars[1] = value;
	sys_obj(Object_Op::SET, address(), vars);
}

std::vector<std::string> Object::get_property_list() const {
	std::vector<std::string> properties;
	sys_obj(Object_Op::GET_PROPERTY_LIST, address(), (Variant *)&properties);
	return properties;
}

void Object::connect(Object target, const std::string &signal, std::string_view method) {
	Variant vars[3];
	vars[0] = target.address();
	vars[1] = signal;
	vars[2] = std::string(method);
	sys_obj(Object_Op::CONNECT, address(), vars);
}

void Object::disconnect(Object target, const std::string &signal, std::string_view method) {
	Variant vars[3];
	vars[0] = target.address();
	vars[1] = signal;
	vars[2] = std::string(method);
	sys_obj(Object_Op::DISCONNECT, address(), vars);
}

std::vector<std::string> Object::get_signal_list() const {
	std::vector<std::string> signals;
	sys_obj(Object_Op::GET_SIGNAL_LIST, address(), (Variant *)&signals);
	return signals;
}
