#pragma once
#include <cmath>

struct Vector2 {
	float x;
	float y;

	float length() const noexcept;
	Vector2 normalized() const noexcept;
	Vector2 rotated(float angle) const noexcept;
	float distance_to(const Vector2& other) const noexcept;
	Vector2 direction_to(const Vector2& other) const noexcept;
	float dot(const Vector2& other) const noexcept;
	static Vector2 from_angle(float angle) noexcept;

	auto& operator += (const Vector2& other) {
		x += other.x;
		y += other.y;
		return *this;
	}
	auto& operator -= (const Vector2& other) {
		x -= other.x;
		y -= other.y;
		return *this;
	}
	auto& operator *= (const Vector2& other) {
		x *= other.x;
		y *= other.y;
		return *this;
	}
	auto& operator /= (const Vector2& other) {
		x /= other.x;
		y /= other.y;
		return *this;
	}
};
struct Vector2i {
	int x;
	int y;

	auto& operator += (const Vector2i& other) {
		x += other.x;
		y += other.y;
		return *this;
	}
	auto& operator -= (const Vector2i& other) {
		x -= other.x;
		y -= other.y;
		return *this;
	}
	auto& operator *= (const Vector2i& other) {
		x *= other.x;
		y *= other.y;
		return *this;
	}
	auto& operator /= (const Vector2i& other) {
		x /= other.x;
		y /= other.y;
		return *this;
	}
};
struct Rect2 {
	float x;
	float y;
	float w;
	float h;

	auto& operator += (const Rect2& other) {
		x += other.x;
		y += other.y;
		w += other.w;
		h += other.h;
		return *this;
	}
	auto& operator -= (const Rect2& other) {
		x -= other.x;
		y -= other.y;
		w -= other.w;
		h -= other.h;
		return *this;
	}
	auto& operator *= (const Rect2& other) {
		x *= other.x;
		y *= other.y;
		w *= other.w;
		h *= other.h;
		return *this;
	}
	auto& operator /= (const Rect2& other) {
		x /= other.x;
		y /= other.y;
		w /= other.w;
		h /= other.h;
		return *this;
	}
};
struct Rect2i {
	int x;
	int y;
	int w;
	int h;

	auto& operator += (const Rect2i& other) {
		x += other.x;
		y += other.y;
		w += other.w;
		h += other.h;
		return *this;
	}
	auto& operator -= (const Rect2i& other) {
		x -= other.x;
		y -= other.y;
		w -= other.w;
		h -= other.h;
		return *this;
	}
	auto& operator *= (const Rect2i& other) {
		x *= other.x;
		y *= other.y;
		w *= other.w;
		h *= other.h;
		return *this;
	}
	auto& operator /= (const Rect2i& other) {
		x /= other.x;
		y /= other.y;
		w /= other.w;
		h /= other.h;
		return *this;
	}
};
struct Vector3 {
	float x;
	float y;
	float z;

	auto& operator += (const Vector3& other) {
		x += other.x;
		y += other.y;
		z += other.z;
		return *this;
	}
	auto& operator -= (const Vector3& other) {
		x -= other.x;
		y -= other.y;
		z -= other.z;
		return *this;
	}
	auto& operator *= (const Vector3& other) {
		x *= other.x;
		y *= other.y;
		z *= other.z;
		return *this;
	}
	auto& operator /= (const Vector3& other) {
		x /= other.x;
		y /= other.y;
		z /= other.z;
		return *this;
	}
};
struct Vector3i {
	int x;
	int y;
	int z;

	auto& operator += (const Vector3i& other) {
		x += other.x;
		y += other.y;
		z += other.z;
		return *this;
	}
	auto& operator -= (const Vector3i& other) {
		x -= other.x;
		y -= other.y;
		z -= other.z;
		return *this;
	}
	auto& operator *= (const Vector3i& other) {
		x *= other.x;
		y *= other.y;
		z *= other.z;
		return *this;
	}
	auto& operator /= (const Vector3i& other) {
		x /= other.x;
		y /= other.y;
		z /= other.z;
		return *this;
	}
};
struct Vector4 {
	float x;
	float y;
	float z;
	float w;

