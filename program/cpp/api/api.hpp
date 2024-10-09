#pragma once
#include <cstddef>
#include "node2d.hpp"
#include "node3d.hpp"
#include "array.hpp"
#include "callable.hpp"
#include "dictionary.hpp"
#include "string.hpp"
#include "basis.hpp"
#include "transform2d.hpp"
#include "transform3d.hpp"
#include "syscalls_fwd.hpp"
#include "timer.hpp"

template <typename T>
using remove_cvref = std::remove_cv_t<std::remove_reference_t<T>>;

/// @brief Print a message to the console.
/// @param ...vars A list of Variants to print.
template <typename... Args>
inline void print(Args &&...vars) {
	std::array<Variant, sizeof...(Args)> vptrs;
	int idx = 0;
	([&] {
		if constexpr (std::is_same_v<Variant, remove_cvref<Args>>)
			vptrs[idx++] = vars;
		else
			vptrs[idx++] = Variant(vars);
	}(),
			...);
	sys_print(vptrs.data(), vptrs.size());
}

/// @brief Get the current scene tree.
/// @return The root node of the scene tree.
inline Object get_tree() {
	return Object("SceneTree");
}

/// @brief Check if the given Node is a part of the current scene tree. Not an instance of another scene.
/// @param node The Node to check.
/// @return True if the Node is a part of the current scene tree, false otherwise.
inline bool is_part_of_tree(Node node) {
	return get_tree()("get_edited_scene_root") == node("get_owner");
}

/// @brief Get a node by its path. By default, this returns the current node.
/// @param path The path to the node.
/// @return The node at the given path.
inline Node get_node(std::string_view path = ".") {
	return Node(path);
}

/// @brief Get the parent of the current node.
/// @return The parent node.
inline Node get_parent() {
	return Node("..");
}

#include <unordered_map>
/// @brief A macro to define a static function that returns a custom state object
/// tied to a Node object. For shared sandbox instances, this is the simplest way
/// to store per-node-instance state.
/// @param State The type of the state object.
/// @note There is currently no way to clear the state objects, so be careful
/// with memory usage.
/// @example
/// struct SlimeState {
/// 	int direction = 1;
/// };
/// PER_OBJECT(SlimeState);
/// // Then use it like this:
/// auto& state = GetSlimeState(slime);
#define PER_OBJECT(State) \
	static State &Get ## State(const Node &node) { \
		static std::unordered_map<uint64_t, State> state; \
		return state[node.address()]; \
	}

/// @brief A property struct that must be instantiated in the global scope.
/// @note This is used to define custom properties for the Sandbox class.
/// On program load, the properties are automatically exposed on the script instance.
/// @example
/// SANDBOXED_PROPERTIES(1, {
/// 	.name = "my_property",
/// 	.type = Variant::Type::INT,
/// 	.getter = []() -> Variant { return 42; },
/// 	.setter = [](Variant value) { print("Set to: ", value); },
/// 	.default_value = Variant{42},
/// });
struct Property {
	using getter_t = Variant (*)();
	using setter_t = Variant (*)(Variant);

	const char * const name = 0;
	const unsigned size = sizeof(Property);
	const Variant::Type type;
	const getter_t getter;
	const setter_t setter;
	const Variant default_value;
};
#define SANDBOXED_PROPERTIES(num, ...) \
	extern "C" const Property properties[num+1] { __VA_ARGS__, {0} };

/// @brief Stop execution of the program.
/// @note This function may return if the program is resumed. However, no such
/// functionality is currently implemented.
inline void halt() {
	fast_exit();
}

/// @brief Check if the program is running in the Godot editor.
/// @return True if running in the editor, false otherwise.
inline bool is_editor() {
	static constexpr int ECALL_IS_EDITOR = 512;
	register int a0 asm("a0");
	register int a7 asm("a7") = ECALL_IS_EDITOR;
	asm volatile("ecall" : "=r"(a0) : "r"(a7));
	return a0;
}
inline bool is_editor_hint() {
	return is_editor(); // Alias
}

/// @brief Load a resource (at run-time) from the given path.
/// @param path The path to the resource.
/// @return The loaded resource.
extern Variant load(std::string_view path);

struct Engine {
	/// @brief Check if the program is running in the Godot editor.
	/// @return True if running in the editor, false otherwise.
	static bool is_editor_hint() {
		return is_editor(); // Fast-path for the hint.
	}

	/// @brief Get the current time scale.
	/// @return The current time scale.
	static double get_time_scale() {
		return get_singleton().call("get_time_scale");
	}

