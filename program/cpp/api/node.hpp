#pragma once
#include "object.hpp"

struct Node : public Object {
	/// @brief Construct a Node object from an existing in-scope Node object.
	/// @param addr The address of the Node object.
	constexpr Node(uint64_t addr) : Object{addr} {}
	Node(Object obj) : Object{obj.address()} {}

	/// @brief Construct a Node object from a path.
	/// @param path The path to the Node object.
	Node(std::string_view path);

	/// @brief Get the name of the node.
	/// @return The name of the node.
	Variant get_name() const;

	/// @brief Set the name of the node.
	/// @param name The new name of the node.
	void set_name(Variant name);

	/// @brief Get the path of the node, relative to the root node.
	/// @return The path of the node.
	Variant get_path() const;

	/// @brief Get the parent of the node.
	/// @return The parent node.
	Node get_parent() const;

	/// @brief Get the Node object at the given path, relative to this node.
	/// @param path The path to the Node object.
	/// @return The Node object.
	Node get_node(const std::string &path) const;

	/// @brief Get the number of children of the node.
	/// @return The number of children.
	unsigned get_child_count() const;

	/// @brief Get the child of the node at the given index.
	/// @param index The index of the child.
	/// @return The child node.
	Node get_child(unsigned index) const;

	/// @brief Add a child to the node.
	/// @param child The child node to add.
	/// @param deferred If true, the child will be added next frame.
	void add_child(const Node &child, bool deferred = false);

	/// @brief Add a sibling to the node.
	/// @param sibling The sibling node to add.
	/// @param deferred If true, the sibling will be added next frame.
	void add_sibling(const Node &sibling, bool deferred = false);

	/// @brief Move a child of the node to a new index.
	/// @param child The child node to move.
	/// @param index The new index of the child.
	void move_child(const Node &child, unsigned index);

	/// @brief Remove a child from the node. The child is *not* freed.
	/// @param child The child node to remove.
	/// @param deferred If true, the child will be removed next frame.
	void remove_child(const Node &child, bool deferred = false);

	/// @brief Get a list of children of the node.
	/// @return A list of children nodes.
	std::vector<Node> get_children() const;

	/// @brief Remove this node from its parent, freeing it.
	/// @note This is a potentially deferred operation.
	void queue_free();

	/// @brief  Duplicate the node.
	/// @return A new Node object with the same properties and children.
	Node duplicate() const;

	/// @brief Create a new Node object.
	/// @param path The path to the Node object.
	/// @return The Node object.
	static Node create(std::string_view path);
};

inline Node Variant::as_node() const {
	if (get_type() == Variant::OBJECT)
		return Node{uintptr_t(v.i)};
	else if (get_type() == Variant::NODE_PATH)
		return Node{this->internal_fetch_string()};

	api_throw("std::bad_cast", "Variant is not a Node or NodePath", this);
}

inline Node Object::as_node() const {
	return Node{address()};
}

template <typename T>
static inline T cast_to(const Variant &var) {
	if (var.get_type() == Variant::OBJECT)
		return T{uintptr_t(var)};
	api_throw("std::bad_cast", "Variant is not an Object", &var);
}

inline Variant::Variant(const Node &node) {
	m_type = OBJECT;
	v.i = node.address();
}
