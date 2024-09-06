#pragma once
#include <cstddef>
#include "node2d.hpp"
#include "node3d.hpp"
#include "array.hpp"
#include "dictionary.hpp"
#include "string.hpp"
#include "syscalls_fwd.hpp"
#include "timer.hpp"

template <typename T>
using remove_cvref = std::remove_cv_t<std::remove_reference_t<T>>;

/// @brief Print a message to the console.
/// @param ...vars A list of Variant objects to print.
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

struct Engine {
	/// @brief Check if the program is running in the Godot editor.
	/// @return True if running in the editor, false otherwise.
	static bool is_editor_hint() {
		return is_editor();
	}
};

/// @brief Check if the given Node is a part of the current scene tree. Not an instance of another scene.
/// @param node The Node to check.
/// @return True if the Node is a part of the current scene tree, false otherwise.
inline bool is_part_of_tree(Node node) {
	return get_tree()("get_edited_scene_root") == node("get_owner");
}
