#pragma once
#include "variant.hpp"

struct Object {
	/// @brief Construct an Object object from an allowed global object.
	Object(const std::string &name);

	/// @brief Construct an Object object from an existing in-scope Object object.
	/// @param addr The address of the Object object.
	Object(uint64_t addr) : m_address{addr} {}

	/// @brief Get a list of methods available on the object.
	/// @return A list of method names.
	std::vector<std::string> get_method_list() const;

	// Call a method on the node.
	// @param method The method to call.
	// @param deferred If true, the method will be called next frame.
	// @param args The arguments to pass to the method.
	// @return The return value of the method.
	Variant callv(const std::string &method, bool deferred, const Variant *argv, unsigned argc);

	template <typename... Args>
	Variant call(const std::string &method, Args... args);

	template <typename... Args>
	Variant operator () (const std::string &method, Args... args);

	template <typename... Args>
	Variant call_deferred(const std::string &method, Args... args);

	// Get a property of the node.
	// @param name The name of the property.
	// @return The value of the property.
	Variant get(const std::string &name) const;

	// Set a property of the node.
	// @param name The name of the property.
	// @param value The value to set the property to.
	void set(const std::string &name, const Variant &value);

	// Get the object identifier.
	uint64_t address() const { return m_address; }

	// Check if the node is valid.
	bool is_valid() const { return m_address != 0; }

protected:
	uint64_t m_address;
};

inline Object Variant::as_object() const {
	if (get_type() == Variant::OBJECT)
		return Object{uintptr_t(v.i)};
	throw std::bad_cast();
}

template <typename... Args>
inline Variant Object::call(const std::string &method, Args... args) {
	Variant argv[] = {args...};
	return callv(method, false, argv, sizeof...(Args));
}

template <typename... Args>
inline Variant Object::operator () (const std::string &method, Args... args) {
	return call(method, args...);
}

template <typename... Args>
inline Variant Object::call_deferred(const std::string &method, Args... args) {
	Variant argv[] = {args...};
	return callv(method, true, argv, sizeof...(Args));
}
