#include "node2d.hpp"

#include "syscalls.h"

// API call to get/set Node2D properties.
// void sys_node2d(Node2D_Op, const char *name, size_t name_size, Variant *v)
MAKE_SYSCALL(ECALL_NODE2D, void, sys_node2d, Node2D_Op, const char *, size_t, Variant *);

static inline void node2d(Node2D_Op op, const std::string &name, const Variant &value) {
	sys_node2d(op, name.c_str(), name.size()+1, const_cast<Variant *>(&value));
}

Variant Node2D::get_position() const {
	Variant var;
	node2d(Node2D_Op::GET_POSITION, name, var);
	return var;
}

void Node2D::set_position(const Variant &value) {
	node2d(Node2D_Op::SET_POSITION, name, value);
}

Variant Node2D::get_rotation() const {
	Variant var;
	node2d(Node2D_Op::GET_ROTATION, name, var);
	return var;
}

void Node2D::set_rotation(const Variant &value) {
	node2d(Node2D_Op::SET_ROTATION, name, value);
}

Variant Node2D::get_scale() const {
	Variant var;
	node2d(Node2D_Op::GET_SCALE, name, var);
	return var;
}

void Node2D::set_scale(const Variant &value) {
	node2d(Node2D_Op::SET_SCALE, name, value);
}

Variant Node2D::get_skew() const {
	Variant var;
	node2d(Node2D_Op::GET_SKEW, name, var);
	return var;
}

void Node2D::set_skew(const Variant &value) {
	node2d(Node2D_Op::SET_SKEW, name, value);
}
