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
};
struct Vector2i {
	int x;
	int y;

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
};
struct Rect2 {
	float x;
	float y;
	float w;
	float h;

	Rect2& operator += (const Rect2& other) {
		x += other.x;
		y += other.y;
		w += other.w;
		h += other.h;
		return *this;
	}
	Rect2& operator -= (const Rect2& other) {
		x -= other.x;
		y -= other.y;
		w -= other.w;
		h -= other.h;
		return *this;
	}
	Rect2& operator *= (const Rect2& other) {
		x *= other.x;
		y *= other.y;
		w *= other.w;
		h *= other.h;
		return *this;
	}
	Rect2& operator /= (const Rect2& other) {
		x /= other.x;
		y /= other.y;
		w /= other.w;
		h /= other.h;
		return *this;
	}

	bool operator == (const Rect2& other) const {
		return __builtin_memcmp(this, &other, sizeof(Rect2)) == 0;
	}
};
struct Rect2i {
	int x;
	int y;
	int w;
	int h;

	Rect2i& operator += (const Rect2i& other) {
		x += other.x;
		y += other.y;
		w += other.w;
		h += other.h;
		return *this;
	}
	Rect2i& operator -= (const Rect2i& other) {
		x -= other.x;
		y -= other.y;
		w -= other.w;
		h -= other.h;
		return *this;
	}
	Rect2i& operator *= (const Rect2i& other) {
		x *= other.x;
		y *= other.y;
		w *= other.w;
		h *= other.h;
		return *this;
	}
	Rect2i& operator /= (const Rect2i& other) {
		x /= other.x;
		y /= other.y;
		w /= other.w;
		h /= other.h;
		return *this;
	}

	bool operator == (const Rect2i& other) const {
		return __builtin_memcmp(this, &other, sizeof(Rect2i)) == 0;
	}
};
struct Vector3 {
	float x;
	float y;
	float z;

	float length() const noexcept;
	Vector3 normalized() const noexcept;
	Vector3 cross(const Vector3& other) const noexcept;
	float distance_to(const Vector3& other) const noexcept;
	Vector3 direction_to(const Vector3& other) const noexcept;
	float dot(const Vector3& other) const noexcept;

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
};
struct Vector3i {
	int x;
	int y;
	int z;

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
};
struct Vector4 {
	float x;
	float y;
	float z;
	float w;

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
};
struct Vector4i {
	int x;
	int y;
	int z;
	int w;

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
	register float x asm("fa0") = angle;
	register float y asm("fa1");
	register int syscall asm("a7") = 513; // ECALL_SINCOS

	__asm__ volatile("ecall"
					 : "+f"(x), "=f"(y)
					 : "r"(syscall));
	return {x, y};
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
			hash_combine(seed, std::hash<float>{}(v.x));
			hash_combine(seed, std::hash<float>{}(v.y));
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
			hash_combine(seed, std::hash<float>{}(r.x));
			hash_combine(seed, std::hash<float>{}(r.y));
			hash_combine(seed, std::hash<float>{}(r.w));
			hash_combine(seed, std::hash<float>{}(r.h));
			return seed;
		}
	};

	template <>
	struct hash<Rect2i>
	{
		std::size_t operator()(const Rect2i &r) const
		{
			std::size_t seed = 0;
			hash_combine(seed, std::hash<int>{}(r.x));
			hash_combine(seed, std::hash<int>{}(r.y));
			hash_combine(seed, std::hash<int>{}(r.w));
			hash_combine(seed, std::hash<int>{}(r.h));
			return seed;
		}
	};

	template <>
	struct hash<Vector3>
	{
		inline std::size_t operator()(const Vector3 &v) const {
			register int op asm("a0") = 0; // Vec3_Op::HASH
			register const Vector3 *vptr asm("a1") = &v;
			register std::size_t hash asm("a0");
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
			hash_combine(seed, std::hash<float>{}(v.x));
			hash_combine(seed, std::hash<float>{}(v.y));
			hash_combine(seed, std::hash<float>{}(v.z));
			hash_combine(seed, std::hash<float>{}(v.w));
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
