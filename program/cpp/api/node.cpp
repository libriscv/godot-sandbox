#include "node.hpp"

#include "syscalls.h"

MAKE_SYSCALL(ECALL_GET_NODE, uint64_t, sys_get_node, uint64_t, const char *, size_t);
MAKE_SYSCALL(ECALL_NODE, void, sys_node, Node_Op, uint64_t, Variant *);
MAKE_SYSCALL(ECALL_NODE_CREATE, uint64_t, sys_node_create, Node_Create_Shortlist, const char *, size_t, const char *, size_t);

static uint64_t sys_fast_get_node(uint64_t parent, const char *path, size_t path_len) {
	// uses sys_get_node
	register uint64_t addr asm("a0") = parent;
	register const char *path_ptr asm("a1") = path;
	register size_t path_size asm("a2") = path_len;
	register int syscall_number asm("a7") = ECALL_GET_NODE;

	asm volatile(
		"ecall"
		: "+r"(addr)
		: "r"(path_ptr), "m"(*path_ptr), "r"(path_size), "r"(syscall_number)
	);

	return addr;
}

Node::Node(std::string_view path) :
		Object(sys_fast_get_node(0, path.begin(), path.size())) {
}

Variant Node::get_name() const {
	Variant var;
	sys_node(Node_Op::GET_NAME, address(), &var);
	return var;
}

void Node::set_name(Variant name) {
	sys_node(Node_Op::SET_NAME, address(), &name);
}

Variant Node::get_path() const {
	Variant var;
	sys_node(Node_Op::GET_PATH, address(), &var);
	return var;
}

Node Node::get_parent() const {
	Variant var;
	sys_node(Node_Op::GET_PARENT, address(), &var);
	return var.as_node();
}

unsigned Node::get_child_count() const {
	Variant var;
	sys_node(Node_Op::GET_CHILD_COUNT, address(), &var);
	return int64_t(var);
}

Node Node::get_child(unsigned index) const {
	Variant var;
	sys_node(Node_Op::GET_CHILD, address(), &var);
	return var.as_node();
}

void Node::add_child(const Node &child, bool deferred) {
	Variant v(int64_t(child.address()));
	sys_node(deferred ? Node_Op::ADD_CHILD_DEFERRED : Node_Op::ADD_CHILD, this->address(), &v);
}

void Node::add_sibling(const Node &sibling, bool deferred) {
	Variant v(int64_t(sibling.address()));
	sys_node(deferred ? Node_Op::ADD_SIBLING_DEFERRED : Node_Op::ADD_SIBLING, this->address(), &v);
}

void Node::move_child(const Node &child, unsigned index) {
	Variant v(int64_t(child.address()));
	sys_node(Node_Op::MOVE_CHILD, this->address(), &v);
}

void Node::remove_child(const Node &child, bool deferred) {
	Variant v(int64_t(child.address()));
	sys_node(deferred ? Node_Op::REMOVE_CHILD_DEFERRED : Node_Op::REMOVE_CHILD, this->address(), &v);
}

std::vector<Node> Node::get_children() const {
	std::vector<Node> children;
	sys_node(Node_Op::GET_CHILDREN, address(), (Variant *)&children);
	return children;
}

Node Node::get_node(const std::string &name) const {
	return Node(sys_fast_get_node(address(), name.c_str(), name.size()));
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

Node Node::Create(std::string_view path) {
	return Node(sys_node_create(Node_Create_Shortlist::CREATE_NODE, nullptr, 0, path.data(), path.size()));
}
