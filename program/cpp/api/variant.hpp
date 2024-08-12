#pragma once
#include <array>
#include <cstdint>
#include <type_traits>
#include <stdexcept>
#include <string_view>
#include <span>
#include <vector>

template<typename T>
struct is_string
	: public std::disjunction<
		std::is_same<char *, typename std::decay<T>::type>,
		std::is_same<const char *, typename std::decay<T>::type>
> {};

template<class T>
struct is_stdstring : public std::is_same<T, std::basic_string<char>> {};

struct Node2D;
#include "vector.hpp"

struct Variant
{
	enum Type {
		NIL,

		// atomic types
		BOOL,
		INT,
		FLOAT,
		STRING,

		// math types
		VECTOR2,
		VECTOR2I,
		RECT2,
		RECT2I,
		VECTOR3,
		VECTOR3I,
		TRANSFORM2D,
		VECTOR4,
		VECTOR4I,
		PLANE,
		QUATERNION,
		AABB,
		BASIS,
		TRANSFORM3D,
		PROJECTION,

		// misc types
		COLOR,
		STRING_NAME,
		NODE_PATH,
		RID,
		OBJECT,
		CALLABLE,
		SIGNAL,
		DICTIONARY,
		ARRAY,

		// typed arrays
		PACKED_BYTE_ARRAY,
		PACKED_INT32_ARRAY,
		PACKED_INT64_ARRAY,
		PACKED_FLOAT32_ARRAY,
		PACKED_FLOAT64_ARRAY,
		PACKED_STRING_ARRAY,
		PACKED_VECTOR2_ARRAY,
		PACKED_VECTOR3_ARRAY,
		PACKED_COLOR_ARRAY,

		VARIANT_MAX
	};

	enum Operator {
		// comparison
		OP_EQUAL,
		OP_NOT_EQUAL,
		OP_LESS,
		OP_LESS_EQUAL,
		OP_GREATER,
		OP_GREATER_EQUAL,
		// mathematic
		OP_ADD,
		OP_SUBTRACT,
		OP_MULTIPLY,
		OP_DIVIDE,
		OP_NEGATE,
		OP_POSITIVE,
		OP_MODULE,
		OP_POWER,
		// bitwise
		OP_SHIFT_LEFT,
		OP_SHIFT_RIGHT,
		OP_BIT_AND,
		OP_BIT_OR,
		OP_BIT_XOR,
		OP_BIT_NEGATE,
		// logic
		OP_AND,
		OP_OR,
		OP_XOR,
		OP_NOT,
		// containment
		OP_IN,
		OP_MAX
	};

	Variant() = default;
	Variant(const Variant &other);
	Variant(Variant &&other);
	~Variant();

	// Constructor for integers and floats
	template <typename T>
	Variant(T value)
	{
		if constexpr (std::is_same_v<T, bool>) {
			m_type = BOOL;
			v.b = value;
		}
		else if constexpr (std::is_integral_v<T>) {
			m_type = INT;
			v.i = value;
		}
		else if constexpr (std::is_floating_point_v<T>) {
			m_type = FLOAT;
			v.f = value;
		}
		else if constexpr (is_string<T>::value || is_stdstring<T>::value) {
			m_type = STRING;
			v.s = new std::string(value);
		}
		else if constexpr (std::is_same_v<T, Vector2>) {
			m_type = VECTOR2;
			v.v2 = value;
		}
		else if constexpr (std::is_same_v<T, Vector2i>) {
			m_type = VECTOR2I;
			v.v2i = value;
		}
		else if constexpr (std::is_same_v<T, Vector3>) {
			m_type = VECTOR3;
			v.v3 = value;
		}
		else if constexpr (std::is_same_v<T, Vector3i>) {
			m_type = VECTOR3I;
			v.v3i = value;
		}
		else if constexpr (std::is_same_v<T, Vector4>) {
			m_type = VECTOR4;
			v.v4 = value;
		}
		else if constexpr (std::is_same_v<T, Vector4i>) {
			m_type = VECTOR4I;
			v.v4i = value;
		}
		else if constexpr (std::is_same_v<T, Rect2>) {
			m_type = RECT2;
			v.r2 = value;
		}
		else if constexpr (std::is_same_v<T, Rect2i>) {
			m_type = RECT2I;
			v.r2i = value;
		}
		else
			static_assert(!std::is_same_v<T, T>, "Unsupported type");
	}

	operator bool() const;
	operator int64_t() const;
	operator int32_t() const;
	operator int16_t() const;
	operator int8_t() const;
	operator uint64_t() const;
	operator uint32_t() const;
	operator uint16_t() const;
	operator uint8_t() const;
	operator double() const;
	operator float() const;
	operator const std::string&() const; // String for STRING and PACKED_BYTE_ARRAY
	operator std::string&();
	operator std::string_view() const; // View for STRING and PACKED_BYTE_ARRAY
	operator std::span<uint8_t>() const; // Modifiable span for PACKED_BYTE_ARRAY

