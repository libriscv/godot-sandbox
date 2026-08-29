#pragma once

#include "variant_types.h"
#include <cstdint>
#include <string>

namespace gdscript {

// The run-time encoding intentionally matches TYPE_TEST_MASK: bit N accepts
// Variant::Type N.  A zero mask means the ordinary untyped Variant slot.
struct TypeSet {
	uint64_t mask = 0;

	bool any() const { return mask == 0; }
	bool single() const { return mask != 0 && (mask & (mask - 1)) == 0; }
	Variant::Type only() const {
		return single() ? static_cast<Variant::Type>(__builtin_ctzll(mask))
		                : Variant::VARIANT_MAX;
	}
	bool contains(Variant::Type type) const {
		return type < Variant::VARIANT_MAX && (mask & (uint64_t(1) << type)) != 0;
	}
	TypeSet intersect(TypeSet other) const { return { mask & other.mask }; }
	TypeSet without(Variant::Type type) const {
		return type < Variant::VARIANT_MAX ? TypeSet{mask & ~(uint64_t(1) << type)} : *this;
	}
	TypeSet non_null() const { return without(Variant::NIL); }
	bool is_nullable_single() const {
		return contains(Variant::NIL) && non_null().single();
	}

	std::string to_string() const {
		static constexpr const char* names[] = {
			"null", "bool", "int", "float", "String", "Vector2", "Vector2i",
			"Rect2", "Rect2i", "Vector3", "Vector3i", "Transform2D", "Vector4",
			"Vector4i", "Plane", "Quaternion", "AABB", "Basis", "Transform3D",
			"Projection", "Color", "StringName", "NodePath", "RID", "Object",
			"Callable", "Signal", "Dictionary", "Array", "PackedByteArray",
			"PackedInt32Array", "PackedInt64Array", "PackedFloat32Array",
			"PackedFloat64Array", "PackedStringArray", "PackedVector2Array",
			"PackedVector3Array", "PackedColorArray", "PackedVector4Array"
		};
		std::string result;
		// Keep null last, matching source spelling and diagnostics in the proposal.
		for (uint32_t i = 1; i < Variant::VARIANT_MAX; i++) {
			if ((mask & (uint64_t(1) << i)) == 0) continue;
			if (!result.empty()) result += " | ";
			result += names[i];
		}
		if (contains(Variant::NIL)) {
			if (!result.empty()) result += " | ";
			result += "null";
		}
		return result;
	}
};

} // namespace gdscript
