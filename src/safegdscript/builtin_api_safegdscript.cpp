#include "builtin_api_safegdscript.h"

#include <godot_cpp/variant/packed_string_array.hpp>

namespace safegd_builtin {
namespace {
#include "builtin_api.gen.inc"
} // namespace

const BuiltinClassInfo *find_builtin_class(const String &p_type) {
	if (p_type.is_empty()) {
		return nullptr;
	}
	for (const BuiltinClassInfo *entry = builtin_classes; entry->name != nullptr; entry++) {
		if (p_type == entry->name) {
			return entry;
		}
	}
	return nullptr;
}

PackedStringArray split_arguments(const char *p_arguments) {
	PackedStringArray arguments;
	if (p_arguments == nullptr || *p_arguments == '\0') {
		return arguments;
	}
	for (const String &argument : String::utf8(p_arguments).split(String::chr(0x1f))) {
		arguments.push_back(argument);
	}
	return arguments;
}

} // namespace safegd_builtin
