#include "variant.hpp"

// Node2D: Contains 2D tranformations.
// Such as: position, rotation, scale, and skew.
struct Node2D {
	Node2D(std::string name) : name(name) {}

	Variant get_position() const;
	void set_position(const Variant &value);

	Variant get_rotation() const;
	void set_rotation(const Variant &value);

	Variant get_scale() const;
	void set_scale(const Variant &value);

	Variant get_skew() const;
	void set_skew(const Variant &value);

	// Node2D object name (with null-terminator).
	const std::string name;
};