	/// @brief Set a new time scale.
	/// @param scale The new time scale.
	static void set_time_scale(double scale) {
		get_singleton().call("set_time_scale", scale);
	}

	/// @brief Get the singleton instance of the Engine.
	/// @return The Engine singleton.
	static Object get_singleton();
};

struct Input {
	/// @brief Check if an action is currently pressed.
	/// @param action The name of the action.
	/// @return True if the action is pressed, false otherwise.
	static bool is_action_pressed(const std::string &action) {
		return get_singleton().call("is_action_pressed", action);
	}

	/// @brief Check if an action is released.
	/// @param action The name of the action.
	/// @return True if the action is released, false otherwise.
	static bool is_action_released(const std::string &action) {
		return get_singleton().call("is_action_released", action);
	}

	/// @brief Check if an action is just pressed.
	/// @param action The name of the action.
	/// @return True if the action is just pressed, false otherwise.
	static bool is_action_just_pressed(const std::string &action) {
		return get_singleton().call("is_action_just_pressed", action);
	}

	/// @brief Check if an action is just released.
	/// @param action The name of the action.
	/// @return True if the action is just released, false otherwise.
	static bool is_action_just_released(const std::string &action) {
		return get_singleton().call("is_action_just_released", action);
	}

	/// @brief Get the singleton instance of the Input class.
	/// @return The Input singleton.
	static Object get_singleton();
};

struct Time {
	/// @brief Get the current time in milliseconds.
	/// @return The current time in milliseconds.
	static int64_t get_ticks_msec() {
		return get_singleton().call("get_ticks_msec");
	}

	/// @brief Get the current time in microseconds.
	/// @return The current time in microseconds.
	static int64_t get_ticks_usec() {
		return get_singleton().call("get_ticks_usec");
	}

	/// @brief Get the singleton instance of the Time class.
	/// @return The Time singleton.
	static Object get_singleton();
};


/// @brief The class database for instantiating Godot objects.
struct ClassDB {
	/// @brief Instantiate a new object of the given class.
	/// @param class_name The name of the class to instantiate.
	/// @param name The name of the object, if it's a Node. Otherwise, this is ignored.
	/// @return The new object.
	static Object instantiate(std::string_view class_name, std::string_view name = "");
};

/// @brief Math and interpolation operations.
struct Math {
	/// @brief The available 64-bit FP math operations.
	static double sin(double x);
	static double cos(double x);
	static double tan(double x);
	static double asin(double x);
	static double acos(double x);
	static double atan(double x);
	static double atan2(double y, double x);
	static double pow(double x, double y);

	/// @brief The available 32-bit FP math operations.
	static float sinf(float x);
	static float cosf(float x);
	static float tanf(float x);
	static float asinf(float x);
	static float acosf(float x);
	static float atanf(float x);
	static float atan2f(float y, float x);
	static float powf(float x, float y);

	/// @brief Linearly interpolate between two values.
	/// @param a The start value.
	/// @param b The end value.
	/// @param t The interpolation factor (between 0 and 1).
	static double lerp(double a, double b, double t);
	static float lerpf(float a, float b, float t);

	/// @brief Smoothly interpolate between two values.
	/// @param from The start value.
	/// @param to The end value.
	/// @param t The interpolation factor (between 0 and 1).
	static double smoothstep(double from, double to, double t);
	static float smoothstepf(float from, float to, float t);

	/// @brief Clamp a value between two bounds.
	/// @param x The value to clamp.
	/// @param min The minimum value.
	/// @param max The maximum value.
	static double clamp(double x, double min, double max);
	static float clampf(float x, float min, float max);

	/// @brief Spherical linear interpolation between two values.
	/// @param a The start value in radians.
	/// @param b The end value in radians.
	/// @param t The interpolation factor (between 0 and 1).
	static double slerp(double a, double b, double t);
	static float slerpf(float a, float b, float t);
};

/* Embed binary data into executable. This data has no guaranteed alignment. */
#define EMBED_BINARY(name, filename) \
	asm(".pushsection .rodata\n" \
	"	.global " #name "\n" \
	#name ":\n" \
	"	.incbin " #filename "\n" \
	#name "_end:\n" \
	"	.int  0\n" \
	"	.global " #name "_size\n" \
	"	.type   " #name "_size, @object\n" \
	"	.align 4\n" \
	#name "_size:\n" \
	"	.int  " #name "_end - " #name "\n" \
	".popsection"); \
	extern char name[]; \
	extern unsigned name ##_size;

#include "api_inline.hpp"
