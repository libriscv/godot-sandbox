#pragma once
#include "syscall_numbers.h"
#include "variant_types.h"
#include <string>

namespace gdscript {

enum class MethodLowering {
	NONE,
	ARRAY_SIZE,
	STRING_SIZE,
	DICT_OP,
};

struct BuiltinMethod {
	MethodLowering lowering = MethodLowering::NONE;
	int op = -1;
	uint32_t result_type = Variant::NIL;
	bool empty_test = false;
	bool has_result = true;

	bool valid() const { return lowering != MethodLowering::NONE; }
};

inline BuiltinMethod find_builtin_method(uint32_t recv_type, const std::string& name, size_t argc) {
	const auto dict = [](Dictionary_Op op, uint32_t result, bool has_result = true) {
		return BuiltinMethod{ MethodLowering::DICT_OP, dictionary_op(op), result, false, has_result };
	};

	switch (recv_type) {
		case Variant::ARRAY:
			if (argc == 0 && name == "size")
				return { MethodLowering::ARRAY_SIZE, -1, Variant::INT, false, true };
			if (argc == 0 && name == "is_empty")
				return { MethodLowering::ARRAY_SIZE, -1, Variant::BOOL, true, true };
			return {};

		case Variant::DICTIONARY:
			if (argc == 0 && name == "size")
				return dict(Dictionary_Op::GET_SIZE, Variant::INT);
			if (argc == 0 && name == "is_empty") {
				BuiltinMethod m = dict(Dictionary_Op::GET_SIZE, Variant::BOOL);
				m.empty_test = true;
				return m;
			}
			if (argc == 1 && name == "has")
				return dict(Dictionary_Op::HAS, Variant::BOOL);
			if (argc == 1 && name == "get")
				return dict(Dictionary_Op::GET, Variant::NIL);
			if (argc == 0 && name == "keys")
				return dict(Dictionary_Op::GET_KEYS, Variant::ARRAY);
			if (argc == 0 && name == "values")
				return dict(Dictionary_Op::GET_VALUES, Variant::ARRAY);
			if (argc == 0 && name == "clear")
				return dict(Dictionary_Op::CLEAR, Variant::NIL, false);
			return {};

		case Variant::STRING:
			if (argc == 0 && name == "length")
				return { MethodLowering::STRING_SIZE, -1, Variant::INT, false, true };
			if (argc == 0 && name == "is_empty")
				return { MethodLowering::STRING_SIZE, -1, Variant::BOOL, true, true };
			return {};

		default:
			return {};
	}
}

} // namespace gdscript
