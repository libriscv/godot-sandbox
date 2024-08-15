#pragma once
#include "node.hpp"

// Node2D: Contains 2D tranformations.
// Such as: position, rotation, scale, and skew.
struct Node2D : public Node {
	Node2D(uint64_t addr) : Node(addr) {}
	Node2D(const std::string& name) : Node(name) {}

	Vector2 get_position() const;
	void set_position(const Variant &value);

	float get_rotation() const;
	void set_rotation(const Variant &value);

	Vector2 get_scale() const;
	void set_scale(const Variant &value);

	float get_skew() const;
	void set_skew(const Variant &value);

	// TODO:
	// void set_transform(const Transform2D &value);
	// Transform2D get_transform() const;

	Node2D duplicate() const;
};

inline Node2D Variant::as_node2d() const {
	if (get_type() == Variant::OBJECT)
		return Node2D{uintptr_t(v.i)};
	else if (get_type() == Variant::NODE_PATH)
		return Node2D{*v.s};

	throw std::bad_cast();
}
