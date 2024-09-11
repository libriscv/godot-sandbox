#pragma once

#include "variant.hpp"

class ArrayIterator;

struct Array {
	Array(unsigned size = 0);
	Array(const std::vector<Variant> &values);
	Array(const Array &other);
	Array(Array &&other);
	~Array();

	Array &operator=(const Array &other);
	Array &operator=(Array &&other);

	operator Variant() const;

	// Array operations
	void push_back(const Variant &value);
	void push_front(const Variant &value);
	void pop_at(int idx);
	void pop_back();
	void pop_front();
	void insert(int idx, const Variant &value);
	void erase(int idx);
	void resize(int size);
	void clear();
	void sort();

	// Array access
	Variant operator[](int idx) const;
	Variant at(int idx) const { return (*this)[idx]; }

	// Array size
	int size() const;

	inline auto begin();
	inline auto end();
	inline auto rbegin();
	inline auto rend();

	static Array from_variant_index(unsigned idx) { Array a; a.m_idx = idx; return a; }
	unsigned get_variant_index() const noexcept { return m_idx; }

private:
	unsigned m_idx = -1;
};

// Array operations
inline Array::operator Variant() const {
	return Variant(*this);
}

inline Array Variant::as_array() const {
	if (m_type != ARRAY) {
		api_throw("std::bad_cast", "Failed to cast Variant to Array", this);
	}
	return Array::from_variant_index(v.i);
}

inline Variant::Variant(const Array& a) {
	m_type = ARRAY;
	v.i = a.get_variant_index();
}

inline Variant::operator Array() const {
	return as_array();
}

class ArrayIterator {
public:
	ArrayIterator(const Array &array, unsigned idx) : m_array(array), m_idx(idx) {}

	bool operator!=(const ArrayIterator &other) const { return m_idx != other.m_idx; }
	ArrayIterator &operator++() { m_idx++; return *this; }
	Variant operator*() const { return m_array[m_idx]; }

private:
	const Array m_array;
	unsigned m_idx;
};

inline auto Array::begin() {
	return ArrayIterator(*this, 0);
}
inline auto Array::end() {
	return ArrayIterator(*this, size());
}
inline auto Array::rbegin() {
	return ArrayIterator(*this, size() - 1);
}
inline auto Array::rend() {
	return ArrayIterator(*this, -1);
}
