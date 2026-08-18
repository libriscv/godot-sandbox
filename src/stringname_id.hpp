#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

// -= StringName as a cheap map key =-
//
// A StringName is a pointer into Godot's interning table, and the engine itself compares
// two of them by that pointer alone. godot-cpp hides it behind opaque bytes, so every
// operation it offers -- operator==, hash(), the String conversion -- is an out-of-line
// call into the engine, and hash() re-walks the characters every time. On a path taken by
// every single guest call that is pure overhead, so read the pointer directly instead.

/// @brief The interned data pointer behind a StringName, which uniquely identifies it.
static inline const void *stringname_id(const godot::StringName &name) noexcept {
	static_assert(sizeof(godot::StringName) == sizeof(void *),
			"StringName must be a single interned pointer for identity comparison to hold");
	const void *id;
	std::memcpy(&id, name._native_ptr(), sizeof(id));
	return id;
}

/// @brief The shared character buffer behind a String, which identifies it up to sharing.
/// @note Two Strings with equal content are only equal by this measure when they are copies
/// of one another, so it identifies a string but cannot answer whether two differ.
static inline const void *string_id(const godot::String &str) noexcept {
	static_assert(sizeof(godot::String) == sizeof(void *),
			"String must be a single buffer pointer for identity comparison to hold");
	const void *id;
	std::memcpy(&id, str._native_ptr(), sizeof(id));
	return id;
}

/// @brief Compare two StringNames the way Godot does internally, without calling into it.
static inline bool stringname_equals(const godot::StringName &a, const godot::StringName &b) noexcept {
	return stringname_id(a) == stringname_id(b);
}

struct StringNameIdHash {
	std::size_t operator()(const godot::StringName &name) const noexcept {
		// Allocation-aligned pointers have zeroes in the low bits, so mix before the
		// container folds the value down to a bucket index.
		const std::uintptr_t id = reinterpret_cast<std::uintptr_t>(stringname_id(name));
		return std::size_t(id * 0x9E3779B97F4A7C15ull >> 16);
	}
};
struct StringNameIdEqual {
	bool operator()(const godot::StringName &a, const godot::StringName &b) const noexcept {
		return stringname_equals(a, b);
	}
};

// Keyed by identity, but storing the StringNames themselves: an entry keeps its own name
// alive, so the interning table can never recycle a cached id behind another name's back.
template <typename Value>
using StringNameMap = std::unordered_map<godot::StringName, Value, StringNameIdHash, StringNameIdEqual>;
using StringNameSet = std::unordered_set<godot::StringName, StringNameIdHash, StringNameIdEqual>;
