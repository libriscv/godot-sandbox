#pragma once

#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

struct Docker {
	using Array = godot::Array;
	using PackedStringArray = godot::PackedStringArray;
	using String = godot::String;

	static Array ContainerStart(String container_name, String image_name);
	static Array ContainerStop(String container_name);
	static bool ContainerExecute(String container_name, const PackedStringArray &args, Array &output);
};
