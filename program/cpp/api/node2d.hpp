#pragma once
#include "node.hpp"

// Node2D: Contains 2D transformations.
// Such as: position, rotation, scale, and skew.
struct Node2D : public Node {
	/// @brief Construct a Node2D object from an existing in-scope Node object.
	/// @param addr The address of the Node2D object.
	Node2D(uint64_t addr) : Node(addr) {}

	/// @brief Construct a Node2D object from a path.
	/// @param path The path to the Node2D object.
	Node2D(const std::string& path) : Node(path) {}

	/// @brief Get the position of the node.
	/// @return The position of the node.
	Vector2 get_position() const;
	/// @brief Set the position of the node.
	/// @param value The new position of the node.
	void set_position(const Variant &value);

	/// @brief Get the rotation of the node.
	/// @return The rotation of the node.
	float get_rotation() const;
	/// @brief Set the rotation of the node.
	/// @param value The new rotation of the node.
	void set_rotation(const Variant &value);

	/// @brief Get the scale of the node.
	/// @return The scale of the node.
	Vector2 get_scale() const;
	/// @brief Set the scale of the node.
	/// @param value The new scale of the node.
	void set_scale(const Variant &value);

	/// @brief Get the skew of the node.
	/// @return The skew of the node.
	float get_skew() const;
	/// @brief Set the skew of the node.
	/// @param value The new skew of the node.
	void set_skew(const Variant &value);

	// TODO:
	// void set_transform(const Transform2D &value);
	// Transform2D get_transform() const;

	/// @brief  Duplicate the node.
	/// @return A new Node2D object with the same properties and children.
	Node2D duplicate() const;
};

inline Node2D Variant::as_node2d() const {
	if (get_type() == Variant::OBJECT)
		return Node2D{uintptr_t(v.i)};
	else if (get_type() == Variant::NODE_PATH)
		return Node2D{*v.s};

	api_throw("std::bad_cast", "Variant is not a Node2D or NodePath", this);
}
