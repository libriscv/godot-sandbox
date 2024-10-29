#pragma once
#include <cmath>
#include <string_view>
#include "syscalls_fwd.hpp"
struct Variant;
enum ClockDirection {
	CLOCKWISE = 0,
	COUNTERCLOCKWISE = 1,
};

struct Vector2 {
	real_t x;
	real_t y;

	float length() const noexcept;
	float length_squared() const noexcept { return x * x + y * y; }
	Vector2 limit_length(double length) const noexcept;

	void normalize() { *this = normalized(); }
	Vector2 normalized() const noexcept;
	float distance_to(const Vector2& other) const noexcept;
	Vector2 direction_to(const Vector2& other) const noexcept;
	float dot(const Vector2& other) const noexcept;
	static Vector2 sincos(float angle) noexcept;
	static Vector2 from_angle(float angle) noexcept;

	Vector2 lerp(const Vector2& to, double weight) const noexcept;
	Vector2 cubic_interpolate(const Vector2& b, const Vector2& pre_a, const Vector2& post_b, double weight) const noexcept;
	Vector2 slerp(const Vector2& to, double weight) const noexcept;

	Vector2 slide(const Vector2& normal) const noexcept;
	Vector2 bounce(const Vector2& normal) const noexcept;
	Vector2 reflect(const Vector2& normal) const noexcept;

	void rotate(real_t angle) noexcept { *this = rotated(angle); }
	Vector2 rotated(real_t angle) const noexcept;

	Vector2 project(const Vector2& vec) const noexcept;
	Vector2 orthogonal() const noexcept { return {y, -x}; }
	float aspect() const noexcept { return x / y; }

	real_t operator [] (int index) const {
		return index == 0 ? x : y;
	}
	real_t& operator [] (int index) {
		return index == 0 ? x : y;
	}

	METHOD(Vector2, abs);
	METHOD(Vector2, bezier_derivative);
	METHOD(Vector2, bezier_interpolate);
	METHOD(Vector2, ceil);
	METHOD(Vector2, clamp);
	METHOD(Vector2, clampf);
	METHOD(real_t,  cross);
	METHOD(Vector2, cubic_interpolate_in_time);
	METHOD(Vector2, floor);
	METHOD(bool,    is_equal_approx);
	METHOD(bool,    is_finite);
	METHOD(bool,    is_normalized);
	METHOD(bool,    is_zero_approx);
	METHOD(Vector2, max);
	METHOD(Vector2, maxf);
	METHOD(int,     max_axis_index);
	METHOD(Vector2, min);
	METHOD(Vector2, minf);
	METHOD(int,     min_axis_index);
	METHOD(Vector2, move_toward);
	METHOD(Vector2, posmod);
	METHOD(Vector2, posmodv);
	METHOD(Vector2, round);
	METHOD(Vector2, sign);
	METHOD(Vector2, snapped);
	METHOD(Vector2, snappedf);

	template <typename... Args>
	Variant operator () (std::string_view method, Args&&... args);

	Vector2& operator += (const Vector2& other) {
		x += other.x;
		y += other.y;
		return *this;
	}
	Vector2& operator -= (const Vector2& other) {
		x -= other.x;
		y -= other.y;
		return *this;
	}
	Vector2& operator *= (const Vector2& other) {
		x *= other.x;
		y *= other.y;
		return *this;
	}
	Vector2& operator /= (const Vector2& other) {
		x /= other.x;
		y /= other.y;
		return *this;
	}

	bool operator == (const Vector2& other) const {
		return x == other.x && y == other.y;
	}

	constexpr Vector2() : x(0), y(0) {}
	constexpr Vector2(real_t val) : x(val), y(val) {}
	constexpr Vector2(real_t x, real_t y) : x(x), y(y) {}

	static Vector2 const ZERO;
	static Vector2 const ONE;
	static Vector2 const LEFT;
	static Vector2 const RIGHT;
	static Vector2 const UP;
	static Vector2 const DOWN;
};
inline constexpr Vector2 const Vector2::ZERO = Vector2(0, 0);
inline constexpr Vector2 const Vector2::ONE = Vector2(1, 1);
inline constexpr Vector2 const Vector2::LEFT = Vector2(-1, 0);
inline constexpr Vector2 const Vector2::RIGHT = Vector2(1, 0);
inline constexpr Vector2 const Vector2::UP = Vector2(0, -1);
inline constexpr Vector2 const Vector2::DOWN = Vector2(0, 1);

struct Vector2i {
	int x;
	int y;

	template <typename... Args>
	Variant operator () (std::string_view method, Args&&... args);

	Vector2i& operator += (const Vector2i& other) {
		x += other.x;
		y += other.y;
		return *this;
	}
	Vector2i& operator -= (const Vector2i& other) {
		x -= other.x;
		y -= other.y;
		return *this;
	}
	Vector2i& operator *= (const Vector2i& other) {
		x *= other.x;
		y *= other.y;
		return *this;
	}
	Vector2i& operator /= (const Vector2i& other) {
		x /= other.x;
		y /= other.y;
		return *this;
	}

	bool operator == (const Vector2i& other) const {
		return x == other.x && y == other.y;
	}

	constexpr Vector2i() : x(0), y(0) {}
	constexpr Vector2i(int val) : x(val), y(val) {}
	constexpr Vector2i(int x, int y) : x(x), y(y) {}
};
struct Rect2 {
	Vector2 position;
	Vector2 size;

	template <typename... Args>
	Variant operator () (std::string_view method, Args&&... args);

	bool operator == (const Rect2& other) const {
		return __builtin_memcmp(this, &other, sizeof(Rect2)) == 0;
	}

	constexpr Rect2() : position(), size() {}
	constexpr Rect2(Vector2 position, Vector2 size) : position(position), size(size) {}
	constexpr Rect2(real_t x, real_t y, real_t width, real_t height) : position(x, y), size(width, height) {}
};
struct Rect2i {
	Vector2i position;
	Vector2i size;

	template <typename... Args>
	Variant operator () (std::string_view method, Args&&... args);

	bool operator == (const Rect2i& other) const {
		return __builtin_memcmp(this, &other, sizeof(Rect2i)) == 0;
	}

	constexpr Rect2i() : position(), size() {}
	constexpr Rect2i(Vector2i position, Vector2i size) : position(position), size(size) {}
	constexpr Rect2i(int x, int y, int width, int height) : position(x, y), size(width, height) {}
};
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

inline auto operator + (const Vector2& a, real_t b) noexcept {
	return Vector2{a.x + b, a.y + b};
}
inline auto operator - (const Vector2& a, real_t b) noexcept {
	return Vector2{a.x - b, a.y - b};
}
inline auto operator * (const Vector2& a, real_t b) noexcept {
	return Vector2{a.x * b, a.y * b};
}
inline auto operator / (const Vector2& a, real_t b) noexcept {
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
inline Vector2 Vector2::sincos(float angle) noexcept {
	register float s asm("fa0") = angle;
	register float c asm("fa1");
	register int syscall asm("a7") = 513; // ECALL_SINCOS

	__asm__ volatile("ecall"
					 : "+f"(s), "=f"(c)
					 : "r"(syscall));
	return {s, c}; // (sine, cosine)
}
inline Vector2 Vector2::from_angle(float angle) noexcept {
	Vector2 v = sincos(angle);
	return {v.y, v.x}; // (cos(angle), sin(angle))
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
