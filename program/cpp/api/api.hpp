#pragma once
#include <cstddef>
#include "node2d.hpp"
#include "node3d.hpp"
#include "syscalls_fwd.hpp"

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

/// @brief Stop execution of the program.
/// @note This function may return if the program is resumed. However, no such
/// functionality is currently implemented.
inline void halt() {
	fast_exit();
}

#include <unordered_map>
/// @brief A macro to define a static function that returns a custom state object
/// tied to a Node object. For shared sandbox instances, this is the simplest way
/// to store per-node-instance state.
/// @param State The type of the state object.
/// @note There is currently no way to clear the state objects, so be careful
/// with memory usage.
#define PER_OBJECT(State) \
	static State &Get ## State(Node &node) { \
		static std::unordered_map<uint64_t, State> state; \
		return state[node.address()]; \
	}
