#pragma once

#include <cstdint>
#include <string_view>

namespace Variant {

enum Type : uint32_t {
	NIL = 0,

	// atomic types
	BOOL = 1,
	INT = 2,
	FLOAT = 3,
	STRING = 4,

	// math types
	VECTOR2 = 5,
	VECTOR2I = 6,
	RECT2 = 7,
	RECT2I = 8,
	VECTOR3 = 9,
	VECTOR3I = 10,
	TRANSFORM2D = 11,
	VECTOR4 = 12,
	VECTOR4I = 13,
	PLANE = 14,
	QUATERNION = 15,
	AABB = 16,
	BASIS = 17,
	TRANSFORM3D = 18,
	PROJECTION = 19,

	// misc types
	COLOR = 20,
	STRING_NAME = 21,
	NODE_PATH = 22,
	RID = 23,
	OBJECT = 24,
	CALLABLE = 25,
	SIGNAL = 26,
	DICTIONARY = 27,
	ARRAY = 28,

	// typed arrays
	PACKED_BYTE_ARRAY = 29,
	PACKED_INT32_ARRAY = 30,
	PACKED_INT64_ARRAY = 31,
	PACKED_FLOAT32_ARRAY = 32,
	PACKED_FLOAT64_ARRAY = 33,
	PACKED_STRING_ARRAY = 34,
	PACKED_VECTOR2_ARRAY = 35,
	PACKED_VECTOR3_ARRAY = 36,
	PACKED_COLOR_ARRAY = 37,
	PACKED_VECTOR4_ARRAY = 38,

	VARIANT_MAX
};

// Nil and Object absent: neither is a cast target.
inline Type type_from_name(std::string_view name) {
	struct Row {
		std::string_view name;
		Type type;
	};
	static constexpr Row rows[] = {
		{ "bool", BOOL },
		{ "int", INT },
		{ "float", FLOAT },
		{ "String", STRING },
		{ "Vector2", VECTOR2 },
		{ "Vector2i", VECTOR2I },
		{ "Rect2", RECT2 },
		{ "Rect2i", RECT2I },
		{ "Vector3", VECTOR3 },
		{ "Vector3i", VECTOR3I },
		{ "Transform2D", TRANSFORM2D },
		{ "Vector4", VECTOR4 },
		{ "Vector4i", VECTOR4I },
		{ "Plane", PLANE },
		{ "Quaternion", QUATERNION },
		{ "AABB", AABB },
		{ "Basis", BASIS },
		{ "Transform3D", TRANSFORM3D },
		{ "Projection", PROJECTION },
		{ "Color", COLOR },
		{ "StringName", STRING_NAME },
		{ "NodePath", NODE_PATH },
		{ "RID", RID },
		{ "Callable", CALLABLE },
		{ "Signal", SIGNAL },
		{ "Dictionary", DICTIONARY },
		{ "Array", ARRAY },
		{ "PackedByteArray", PACKED_BYTE_ARRAY },
		{ "PackedInt32Array", PACKED_INT32_ARRAY },
		{ "PackedInt64Array", PACKED_INT64_ARRAY },
		{ "PackedFloat32Array", PACKED_FLOAT32_ARRAY },
		{ "PackedFloat64Array", PACKED_FLOAT64_ARRAY },
		{ "PackedStringArray", PACKED_STRING_ARRAY },
		{ "PackedVector2Array", PACKED_VECTOR2_ARRAY },
		{ "PackedVector3Array", PACKED_VECTOR3_ARRAY },
		{ "PackedColorArray", PACKED_COLOR_ARRAY },
		{ "PackedVector4Array", PACKED_VECTOR4_ARRAY },
	};
	for (const Row& row : rows) {
		if (row.name == name) {
			return row.type;
		}
	}
	return VARIANT_MAX;
}

} // namespace Variant
