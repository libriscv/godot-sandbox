#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

// StringName is an interned pointer; godot-cpp wraps it in opaque bytes whose
// every operator is an engine call. Read the pointer directly on 64-bit builds;
// fall back to godot-cpp operators where the opaque buffer is narrower.

inline constexpr bool stringname_is_pointer = sizeof(godot::StringName) == sizeof(void *);
inline constexpr bool string_is_pointer = sizeof(godot::String) == sizeof(void *);

namespace detail {
	// Min-size memcpy so non-pointer-sized layouts still compile (if constexpr
	// discards the call but the non-template branch is still instantiated).
	template <typename T>
	static inline const void *opaque_pointer(const T &value) noexcept {
		const void *id = nullptr;
		std::memcpy(&id, value._native_ptr(), sizeof(T) < sizeof(id) ? sizeof(T) : sizeof(id));
		return id;
	}
} // namespace detail

// Pointer compare on 64-bit, engine operator== otherwise.
static inline bool stringname_equals(const godot::StringName &a, const godot::StringName &b) noexcept {
	if constexpr (stringname_is_pointer) {
		return detail::opaque_pointer(a) == detail::opaque_pointer(b);
	} else {
		return a == b;
	}
}

// Hash key for the name-address cache. Not equality: distinct copies collide.
static inline std::uintptr_t string_cache_key(const godot::String &str) noexcept {
	if constexpr (string_is_pointer) {
		return reinterpret_cast<std::uintptr_t>(detail::opaque_pointer(str));
	} else {
		return std::uintptr_t(str.hash());
	}
}

// Pointer identity on 64-bit, content compare otherwise.
static inline bool string_cache_hit(const godot::String &cached, const godot::String &str) noexcept {
	if constexpr (string_is_pointer) {
		return detail::opaque_pointer(cached) == detail::opaque_pointer(str);
	} else {
		return cached == str;
	}
}

struct StringNameIdHash {
	std::size_t operator()(const godot::StringName &name) const noexcept {
		if constexpr (stringname_is_pointer) {
			// Low bits are zero from alignment; mix before bucket fold.
			const std::uintptr_t id = reinterpret_cast<std::uintptr_t>(detail::opaque_pointer(name));
			return std::size_t(id * 0x9E3779B97F4A7C15ull >> 16);
		} else {
			return std::size_t(name.hash());
		}
	}
};
struct StringNameIdEqual {
	bool operator()(const godot::StringName &a, const godot::StringName &b) const noexcept {
		return stringname_equals(a, b);
	}
};

// Entries keep the StringName alive, preventing interning-table recycling.
template <typename Value>
using StringNameMap = std::unordered_map<godot::StringName, Value, StringNameIdHash, StringNameIdEqual>;
using StringNameSet = std::unordered_set<godot::StringName, StringNameIdHash, StringNameIdEqual>;
