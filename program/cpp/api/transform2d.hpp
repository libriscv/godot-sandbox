#pragma once

#include "variant.hpp"

/**
 * @brief Transform2D wrapper for godot-cpp Transform2D.
 * Implemented by referencing and mutating a host-side Transform2D Variant.
**/
struct Transform2D {
	constexpr Transform2D() {} // DON'T TOUCH

	static Transform2D identity();
	Transform2D(const Vector2 &x, const Vector2 &y, const Vector2 &origin);

	Transform2D &operator =(const Transform2D &transform);

	// Transform2D operations
	void invert();
	void affine_invert();
	Transform2D inverse() const;
	Transform2D orthonormalized() const;
	Transform2D rotated(const double p_angle) const;
	Transform2D scaled(const Vector2 &scale) const;
	Transform2D translated(const Vector2 &offset) const;

	// Transform2D access
	Vector2 get_column(int idx) const;
	void set_column(int idx, const Vector2 &axis);

	static Transform2D from_variant_index(unsigned idx) { Transform2D a {}; a.m_idx = idx; return a; }
	unsigned get_variant_index() const noexcept { return m_idx; }
private:
	unsigned m_idx = INT32_MIN;
};

inline Variant::Variant(const Transform2D &t) {
	m_type = Variant::TRANSFORM2D;
	v.i = t.get_variant_index();
}

inline Variant::operator Transform2D() const {
	if (m_type != Variant::TRANSFORM2D) {
		api_throw("std::bad_cast", "Failed to cast Variant to Transform2D", this);
	}
	return Transform2D::from_variant_index(v.i);
}

inline Transform2D Variant::as_transform2d() const {
	return static_cast<Transform2D>(*this);
}

inline Transform2D &Transform2D::operator =(const Transform2D &transform) {
	this->m_idx = transform.m_idx;
	return *this;
}
