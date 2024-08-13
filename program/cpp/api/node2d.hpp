#include "variant.hpp"

// Node2D: Contains 2D tranformations.
// Such as: position, rotation, scale, and skew.
struct Node2D {
	Node2D(uint64_t addr) : m_address{addr} {}
	Node2D(const std::string& name);

	Variant get_position() const;
	void set_position(const Variant &value);

	Variant get_rotation() const;
	void set_rotation(const Variant &value);

	Variant get_scale() const;
	void set_scale(const Variant &value);

	Variant get_skew() const;
	void set_skew(const Variant &value);

	void queue_free();

	Node2D duplicate() const;

	void add_child(const Node2D &child, bool legible_unique_name = true);

	std::string get_name() const;

	std::string get_path() const;

	Node2D get_parent() const;

	// Get the Node2D object identifier.
	uint64_t address() const { return m_address; }

private:
	// Node2D object identifier.
	uint64_t m_address;
};

inline Node2D Variant::as_node2d() const {
	if (get_type() == Variant::OBJECT)
		return Node2D{uintptr_t(v.i)};
	else if (get_type() == Variant::NODE_PATH)
		return Node2D{*v.s};

	throw std::bad_cast();
}
