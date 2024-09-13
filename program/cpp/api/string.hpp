#pragma once

#include "variant.hpp"
#include <string>

/**
 * @brief String wrapper for Godot String.
 * Implemented by referencing and mutating a host-side String Variant.
 */
union String {
	constexpr String() {} // DON'T TOUCH
	String(std::string_view value);
	String &operator =(std::string_view value);
	template <size_t N>
	String(const char (&value)[N]);

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

	operator std::string() const { return utf8(); }
	operator std::u32string() const { return utf32(); }
	std::string utf8() const;
	std::u32string utf32() const;

	// String size
	int size() const;

	static String from_variant_index(unsigned idx) { String a {}; a.m_idx = idx; return a; }
	unsigned get_variant_index() const noexcept { return m_idx; }
	static unsigned Create(const char *data, size_t size);

private:
	unsigned m_idx = -1;
};
using NodePath = String; // NodePath is compatible with String

inline Variant::Variant(const String &s) {
	m_type = Variant::STRING;
	v.i = s.get_variant_index();
}

inline Variant::operator String() const {
	return as_string();
}

inline String Variant::as_string() const {
	if (m_type != STRING) {
		api_throw("std::bad_cast", "Failed to cast Variant to String", this);
	}
	return String::from_variant_index(v.i);
}

inline String::String(std::string_view value)
	: m_idx(Create(value.data(), value.size())) {}
inline String &String::operator =(std::string_view value) {
	m_idx = Create(value.data(), value.size());
	return *this;
}
template <size_t N>
inline String::String(const char (&value)[N])
	: m_idx(Create(value, N - 1)) {}
