#include "node2d.hpp"

#include "syscalls.h"

// API call to get/set Node2D properties.
// void sys_node2d(Node2D_Op, const char *name, size_t name_size, Variant *v)
MAKE_SYSCALL(ECALL_GET_NODE, uint64_t, sys_get_node, const char *, size_t);
MAKE_SYSCALL(ECALL_NODE, void, sys_node, Node_Op, uint64_t, Variant *);
MAKE_SYSCALL(ECALL_NODE2D, void, sys_node2d, Node2D_Op, uint64_t, Variant *);

static inline void node2d(Node2D_Op op, uint64_t address, const Variant &value) {
	sys_node2d(op, address, const_cast<Variant *>(&value));
}

Node2D::Node2D(const std::string &name) {
	this->m_address = sys_get_node(name.c_str(), name.size());
}

Variant Node2D::get_position() const {
	Variant var;
	node2d(Node2D_Op::GET_POSITION, address(), var);
	return var;
}

void Node2D::set_position(const Variant &value) {
	node2d(Node2D_Op::SET_POSITION, address(), value);
}

Variant Node2D::get_rotation() const {
	Variant var;
	node2d(Node2D_Op::GET_ROTATION, address(), var);
	return var;
}

void Node2D::set_rotation(const Variant &value) {
	node2d(Node2D_Op::SET_ROTATION, address(), value);
}

Variant Node2D::get_scale() const {
	Variant var;
	node2d(Node2D_Op::GET_SCALE, address(), var);
	return var;
}

void Node2D::set_scale(const Variant &value) {
	node2d(Node2D_Op::SET_SCALE, address(), value);
}

Variant Node2D::get_skew() const {
	Variant var;
	node2d(Node2D_Op::GET_SKEW, address(), var);
	return var;
}

void Node2D::set_skew(const Variant &value) {
	node2d(Node2D_Op::SET_SKEW, address(), value);
}

void Node2D::queue_free() {
	sys_node(Node_Op::QUEUE_FREE, address(), nullptr);
	//this->m_address = 0;
}

Node2D Node2D::duplicate() const {
	Variant result;
	sys_node(Node_Op::DUPLICATE, this->address(), &result);
	return result.as_node2d();
}

void Node2D::add_child(const Node2D &child, bool deferred) {
	Variant v(int64_t(child.address()));
	sys_node(deferred ? Node_Op::ADD_CHILD_DEFERRED : Node_Op::ADD_CHILD, this->address(), &v);
}

std::string Node2D::get_name() const {
	Variant var;
	sys_node(Node_Op::GET_NAME, address(), &var);
	return std::string(var);
}

std::string Node2D::get_path() const {
	Variant var;
	sys_node(Node_Op::GET_PATH, address(), &var);
	return std::string(var);
}

Node2D Node2D::get_parent() const {
	Variant var;
	sys_node(Node_Op::GET_PARENT, address(), &var);
	return var.as_node2d();
}
