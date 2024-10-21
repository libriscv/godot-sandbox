#pragma once
#include "node.hpp"
struct Transform2D;

// Node2D: Contains 2D transformations.
// Such as: position, rotation, scale, and skew.
struct Node2D : public Node {
	/// @brief Construct a Node2D object from an existing in-scope Node object.
	/// @param addr The address of the Node2D object.
	constexpr Node2D(uint64_t addr) : Node(addr) {}
	Node2D(Object obj) : Node(obj) {}
	Node2D(Node node) : Node(node) {}

	/// @brief Construct a Node2D object from a path.
	/// @param path The path to the Node2D object.
	Node2D(std::string_view path) : Node(path) {}

	/// @brief Get the position of the node.
	/// @return The position of the node.
	Vector2 get_position() const;
	/// @brief Set the position of the node.
	/// @param value The new position of the node.
	void set_position(const Vector2 &value);

	/// @brief Get the rotation of the node.
	/// @return The rotation of the node.
	real_t get_rotation() const;
	/// @brief Set the rotation of the node.
	/// @param value The new rotation of the node.
	void set_rotation(real_t value);

	/// @brief Get the scale of the node.
	/// @return The scale of the node.
	Vector2 get_scale() const;
	/// @brief Set the scale of the node.
	/// @param value The new scale of the node.
	void set_scale(const Vector2 &value);

	/// @brief Get the skew of the node.
	/// @return The skew of the node.
	float get_skew() const;
	/// @brief Set the skew of the node.
	/// @param value The new skew of the node.
	void set_skew(const Variant &value);

	/// @brief Set the 2D transform of the node.
	/// @param value The new 2D transform of the node.
	void set_transform(const Transform2D &value);

	/// @brief Get the 2D transform of the node.
	/// @return The 2D transform of the node.
	Transform2D get_transform() const;

	/// @brief  Duplicate the node.
	/// @return A new Node2D object with the same properties and children.
	Node2D duplicate() const;

	/// @brief Create a new Node2D object.
	/// @param path The path to the Node2D object.
	/// @return The Node2D object.
	static Node2D create(std::string_view path);
};

inline Node2D Variant::as_node2d() const {
	if (get_type() == Variant::OBJECT)
		return Node2D{uintptr_t(v.i)};
	else if (get_type() == Variant::NODE_PATH)
		return Node2D{this->internal_fetch_string()};

	api_throw("std::bad_cast", "Variant is not a Node2D or NodePath", this);
}

inline Node2D Object::as_node2d() const {
	return Node2D{address()};
}

inline Variant::Variant(const Node2D &node) {
	m_type = OBJECT;
	v.i = node.address();
}
