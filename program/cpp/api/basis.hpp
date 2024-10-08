#pragma once

#include "variant.hpp"

/**
 * @brief Basis wrapper for godot-cpp Basis.
 * Implemented by referencing and mutating a host-side Basis Variant.
 */
struct Basis {
	constexpr Basis() {} // DON'T TOUCH

	static Basis identity();
	Basis(const Vector3 &x, const Vector3 &y, const Vector3 &z);

	Basis &operator =(const Basis &basis);

	// Basis operations
	void invert();
	void transpose();
	Basis inverse() const;
	Basis transposed() const;
	double determinant() const;

	Basis rotated(const Vector3 &axis, double angle) const;
	Basis lerp(const Basis &to, double t) const;
	Basis slerp(const Basis &to, double t) const;

	// Basis access
	Vector3 operator[](int idx) const { return get_row(idx); }

	void set_row(int idx, const Vector3 &axis);
	Vector3 get_row(int idx) const;
	void set_column(int idx, const Vector3 &axis);
	Vector3 get_column(int idx) const;

	// Basis size
	static constexpr int size() { return 3; }

	static Basis from_variant_index(unsigned idx) { Basis a {}; a.m_idx = idx; return a; }
	unsigned get_variant_index() const noexcept { return m_idx; }
private:
	unsigned m_idx = INT32_MIN;
};

inline Variant::Variant(const Basis &b) {
	m_type = Variant::BASIS;
	v.i = b.get_variant_index();
}

inline Variant::operator Basis() const {
	if (m_type != Variant::BASIS) {
		api_throw("std::bad_cast", "Failed to cast Variant to Basis", this);
	}
	return Basis::from_variant_index(v.i);
}

inline Basis Variant::as_basis() const {
	return static_cast<Basis>(*this);
}

inline Basis &Basis::operator =(const Basis &basis) {
	this->m_idx = basis.m_idx;
	return *this;
}
