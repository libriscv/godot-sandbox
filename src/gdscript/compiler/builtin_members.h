#pragma once
#include "variant_types.h"
#include <string>

namespace gdscript {

struct BuiltinMember {
	int first_component = 0;
	int count = 0;
	uint32_t result_type = Variant::NIL;
	bool integer = false;

	bool valid() const { return count != 0; }
};

inline BuiltinMember find_builtin_member(uint32_t type, const std::string& member) {
	const auto scalar = [](int index, bool integer) {
		return BuiltinMember{ index, 1, integer ? Variant::INT : Variant::FLOAT, integer };
	};
	const auto pair = [](int index, bool integer) {
		return BuiltinMember{ index, 2, integer ? Variant::VECTOR2I : Variant::VECTOR2, integer };
	};

	switch (type) {
		case Variant::VECTOR2:
		case Variant::VECTOR2I: {
			const bool integer = type == Variant::VECTOR2I;
			if (member == "x") return scalar(0, integer);
			if (member == "y") return scalar(1, integer);
			return {};
		}

		case Variant::VECTOR3:
		case Variant::VECTOR3I: {
			const bool integer = type == Variant::VECTOR3I;
			if (member == "x") return scalar(0, integer);
			if (member == "y") return scalar(1, integer);
			if (member == "z") return scalar(2, integer);
			return {};
		}

		case Variant::VECTOR4:
		case Variant::VECTOR4I: {
			const bool integer = type == Variant::VECTOR4I;
			if (member == "x") return scalar(0, integer);
			if (member == "y") return scalar(1, integer);
			if (member == "z") return scalar(2, integer);
			if (member == "w") return scalar(3, integer);
			return {};
		}

		case Variant::COLOR:
			if (member == "r") return scalar(0, false);
			if (member == "g") return scalar(1, false);
			if (member == "b") return scalar(2, false);
			if (member == "a") return scalar(3, false);
			return {};

		case Variant::RECT2:
		case Variant::RECT2I: {
			const bool integer = type == Variant::RECT2I;
			if (member == "position") return pair(0, integer);
			if (member == "size") return pair(2, integer);
			return {};
		}

		case Variant::PLANE:
			if (member == "normal") return BuiltinMember{ 0, 3, Variant::VECTOR3, false };
			if (member == "x") return scalar(0, false);
			if (member == "y") return scalar(1, false);
			if (member == "z") return scalar(2, false);
			if (member == "d") return scalar(3, false);
			return {};

		default:
			return {};
	}
}

inline const uint32_t* builtin_member_candidates(size_t& count) {
	static const uint32_t types[] = {
		Variant::VECTOR2, Variant::VECTOR2I,
		Variant::VECTOR3, Variant::VECTOR3I,
		Variant::VECTOR4, Variant::VECTOR4I,
		Variant::COLOR,
		Variant::RECT2, Variant::RECT2I,
		Variant::PLANE,
	};
	count = sizeof(types) / sizeof(types[0]);
	return types;
}

} // namespace gdscript