	Node2D as_node2d() const;

	const Vector2& v2() const;
	Vector2& v2();
	const Vector2i& v2i() const;
	Vector2i& v2i();
	const Vector3& v3() const;
	Vector3& v3();
	const Vector3i& v3i() const;
	Vector3i& v3i();
	const Vector4& v4() const;
	Vector4& v4();
	const Vector4i& v4i() const;
	Vector4i& v4i();
	const Rect2& r2() const;
	Rect2& r2();
	const Rect2i& r2i() const;
	Rect2i& r2i();

	std::vector<float>& f32array() const; // Modifiable vector for PACKED_FLOAT32_ARRAY
	std::vector<double>& f64array() const; // Modifiable vector for PACKED_FLOAT64_ARRAY

	void callp(const std::string &method, const Variant *args, int argcount, Variant &r_ret, int &r_error);

	template <typename... Args>
	Variant method_call(const std::string &method, Args... args) {
		std::array<Variant, sizeof...(args)> vargs = { args... };
		Variant result;
		int error;
		callp(method, vargs.data(), vargs.size(), result, error);
		return result;
	}

	template <typename... Args>
	Variant call(Args... args) {
		std::array<Variant, sizeof...(args)> vargs = { args... };
		Variant result;
		int error;
		callp("call", vargs.data(), vargs.size(), result, error);
		return result;
	}

	static void evaluate(const Operator &op, const Variant &a, const Variant &b, Variant &r_ret, bool &r_valid);

	Variant &operator=(const Variant &other);
	Variant &operator=(Variant &&other);
	bool operator==(const Variant &other) const;
	bool operator!=(const Variant &other) const;
	bool operator<(const Variant &other) const;

	Type get_type() const noexcept { return m_type; }

	static constexpr unsigned GODOT_VARIANT_SIZE = 24;
private:
	Type m_type = NIL;
	union {
		uint8_t opaque[GODOT_VARIANT_SIZE] { 0 };
		bool    b;
		int64_t i;
		double  f;
		Vector2  v2;
		Vector2i v2i;
		Vector3  v3;
		Vector3i v3i;
		Vector4  v4;
		Vector4i v4i;
		Rect2    r2;
		Rect2i   r2i;
		std::string *s;
		std::vector<float> *f32array;
		std::vector<double> *f64array;
	} v;
};

inline Variant::operator bool() const
{
	if (m_type == BOOL)
		return v.b;
	throw std::bad_cast();
}

inline Variant::operator int64_t() const
{
	if (m_type == INT)
		return v.i;
	throw std::bad_cast();
}

inline Variant::operator int32_t() const
{
	if (m_type == INT)
		return static_cast<int32_t>(v.i);
	throw std::bad_cast();
}

inline Variant::operator int16_t() const
{
	if (m_type == INT)
		return static_cast<int16_t>(v.i);
	throw std::bad_cast();
}

inline Variant::operator int8_t() const
{
	if (m_type == INT)
		return static_cast<int8_t>(v.i);
	throw std::bad_cast();
}

inline Variant::operator uint64_t() const
{
	if (m_type == INT)
		return static_cast<uint64_t>(v.i);
	throw std::bad_cast();
}

inline Variant::operator uint32_t() const
{
	if (m_type == INT)
		return static_cast<uint32_t>(v.i);
	throw std::bad_cast();
}

inline Variant::operator uint16_t() const
{
	if (m_type == INT)
		return static_cast<uint16_t>(v.i);
	throw std::bad_cast();
}

inline Variant::operator uint8_t() const
{
	if (m_type == INT)
		return static_cast<uint8_t>(v.i);
	throw std::bad_cast();
}

inline Variant::operator double() const
{
	if (m_type == FLOAT)
		return v.f;
	throw std::bad_cast();
}

inline Variant::operator float() const
{
	if (m_type == FLOAT)
		return static_cast<float>(v.f);
	throw std::bad_cast();
}

inline Variant::operator const std::string&() const
{
	if (m_type == STRING || m_type == PACKED_BYTE_ARRAY)
		return *v.s;
	throw std::bad_cast();
}
inline Variant::operator std::string&()
{
	if (m_type == STRING || m_type == PACKED_BYTE_ARRAY)
		return *v.s;
	throw std::bad_cast();
}

inline Variant::operator std::string_view() const
{
	if (m_type == STRING || m_type == PACKED_BYTE_ARRAY)
		return std::string_view(*v.s);
	throw std::bad_cast();
}

inline Variant::operator std::span<uint8_t>() const
{
	if (m_type == PACKED_BYTE_ARRAY)
		return std::span<uint8_t>(reinterpret_cast<uint8_t *>(v.s->data()), v.s->size());
	throw std::bad_cast();
}

inline const Vector2& Variant::v2() const
{
	if (m_type == VECTOR2)
		return v.v2;
	throw std::bad_cast();
}

inline Vector2& Variant::v2()
{
	if (m_type == VECTOR2)
		return v.v2;
	throw std::bad_cast();
}

