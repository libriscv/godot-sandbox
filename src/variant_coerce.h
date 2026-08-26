#pragma once

#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

// GDExtension scripts receive Variants unnarrowed; GDScriptFunction::call
// handles it for .gd but ScriptExtension never reaches that path.
// Narrow at the boundary: can_convert_strict, then type_convert.
[[nodiscard]] inline bool coerce_variant_to(godot::Variant &value, godot::Variant::Type type) {
	if (type == godot::Variant::NIL) {
		return true;
	}
	const godot::Variant::Type from = value.get_type();
	if (from == type) {
		return true;
	}
	if (!godot::Variant::can_convert_strict(from, type)) {
		return false;
	}
	value = godot::UtilityFunctions::type_convert(value, type);
	return true;
}
