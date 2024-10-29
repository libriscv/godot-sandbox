#pragma once
#include <cmath>
#include <string_view>
#include "syscalls_fwd.hpp"
enum ClockDirection {
	CLOCKWISE = 0,
	COUNTERCLOCKWISE = 1,
};

#include "vector2.hpp"
#include "vector2i.hpp"
#include "rect2.hpp"
#include "rect2i.hpp"

struct Vector3 {
	real_t x;
	real_t y;
	real_t z;

	float length() const noexcept;
	float length_squared() const noexcept { return this->dot(*this); }
	void normalize() { *this = normalized(); }
	Vector3 normalized() const noexcept;
	float dot(const Vector3& other) const noexcept;
	Vector3 cross(const Vector3& other) const noexcept;
	float distance_to(const Vector3& other) const noexcept;
	float distance_squared_to(const Vector3& other) const noexcept;
	float angle_to(const Vector3& other) const noexcept;
	Vector3 direction_to(const Vector3& other) const noexcept;
	Vector3 floor() const noexcept;

	template <typename... Args>
	Variant operator () (std::string_view method, Args&&... args);

	Vector3& operator += (const Vector3& other) {
		x += other.x;
		y += other.y;
		z += other.z;
		return *this;
	}
	Vector3& operator -= (const Vector3& other) {
		x -= other.x;
		y -= other.y;
		z -= other.z;
		return *this;
	}
	Vector3& operator *= (const Vector3& other) {
		x *= other.x;
		y *= other.y;
		z *= other.z;
		return *this;
	}
	Vector3& operator /= (const Vector3& other) {
		x /= other.x;
		y /= other.y;
		z /= other.z;
		return *this;
	}

	bool operator == (const Vector3& other) const {
		return __builtin_memcmp(this, &other, sizeof(Vector3)) == 0;
	}

	constexpr Vector3() : x(0), y(0), z(0) {}
	constexpr Vector3(real_t val) : x(val), y(val), z(val) {}
	constexpr Vector3(real_t x, real_t y, real_t z) : x(x), y(y), z(z) {}
};
struct Vector3i {
	int x;
	int y;
	int z;

	template <typename... Args>
	Variant operator () (std::string_view method, Args&&... args);

	Vector3i& operator += (const Vector3i& other) {
		x += other.x;
		y += other.y;
		z += other.z;
		return *this;
	}
	Vector3i& operator -= (const Vector3i& other) {
		x -= other.x;
		y -= other.y;
		z -= other.z;
		return *this;
	}
	Vector3i& operator *= (const Vector3i& other) {
		x *= other.x;
		y *= other.y;
		z *= other.z;
		return *this;
	}
	Vector3i& operator /= (const Vector3i& other) {
		x /= other.x;
		y /= other.y;
		z /= other.z;
		return *this;
	}

	bool operator == (const Vector3i& other) const {
		return __builtin_memcmp(this, &other, sizeof(Vector3i)) == 0;
	}

	constexpr Vector3i() : x(0), y(0), z(0) {}
	constexpr Vector3i(int val) : x(val), y(val), z(val) {}
	constexpr Vector3i(int x, int y, int z) : x(x), y(y), z(z) {}
};
struct Vector4 {
	real_t x;
	real_t y;
	real_t z;
	real_t w;

	template <typename... Args>
	Variant operator () (std::string_view method, Args&&... args);

	Vector4& operator += (const Vector4& other) {
		x += other.x;
		y += other.y;
		z += other.z;
		w += other.w;
		return *this;
	}
	Vector4& operator -= (const Vector4& other) {
		x -= other.x;
		y -= other.y;
		z -= other.z;
		w -= other.w;
		return *this;
	}
	Vector4& operator *= (const Vector4& other) {
		x *= other.x;
		y *= other.y;
		z *= other.z;
		w *= other.w;
		return *this;
	}
	Vector4& operator /= (const Vector4& other) {
		x /= other.x;
		y /= other.y;
		z /= other.z;
		w /= other.w;
		return *this;
	}

	bool operator == (const Vector4& other) const {
		return __builtin_memcmp(this, &other, sizeof(Vector4)) == 0;
	}

	constexpr Vector4() : x(0), y(0), z(0), w(0) {}
	constexpr Vector4(real_t val) : x(val), y(val), z(val), w(val) {}
	constexpr Vector4(real_t x, real_t y, real_t z, real_t w) : x(x), y(y), z(z), w(w) {}
	constexpr Vector4(Vector3 v, real_t w) : x(v.x), y(v.y), z(v.z), w(w) {}
};
struct Vector4i {
	int x;
	int y;
	int z;
	int w;

	template <typename... Args>
	Variant operator () (std::string_view method, Args&&... args);

	Vector4i& operator += (const Vector4i& other) {
		x += other.x;
		y += other.y;
		z += other.z;
		w += other.w;
		return *this;
	}
	Vector4i& operator -= (const Vector4i& other) {
		x -= other.x;
		y -= other.y;
		z -= other.z;
		w -= other.w;
		return *this;
	}
	Vector4i& operator *= (const Vector4i& other) {
		x *= other.x;
		y *= other.y;
		z *= other.z;
		w *= other.w;
		return *this;
	}
	Vector4i& operator /= (const Vector4i& other) {
		x /= other.x;
		y /= other.y;
		z /= other.z;
		w /= other.w;
		return *this;
	}

