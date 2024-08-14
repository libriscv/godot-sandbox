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

	/// @brief Add a child to the node.
	/// @param child The child node to add.
	/// @param deferred If true, the child will be added next frame.
	void add_child(const Node2D &child, bool deferred = false);

	std::string get_name() const;

	std::string get_path() const;

	Node2D get_parent() const;

	// Call a method on the node.
	// @param method The method to call.
	// @param deferred If true, the method will be called next frame.
	// @param args The arguments to pass to the method.
	// @return The return value of the method.
	Variant callv(const std::string &method, bool deferred, const Variant *argv, unsigned argc);

	template <typename... Args>
	Variant call(const std::string &method, Args... args) {
		Variant argv[] = {args...};
		return callv(method, false, argv, sizeof...(Args));
	}

	template <typename... Args>
	Variant call_deferred(const std::string &method, Args... args) {
		Variant argv[] = {args...};
		return callv(method, true, argv, sizeof...(Args));
	}

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
