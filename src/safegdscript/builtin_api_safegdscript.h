#pragma once

#include <godot_cpp/variant/string.hpp>

using namespace godot;

// Variant built-ins not in ClassDB (from generate_builtin_api.py).
namespace safegd_builtin {

struct BuiltinMember {
	const char *name;
	const char *type;
};

struct BuiltinMethod {
	const char *name;
	const char *return_type;
	// Arguments as "name: Type = default", separated by U+001F.
	const char *arguments;
	bool is_static;
};

struct BuiltinClassInfo {
	const char *name;
	const BuiltinMember *members;
	const BuiltinMember *constants;
	const BuiltinMethod *methods;
	const char *const *constructors;
};

const BuiltinClassInfo *find_builtin_class(const String &p_type);
PackedStringArray split_arguments(const char *p_arguments);

} // namespace safegd_builtin