inline const Vector2i& Variant::v2i() const
{
	if (m_type == VECTOR2I)
		return v.v2i;
	throw std::bad_cast();
}

inline Vector2i& Variant::v2i()
{
	if (m_type == VECTOR2I)
		return v.v2i;
	throw std::bad_cast();
}

inline const Vector3& Variant::v3() const
{
	if (m_type == VECTOR3)
		return v.v3;
	throw std::bad_cast();
}

inline Vector3& Variant::v3()
{
	if (m_type == VECTOR3)
		return v.v3;
	throw std::bad_cast();
}

inline const Vector3i& Variant::v3i() const
{
	if (m_type == VECTOR3I)
		return v.v3i;
	throw std::bad_cast();
}

inline Vector3i& Variant::v3i()
{
	if (m_type == VECTOR3I)
		return v.v3i;
	throw std::bad_cast();
}

inline const Vector4& Variant::v4() const
{
	if (m_type == VECTOR4)
		return v.v4;
	throw std::bad_cast();
}

inline Vector4& Variant::v4()
{
	if (m_type == VECTOR4)
		return v.v4;
	throw std::bad_cast();
}

inline const Vector4i& Variant::v4i() const
{
	if (m_type == VECTOR4I)
		return v.v4i;
	throw std::bad_cast();
}

inline Vector4i& Variant::v4i()
{
	if (m_type == VECTOR4I)
		return v.v4i;
	throw std::bad_cast();
}

inline const Rect2& Variant::r2() const
{
	if (m_type == RECT2)
		return v.r2;
	throw std::bad_cast();
}

inline Rect2& Variant::r2()
{
	if (m_type == RECT2)
		return v.r2;
	throw std::bad_cast();
}

inline const Rect2i& Variant::r2i() const
{
	if (m_type == RECT2I)
		return v.r2i;
	throw std::bad_cast();
}

inline Rect2i& Variant::r2i()
{
	if (m_type == RECT2I)
		return v.r2i;
	throw std::bad_cast();
}

inline std::vector<float>& Variant::f32array() const
{
	if (m_type == PACKED_FLOAT32_ARRAY)
		return *v.f32array;
	throw std::bad_cast();
}

inline std::vector<double>& Variant::f64array() const
{
	if (m_type == PACKED_FLOAT64_ARRAY)
		return *v.f64array;
	throw std::bad_cast();
}

inline Variant::Variant(const Variant &other)
{
	m_type = other.m_type;
	if (m_type == STRING || m_type == PACKED_BYTE_ARRAY || m_type == NODE_PATH)
		v.s = new std::string(*other.v.s);
	else if (m_type == PACKED_FLOAT32_ARRAY)
		v.f32array = new std::vector<float>(*other.v.f32array);
	else if (m_type == PACKED_FLOAT64_ARRAY)
		v.f64array = new std::vector<double>(*other.v.f64array);
	else
		v = other.v;
}
inline Variant::Variant(Variant &&other)
{
	m_type = other.m_type;
	v = other.v;

	other.m_type = NIL;
}
inline Variant::~Variant()
{
	if (m_type == STRING || m_type == PACKED_BYTE_ARRAY || m_type == NODE_PATH)
		delete v.s;
	else if (m_type == PACKED_FLOAT32_ARRAY)
		delete v.f32array;
	else if (m_type == PACKED_FLOAT64_ARRAY)
		delete v.f64array;
}

inline Variant &Variant::operator=(const Variant &other) {
	m_type = other.m_type;
	if (m_type == STRING || m_type == PACKED_BYTE_ARRAY || m_type == NODE_PATH)
		v.s = new std::string(*other.v.s);
	else if (m_type == PACKED_FLOAT32_ARRAY)
		v.f32array = new std::vector<float>(*other.v.f32array);
	else if (m_type == PACKED_FLOAT64_ARRAY)
		v.f64array = new std::vector<double>(*other.v.f64array);
	else
		v = other.v;

	return *this;
}
inline Variant &Variant::operator=(Variant &&other) {
	m_type = other.m_type;
	v = other.v;

	other.m_type = NIL;
	return *this;
}

inline bool Variant::operator==(const Variant &other) const {
	if (get_type() != other.get_type()) {
		return false;
	}
	bool valid = false;
	Variant result;
	evaluate(OP_EQUAL, *this, other, result, valid);
	return result.operator bool();
}
inline bool Variant::operator!=(const Variant &other) const {
	if (get_type() != other.get_type()) {
		return true;
	}
	bool valid = false;
	Variant result;
	evaluate(OP_NOT_EQUAL, *this, other, result, valid);
	return result.operator bool();
}
inline bool Variant::operator<(const Variant &other) const {
	if (get_type() != other.get_type()) {
		return get_type() < other.get_type();
	}
	bool valid = false;
	Variant result;
	evaluate(OP_LESS, *this, other, result, valid);
	return result.operator bool();
}
