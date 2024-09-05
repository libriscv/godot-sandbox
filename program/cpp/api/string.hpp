#pragma once

#include "variant.hpp"
#include <string>

/**
 * @brief String wrapper for Godot String.
 * Implemented by referencing and mutating a host-side String Variant.
 */
struct String {
	String(std::string_view value = "");
	String(const String &other);
	String(String &&other);
	~String() = default;

	String &operator=(const String &other);
	String &operator=(String &&other);

	// String operations
	void append(const String &value);
	void append(std::string_view value);
	void erase(int idx, int count = 1);
	void insert(int idx, const String &value);
	int find(const String &value) const;
	bool contains(std::string_view value) const { return find(value) != -1; }
	bool empty() const { return size() == 0; }

	String &operator +=(const String &value) { append(value); return *this; }
	String &operator +=(std::string_view value) { append(value); return *this; }

	// String access
	String operator[](int idx) const;
	String at(int idx) const { return (*this)[idx]; }

	// String size
	int size() const;

	static String from_variant_index(unsigned idx) { String a; a.m_idx = idx; return a; }
	unsigned get_variant_index() const noexcept { return m_idx; }

private:
	unsigned m_idx = -1;
};

inline Variant::Variant(const String &s) {
	m_type = Variant::STRING;
	v.i = s.get_variant_index();
}

inline Variant::operator String() const {
	return String::from_variant_index(v.i);
}

inline String Variant::as_string() const {
	return String::from_variant_index(v.i);
}

inline String operator +(const String &lhs, const String &rhs) {
	String s = lhs;
	s.append(rhs);
	return s;
}

inline String operator +(const String &lhs, std::string_view rhs) {
	String s = lhs;
	s.append(rhs);
	return s;
}