	auto& operator += (const Vector4& other) {
		x += other.x;
		y += other.y;
		z += other.z;
		w += other.w;
		return *this;
	}
	auto& operator -= (const Vector4& other) {
		x -= other.x;
		y -= other.y;
		z -= other.z;
		w -= other.w;
		return *this;
	}
	auto& operator *= (const Vector4& other) {
		x *= other.x;
		y *= other.y;
		z *= other.z;
		w *= other.w;
		return *this;
	}
	auto& operator /= (const Vector4& other) {
		x /= other.x;
		y /= other.y;
		z /= other.z;
		w /= other.w;
		return *this;
	}
};
struct Vector4i {
	int x;
	int y;
	int z;
	int w;

	auto& operator += (const Vector4i& other) {
		x += other.x;
		y += other.y;
		z += other.z;
		w += other.w;
		return *this;
	}
	auto& operator -= (const Vector4i& other) {
		x -= other.x;
		y -= other.y;
		z -= other.z;
		w -= other.w;
		return *this;
	}
	auto& operator *= (const Vector4i& other) {
		x *= other.x;
		y *= other.y;
		z *= other.z;
		w *= other.w;
		return *this;
	}
	auto& operator /= (const Vector4i& other) {
		x /= other.x;
		y /= other.y;
		z /= other.z;
		w /= other.w;
		return *this;
	}
};

inline auto operator + (const Vector2& a, const Vector2& b) noexcept {
	return Vector2{a.x + b.x, a.y + b.y};
}
inline auto operator - (const Vector2& a, const Vector2& b) noexcept {
	return Vector2{a.x - b.x, a.y - b.y};
}
inline auto operator * (const Vector2& a, const Vector2& b) noexcept {
	return Vector2{a.x * b.x, a.y * b.y};
}
inline auto operator / (const Vector2& a, const Vector2& b) noexcept {
	return Vector2{a.x / b.x, a.y / b.y};
}

inline auto operator + (const Vector2i& a, const Vector2i& b) noexcept {
	return Vector2i{a.x + b.x, a.y + b.y};
}
inline auto operator - (const Vector2i& a, const Vector2i& b) noexcept {
	return Vector2i{a.x - b.x, a.y - b.y};
}
inline auto operator * (const Vector2i& a, const Vector2i& b) noexcept {
	return Vector2i{a.x * b.x, a.y * b.y};
}
inline auto operator / (const Vector2i& a, const Vector2i& b) noexcept {
	return Vector2i{a.x / b.x, a.y / b.y};
}

inline auto operator + (const Rect2& a, const Rect2& b) noexcept {
	return Rect2{a.x + b.x, a.y + b.y, a.w + b.w, a.h + b.h};
}
inline auto operator - (const Rect2& a, const Rect2& b) noexcept {
	return Rect2{a.x - b.x, a.y - b.y, a.w - b.w, a.h - b.h};
}
inline auto operator * (const Rect2& a, const Rect2& b) noexcept {
	return Rect2{a.x * b.x, a.y * b.y, a.w * b.w, a.h * b.h};
}
inline auto operator / (const Rect2& a, const Rect2& b) noexcept {
	return Rect2{a.x / b.x, a.y / b.y, a.w / b.w, a.h / b.h};
}

inline auto operator + (const Rect2i& a, const Rect2i& b) noexcept {
	return Rect2i{a.x + b.x, a.y + b.y, a.w + b.w, a.h + b.h};
}
inline auto operator - (const Rect2i& a, const Rect2i& b) noexcept {
	return Rect2i{a.x - b.x, a.y - b.y, a.w - b.w, a.h - b.h};
}
inline auto operator * (const Rect2i& a, const Rect2i& b) noexcept {
	return Rect2i{a.x * b.x, a.y * b.y, a.w * b.w, a.h * b.h};
}
inline auto operator / (const Rect2i& a, const Rect2i& b) noexcept {
	return Rect2i{a.x / b.x, a.y / b.y, a.w / b.w, a.h / b.h};
}

