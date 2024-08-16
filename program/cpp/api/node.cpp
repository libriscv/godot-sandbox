#include "node.hpp"

#include "syscalls.h"

MAKE_SYSCALL(ECALL_GET_NODE, uint64_t, sys_get_node, uint64_t, const char *, size_t);
MAKE_SYSCALL(ECALL_NODE, void, sys_node, Node_Op, uint64_t, Variant *);

Node::Node(const std::string &path) :
		Object(sys_get_node(0, path.c_str(), path.size())) {
}

std::string Node::get_name() const {
	Variant var;
	sys_node(Node_Op::GET_NAME, address(), &var);
	return std::string(var);
}

std::string Node::get_path() const {
	Variant var;
	sys_node(Node_Op::GET_PATH, address(), &var);
	return std::string(var);
}

Node Node::get_parent() const {
	Variant var;
	sys_node(Node_Op::GET_PARENT, address(), &var);
	return var.as_node();
}

void Node::add_child(const Node &child, bool deferred) {
	Variant v(int64_t(child.address()));
	sys_node(deferred ? Node_Op::ADD_CHILD_DEFERRED : Node_Op::ADD_CHILD, this->address(), &v);
}

std::vector<Node> Node::get_children() const {
	std::vector<Node> children;
	sys_node(Node_Op::GET_CHILDREN, address(), (Variant *)&children);
	return children;
}

Node Node::get_node(const std::string &name) const {
	return Node(sys_get_node(address(), name.c_str(), name.size()));
}

void Node::queue_free() {
	sys_node(Node_Op::QUEUE_FREE, address(), nullptr);
	//this->m_address = 0;
}

Node Node::duplicate() const {
	Variant result;
	sys_node(Node_Op::DUPLICATE, this->address(), &result);
	return result.as_node();
}
