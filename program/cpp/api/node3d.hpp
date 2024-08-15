#pragma once
#include "node.hpp"

// Node3D: Contains 3D tranformations.
// Such as: position, rotation, scale, and skew.
struct Node3D : public Node {
	Node3D(uint64_t addr) : Node(addr) {}
	Node3D(const std::string& name) : Node(name) {}

	Vector3 get_position() const;
	void set_position(const Variant &value);

	Vector3 get_rotation() const;
	void set_rotation(const Variant &value);

	Vector3 get_scale() const;
	void set_scale(const Variant &value);

	// TODO:
	// void set_transform(const Transform3D &value);
	// Transform3D get_transform() const;
	// void set_quaternion(const Quaternion &value);
	// Quaternion get_quaternion() const;

	Node3D duplicate() const;
};

inline Node3D Variant::as_node3d() const {
	if (get_type() == Variant::OBJECT)
		return Node3D{uintptr_t(v.i)};
	else if (get_type() == Variant::NODE_PATH)
		return Node3D{*v.s};

	throw std::bad_cast();
}