inline auto operator + (const Vector3& a, const Vector3& b) noexcept {
	return Vector3{a.x + b.x, a.y + b.y, a.z + b.z};
}
inline auto operator - (const Vector3& a, const Vector3& b) noexcept {
	return Vector3{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline auto operator * (const Vector3& a, const Vector3& b) noexcept {
	return Vector3{a.x * b.x, a.y * b.y, a.z * b.z};
}
inline auto operator / (const Vector3& a, const Vector3& b) noexcept {
	return Vector3{a.x / b.x, a.y / b.y, a.z / b.z};
}

inline auto operator + (const Vector3i& a, const Vector3i& b) noexcept {
	return Vector3i{a.x + b.x, a.y + b.y, a.z + b.z};
}
inline auto operator - (const Vector3i& a, const Vector3i& b) noexcept {
	return Vector3i{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline auto operator * (const Vector3i& a, const Vector3i& b) noexcept {
	return Vector3i{a.x * b.x, a.y * b.y, a.z * b.z};
}
inline auto operator / (const Vector3i& a, const Vector3i& b) noexcept {
	return Vector3i{a.x / b.x, a.y / b.y, a.z / b.z};
}

inline auto operator + (const Vector4& a, const Vector4& b) noexcept {
	return Vector4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
inline auto operator - (const Vector4& a, const Vector4& b) noexcept {
	return Vector4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
inline auto operator * (const Vector4& a, const Vector4& b) noexcept {
	return Vector4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}
inline auto operator / (const Vector4& a, const Vector4& b) noexcept {
	return Vector4{a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w};
}

inline auto operator + (const Vector4i& a, const Vector4i& b) noexcept {
	return Vector4i{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
inline auto operator - (const Vector4i& a, const Vector4i& b) noexcept {
	return Vector4i{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
inline auto operator * (const Vector4i& a, const Vector4i& b) noexcept {
	return Vector4i{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}
inline auto operator / (const Vector4i& a, const Vector4i& b) noexcept {
	return Vector4i{a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w};
}

inline auto operator + (const Vector2& a, float b) noexcept {
	return Vector2{a.x + b, a.y + b};
}
inline auto operator - (const Vector2& a, float b) noexcept {
	return Vector2{a.x - b, a.y - b};
}
inline auto operator * (const Vector2& a, float b) noexcept {
	return Vector2{a.x * b, a.y * b};
}
inline auto operator / (const Vector2& a, float b) noexcept {
	return Vector2{a.x / b, a.y / b};
}

inline auto operator + (const Vector2i& a, int b) noexcept {
	return Vector2i{a.x + b, a.y + b};
}
inline auto operator - (const Vector2i& a, int b) noexcept {
	return Vector2i{a.x - b, a.y - b};
}
inline auto operator * (const Vector2i& a, int b) noexcept {
	return Vector2i{a.x * b, a.y * b};
}
inline auto operator / (const Vector2i& a, int b) noexcept {
	return Vector2i{a.x / b, a.y / b};
}

inline auto operator + (const Rect2& a, float b) noexcept {
	return Rect2{a.x + b, a.y + b, a.w + b, a.h + b};
}
inline auto operator - (const Rect2& a, float b) noexcept {
	return Rect2{a.x - b, a.y - b, a.w - b, a.h - b};
}
inline auto operator * (const Rect2& a, float b) noexcept {
	return Rect2{a.x * b, a.y * b, a.w * b, a.h * b};
}
inline auto operator / (const Rect2& a, float b) noexcept {
	return Rect2{a.x / b, a.y / b, a.w / b, a.h / b};
}

inline auto operator + (const Rect2i& a, int b) noexcept {
	return Rect2i{a.x + b, a.y + b, a.w + b, a.h + b};
}
inline auto operator - (const Rect2i& a, int b) noexcept {
	return Rect2i{a.x - b, a.y - b, a.w - b, a.h - b};
}
inline auto operator * (const Rect2i& a, int b) noexcept {
	return Rect2i{a.x * b, a.y * b, a.w * b, a.h * b};
}
inline auto operator / (const Rect2i& a, int b) noexcept {
	return Rect2i{a.x / b, a.y / b, a.w / b, a.h / b};
}

inline auto operator + (const Vector3& a, float b) noexcept {
	return Vector3{a.x + b, a.y + b, a.z + b};
}
inline auto operator - (const Vector3& a, float b) noexcept {
	return Vector3{a.x - b, a.y - b, a.z - b};
}
inline auto operator * (const Vector3& a, float b) noexcept {
	return Vector3{a.x * b, a.y * b, a.z * b};
}
inline auto operator / (const Vector3& a, float b) noexcept {
	return Vector3{a.x / b, a.y / b, a.z / b};
}

inline auto operator + (const Vector3i& a, int b) noexcept {
	return Vector3i{a.x + b, a.y + b, a.z + b};
}
inline auto operator - (const Vector3i& a, int b) noexcept {
	return Vector3i{a.x - b, a.y - b, a.z - b};
}
inline auto operator * (const Vector3i& a, int b) noexcept {
	return Vector3i{a.x * b, a.y * b, a.z * b};
}
inline auto operator / (const Vector3i& a, int b) noexcept {
	return Vector3i{a.x / b, a.y / b, a.z / b};
}

inline auto operator + (const Vector4& a, float b) noexcept {
	return Vector4{a.x + b, a.y + b, a.z + b, a.w + b};
}
inline auto operator - (const Vector4& a, float b) noexcept {
	return Vector4{a.x - b, a.y - b, a.z - b, a.w - b};
}
inline auto operator * (const Vector4& a, float b) noexcept {
	return Vector4{a.x * b, a.y * b, a.z * b, a.w * b};
}
inline auto operator / (const Vector4& a, float b) noexcept {
	return Vector4{a.x / b, a.y / b, a.z / b, a.w / b};
}

inline auto operator + (const Vector4i& a, int b) noexcept {
	return Vector4i{a.x + b, a.y + b, a.z + b, a.w + b};
}
inline auto operator - (const Vector4i& a, int b) noexcept {
	return Vector4i{a.x - b, a.y - b, a.z - b, a.w - b};
}
inline auto operator * (const Vector4i& a, int b) noexcept {
	return Vector4i{a.x * b, a.y * b, a.z * b, a.w * b};
}
inline auto operator / (const Vector4i& a, int b) noexcept {
	return Vector4i{a.x / b, a.y / b, a.z / b, a.w / b};
}

inline float Vector2::length() const noexcept {
	register float x asm("fa0") = this->x;
	register float y asm("fa1") = this->y;
	register int syscall asm("a7") = 514; // ECALL_VEC2_LENGTH

	__asm__ volatile("ecall"
					 : "+f"(x)
					 : "f"(y), "r"(syscall));
	return x;
}

inline Vector2 Vector2::normalized() const noexcept {
	register float x asm("fa0") = this->x;
	register float y asm("fa1") = this->y;
	register int syscall asm("a7") = 515; // ECALL_VEC2_NORMALIZED

	__asm__ volatile("ecall"
					 : "+f"(x), "+f"(y)
					 : "r"(syscall));
	return {x, y};
}

inline Vector2 Vector2::rotated(float angle) const noexcept {
	register float x asm("fa0") = this->x;
	register float y asm("fa1") = this->y;
	register float a asm("fa2") = angle;
	register int syscall asm("a7") = 516; // ECALL_VEC2_ROTATED

	__asm__ volatile("ecall"
					 : "+f"(x), "+f"(y)
					 : "f"(a), "r"(syscall));
	return {x, y};
}

inline float Vector2::distance_to(const Vector2& other) const noexcept {
	return (*this - other).length();
}
inline Vector2 Vector2::direction_to(const Vector2& other) const noexcept {
	return (*this - other).normalized();
}
inline float Vector2::dot(const Vector2& other) const noexcept {
	return x * other.x + y * other.y;
}
inline Vector2 Vector2::from_angle(float angle) noexcept {
	register float a asm("fa0") = angle;
	register float x asm("fa0");
	register float y asm("fa1");
	register int syscall asm("a7") = 513; // ECALL_SINCOS

	__asm__ volatile("ecall"
					 : "=f"(x), "=f"(y)
					 : "f"(a), "r"(syscall));
	return {x, y};
}
