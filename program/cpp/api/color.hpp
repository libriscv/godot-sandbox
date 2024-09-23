#pragma once

struct Color {
	float r;
	float g;
	float b;
	float a;

	template <typename... Args>
	Variant operator () (std::string_view method, Args&&... args);

	Color& operator += (const Color& other) {
		r += other.r;
		g += other.g;
		b += other.b;
		a += other.a;
		return *this;
	}
	Color& operator -= (const Color& other) {
		r -= other.r;
		g -= other.g;
		b -= other.b;
		a -= other.a;
		return *this;
	}
	Color& operator *= (const Color& other) {
		r *= other.r;
		g *= other.g;
		b *= other.b;
		a *= other.a;
		return *this;
	}
	Color& operator /= (const Color& other) {
		r /= other.r;
		g /= other.g;
		b /= other.b;
		a /= other.a;
		return *this;
	}

	Color& operator += (float other) {
		r += other;
		g += other;
		b += other;
		a += other;
		return *this;
	}
	Color& operator -= (float other) {
		r -= other;
		g -= other;
		b -= other;
		a -= other;
		return *this;
	}
	Color& operator *= (float other) {
		r *= other;
		g *= other;
		b *= other;
		a *= other;
		return *this;
	}
	Color& operator /= (float other) {
		r /= other;
		g /= other;
		b /= other;
		a /= other;
		return *this;
	}
};

inline auto operator + (const Color& a, float b) noexcept {
	return Color{a.r + b, a.g + b, a.b + b, a.a + b};
}
inline auto operator - (const Color& a, float b) noexcept {
	return Color{a.r - b, a.g - b, a.b - b, a.a - b};
}
inline auto operator * (const Color& a, float b) noexcept {
	return Color{a.r * b, a.g * b, a.b * b, a.a * b};
}
inline auto operator / (const Color& a, float b) noexcept {
	return Color{a.r / b, a.g / b, a.b / b, a.a / b};
}

inline auto operator + (const Color& a, const Color& b) noexcept {
	return Color{a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a};
}
inline auto operator - (const Color& a, const Color& b) noexcept {
	return Color{a.r - b.r, a.g - b.g, a.b - b.b, a.a - b.a};
}
inline auto operator * (const Color& a, const Color& b) noexcept {
	return Color{a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a};
}
inline auto operator / (const Color& a, const Color& b) noexcept {
	return Color{a.r / b.r, a.g / b.g, a.b / b.b, a.a / b.a};
}