	bool operator == (const Vector4i& other) const {
		return __builtin_memcmp(this, &other, sizeof(Vector4i)) == 0;
	}
};

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

inline auto operator + (const Vector3& a, real_t b) noexcept {
	return Vector3{a.x + b, a.y + b, a.z + b};
}
inline auto operator - (const Vector3& a, real_t b) noexcept {
	return Vector3{a.x - b, a.y - b, a.z - b};
}
inline auto operator * (const Vector3& a, real_t b) noexcept {
	return Vector3{a.x * b, a.y * b, a.z * b};
}
inline auto operator / (const Vector3& a, real_t b) noexcept {
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

inline auto operator + (const Vector4& a, real_t b) noexcept {
	return Vector4{a.x + b, a.y + b, a.z + b, a.w + b};
}
inline auto operator - (const Vector4& a, real_t b) noexcept {
	return Vector4{a.x - b, a.y - b, a.z - b, a.w - b};
}
inline auto operator * (const Vector4& a, real_t b) noexcept {
	return Vector4{a.x * b, a.y * b, a.z * b, a.w * b};
}
inline auto operator / (const Vector4& a, real_t b) noexcept {
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
inline Vector3 Vector3::floor() const noexcept {
	register const Vector3 *vptr asm("a0") = this;
	register float resultX asm("fa0");
	register float resultY asm("fa1");
	register float resultZ asm("fa2");
	register int op asm("a2") = 11; // Vec3_Op::FLOOR
	register int syscall asm("a7") = 537; // ECALL_VEC3_OPS

	__asm__ volatile("ecall"
					 : "=f"(resultX), "=f"(resultY), "=f"(resultZ)
					 : "r"(op), "r"(vptr), "m"(*vptr), "r"(syscall));
	return {resultX, resultY, resultZ};
}

namespace std
{
	inline void hash_combine(std::size_t &seed, std::size_t hash)
	{
		hash += 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= hash;
	}

	template <>
	struct hash<Vector2>
	{
		std::size_t operator()(const Vector2 &v) const
		{
			std::size_t seed = 0;
			hash_combine(seed, std::hash<real_t>{}(v.x));
			hash_combine(seed, std::hash<real_t>{}(v.y));
			return seed;
		}
	};

	template <>
	struct hash<Vector2i>
	{
		std::size_t operator()(const Vector2i &v) const
		{
			std::size_t seed = 0;
			hash_combine(seed, std::hash<int>{}(v.x));
			hash_combine(seed, std::hash<int>{}(v.y));
			return seed;
		}
	};

	template <>
	struct hash<Rect2>
	{
		std::size_t operator()(const Rect2 &r) const
		{
			std::size_t seed = 0;
			hash_combine(seed, std::hash<real_t>{}(r.position.x));
			hash_combine(seed, std::hash<real_t>{}(r.position.y));
			hash_combine(seed, std::hash<real_t>{}(r.size.x));
			hash_combine(seed, std::hash<real_t>{}(r.size.y));
			return seed;
		}
	};

	template <>
	struct hash<Rect2i>
	{
		std::size_t operator()(const Rect2i &r) const
		{
			std::size_t seed = 0;
			hash_combine(seed, std::hash<int>{}(r.position.x));
			hash_combine(seed, std::hash<int>{}(r.position.y));
			hash_combine(seed, std::hash<int>{}(r.size.x));
			hash_combine(seed, std::hash<int>{}(r.size.y));
			return seed;
		}
	};

	template <>
	struct hash<Vector3>
	{
		inline std::size_t operator()(const Vector3 &v) const {
			register const Vector3 *vptr asm("a0") = &v;
			register std::size_t hash asm("a0");
			register int op asm("a2") = 0; // Vec3_Op::HASH
			register int syscall asm("a7") = 537; // ECALL_VEC3_OPS

			__asm__ volatile("ecall"
							: "=r"(hash)
							: "r"(op), "r"(vptr), "m"(*vptr), "r"(syscall));
			return hash;
		}
	};

	template <>
	struct hash<Vector3i>
	{
		std::size_t operator()(const Vector3i &v) const
		{
			std::size_t seed = 0;
			hash_combine(seed, std::hash<int>{}(v.x));
			hash_combine(seed, std::hash<int>{}(v.y));
			hash_combine(seed, std::hash<int>{}(v.z));
			return seed;
		}
	};

	template <>
	struct hash<Vector4>
	{
		std::size_t operator()(const Vector4 &v) const
		{
			std::size_t seed = 0;
			hash_combine(seed, std::hash<real_t>{}(v.x));
			hash_combine(seed, std::hash<real_t>{}(v.y));
			hash_combine(seed, std::hash<real_t>{}(v.z));
			hash_combine(seed, std::hash<real_t>{}(v.w));
			return seed;
		}
	};

	template <>
	struct hash<Vector4i>
	{
		std::size_t operator()(const Vector4i &v) const
		{
			std::size_t seed = 0;
			hash_combine(seed, std::hash<int>{}(v.x));
			hash_combine(seed, std::hash<int>{}(v.y));
			hash_combine(seed, std::hash<int>{}(v.z));
			hash_combine(seed, std::hash<int>{}(v.w));
			return seed;
		}
	};
}
