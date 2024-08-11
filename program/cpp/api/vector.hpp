#pragma once

struct Vector2 {
	float x;
	float y;

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

inline auto operator + (const Vector2& a, const Vector2& b) {
	return Vector2{a.x + b.x, a.y + b.y};
}
inline auto operator - (const Vector2& a, const Vector2& b) {
	return Vector2{a.x - b.x, a.y - b.y};
}
inline auto operator * (const Vector2& a, const Vector2& b) {
	return Vector2{a.x * b.x, a.y * b.y};
}
inline auto operator / (const Vector2& a, const Vector2& b) {
	return Vector2{a.x / b.x, a.y / b.y};
}

inline auto operator + (const Vector2i& a, const Vector2i& b) {
	return Vector2i{a.x + b.x, a.y + b.y};
}
inline auto operator - (const Vector2i& a, const Vector2i& b) {
	return Vector2i{a.x - b.x, a.y - b.y};
}
inline auto operator * (const Vector2i& a, const Vector2i& b) {
	return Vector2i{a.x * b.x, a.y * b.y};
}
inline auto operator / (const Vector2i& a, const Vector2i& b) {
	return Vector2i{a.x / b.x, a.y / b.y};
}

inline auto operator + (const Rect2& a, const Rect2& b) {
	return Rect2{a.x + b.x, a.y + b.y, a.w + b.w, a.h + b.h};
}
inline auto operator - (const Rect2& a, const Rect2& b) {
	return Rect2{a.x - b.x, a.y - b.y, a.w - b.w, a.h - b.h};
}
inline auto operator * (const Rect2& a, const Rect2& b) {
	return Rect2{a.x * b.x, a.y * b.y, a.w * b.w, a.h * b.h};
}
inline auto operator / (const Rect2& a, const Rect2& b) {
	return Rect2{a.x / b.x, a.y / b.y, a.w / b.w, a.h / b.h};
}

inline auto operator + (const Rect2i& a, const Rect2i& b) {
	return Rect2i{a.x + b.x, a.y + b.y, a.w + b.w, a.h + b.h};
}
inline auto operator - (const Rect2i& a, const Rect2i& b) {
	return Rect2i{a.x - b.x, a.y - b.y, a.w - b.w, a.h - b.h};
}
inline auto operator * (const Rect2i& a, const Rect2i& b) {
	return Rect2i{a.x * b.x, a.y * b.y, a.w * b.w, a.h * b.h};
}
inline auto operator / (const Rect2i& a, const Rect2i& b) {
	return Rect2i{a.x / b.x, a.y / b.y, a.w / b.w, a.h / b.h};
}

inline auto operator + (const Vector3& a, const Vector3& b) {
	return Vector3{a.x + b.x, a.y + b.y, a.z + b.z};
}
inline auto operator - (const Vector3& a, const Vector3& b) {
	return Vector3{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline auto operator * (const Vector3& a, const Vector3& b) {
	return Vector3{a.x * b.x, a.y * b.y, a.z * b.z};
}
inline auto operator / (const Vector3& a, const Vector3& b) {
	return Vector3{a.x / b.x, a.y / b.y, a.z / b.z};
}

inline auto operator + (const Vector3i& a, const Vector3i& b) {
	return Vector3i{a.x + b.x, a.y + b.y, a.z + b.z};
}
inline auto operator - (const Vector3i& a, const Vector3i& b) {
	return Vector3i{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline auto operator * (const Vector3i& a, const Vector3i& b) {
	return Vector3i{a.x * b.x, a.y * b.y, a.z * b.z};
}
inline auto operator / (const Vector3i& a, const Vector3i& b) {
	return Vector3i{a.x / b.x, a.y / b.y, a.z / b.z};
}

inline auto operator + (const Vector4& a, const Vector4& b) {
	return Vector4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
inline auto operator - (const Vector4& a, const Vector4& b) {
	return Vector4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
inline auto operator * (const Vector4& a, const Vector4& b) {
	return Vector4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}
inline auto operator / (const Vector4& a, const Vector4& b) {
	return Vector4{a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w};
}

inline auto operator + (const Vector4i& a, const Vector4i& b) {
	return Vector4i{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
inline auto operator - (const Vector4i& a, const Vector4i& b) {
	return Vector4i{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
inline auto operator * (const Vector4i& a, const Vector4i& b) {
	return Vector4i{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}
inline auto operator / (const Vector4i& a, const Vector4i& b) {
	return Vector4i{a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w};
}

inline auto operator + (const Vector2& a, float b) {
	return Vector2{a.x + b, a.y + b};
}
inline auto operator - (const Vector2& a, float b) {
	return Vector2{a.x - b, a.y - b};
}
inline auto operator * (const Vector2& a, float b) {
	return Vector2{a.x * b, a.y * b};
}
inline auto operator / (const Vector2& a, float b) {
	return Vector2{a.x / b, a.y / b};
}

inline auto operator + (const Vector2i& a, int b) {
	return Vector2i{a.x + b, a.y + b};
}
inline auto operator - (const Vector2i& a, int b) {
	return Vector2i{a.x - b, a.y - b};
}
inline auto operator * (const Vector2i& a, int b) {
	return Vector2i{a.x * b, a.y * b};
}
inline auto operator / (const Vector2i& a, int b) {
	return Vector2i{a.x / b, a.y / b};
}

inline auto operator + (const Rect2& a, float b) {
	return Rect2{a.x + b, a.y + b, a.w + b, a.h + b};
}
inline auto operator - (const Rect2& a, float b) {
	return Rect2{a.x - b, a.y - b, a.w - b, a.h - b};
}
inline auto operator * (const Rect2& a, float b) {
	return Rect2{a.x * b, a.y * b, a.w * b, a.h * b};
}
inline auto operator / (const Rect2& a, float b) {
	return Rect2{a.x / b, a.y / b, a.w / b, a.h / b};
}

inline auto operator + (const Rect2i& a, int b) {
	return Rect2i{a.x + b, a.y + b, a.w + b, a.h + b};
}
inline auto operator - (const Rect2i& a, int b) {
	return Rect2i{a.x - b, a.y - b, a.w - b, a.h - b};
}
inline auto operator * (const Rect2i& a, int b) {
	return Rect2i{a.x * b, a.y * b, a.w * b, a.h * b};
}
inline auto operator / (const Rect2i& a, int b) {
	return Rect2i{a.x / b, a.y / b, a.w / b, a.h / b};
}

inline auto operator + (const Vector3& a, float b) {
	return Vector3{a.x + b, a.y + b, a.z + b};
}
inline auto operator - (const Vector3& a, float b) {
	return Vector3{a.x - b, a.y - b, a.z - b};
}
inline auto operator * (const Vector3& a, float b) {
	return Vector3{a.x * b, a.y * b, a.z * b};
}
inline auto operator / (const Vector3& a, float b) {
	return Vector3{a.x / b, a.y / b, a.z / b};
}

inline auto operator + (const Vector3i& a, int b) {
	return Vector3i{a.x + b, a.y + b, a.z + b};
}
inline auto operator - (const Vector3i& a, int b) {
	return Vector3i{a.x - b, a.y - b, a.z - b};
}
inline auto operator * (const Vector3i& a, int b) {
	return Vector3i{a.x * b, a.y * b, a.z * b};
}
inline auto operator / (const Vector3i& a, int b) {
	return Vector3i{a.x / b, a.y / b, a.z / b};
}

inline auto operator + (const Vector4& a, float b) {
	return Vector4{a.x + b, a.y + b, a.z + b, a.w + b};
}
inline auto operator - (const Vector4& a, float b) {
	return Vector4{a.x - b, a.y - b, a.z - b, a.w - b};
}
inline auto operator * (const Vector4& a, float b) {
	return Vector4{a.x * b, a.y * b, a.z * b, a.w * b};
}
inline auto operator / (const Vector4& a, float b) {
	return Vector4{a.x / b, a.y / b, a.z / b, a.w / b};
}

inline auto operator + (const Vector4i& a, int b) {
	return Vector4i{a.x + b, a.y + b, a.z + b, a.w + b};
}
inline auto operator - (const Vector4i& a, int b) {
	return Vector4i{a.x - b, a.y - b, a.z - b, a.w - b};
}
inline auto operator * (const Vector4i& a, int b) {
	return Vector4i{a.x * b, a.y * b, a.z * b, a.w * b};
}
inline auto operator / (const Vector4i& a, int b) {
	return Vector4i{a.x / b, a.y / b, a.z / b, a.w / b};
}
