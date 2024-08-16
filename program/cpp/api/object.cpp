#include "object.hpp"

#include "syscalls.h"
#include "variant.hpp"

MAKE_SYSCALL(ECALL_GET_OBJ, uint64_t, sys_get_obj, const char *, size_t);
MAKE_SYSCALL(ECALL_OBJ, void, sys_obj, Object_Op, uint64_t, Variant *);
MAKE_SYSCALL(ECALL_OBJ_CALLP, void, sys_obj_callp, uint64_t, const char *, size_t, bool, Variant *, const Variant *, unsigned);

Object::Object(const std::string &name) :
		m_address{ sys_get_obj(name.c_str(), name.size()) } {
}

std::vector<std::string> Object::get_method_list() const {
	std::vector<std::string> methods;
	sys_obj(Object_Op::GET_METHOD_LIST, address(), (Variant *)&methods);
	return methods;
}

Variant Object::callv(const std::string &method, bool deferred, const Variant *argv, unsigned argc) {
	Variant var;
	sys_obj_callp(address(), method.c_str(), method.size(), deferred, &var, argv, argc);
	return var;
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
