#pragma once
#include "variant.hpp"

struct Node {
	Node(uint64_t addr) : m_address{addr} {}
	Node(const std::string& name);

	std::string get_name() const;

	std::string get_path() const;

	Node get_parent() const;

	/// @brief Add a child to the node.
	/// @param child The child node to add.
	/// @param deferred If true, the child will be added next frame.
	void add_child(const Node &child, bool deferred = false);

	void queue_free();

	Node duplicate() const;

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

	// Get the object identifier.
	uint64_t address() const { return m_address; }

protected:
	// Node object identifier.
	uint64_t m_address;
};

inline Node Variant::as_node() const {
	if (get_type() == Variant::OBJECT)
		return Node{uintptr_t(v.i)};
	else if (get_type() == Variant::NODE_PATH)
		return Node{*v.s};

	throw std::bad_cast();
}
