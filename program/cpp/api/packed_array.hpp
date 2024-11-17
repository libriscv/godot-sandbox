#pragma once
#include <cstdint>
#include <vector>
#include "color.hpp"
#include "vector.hpp"
struct Variant;

/**
 * @brief A reference to a host-side Packed Array.
 * Supported:
 * - PackedByteArray
 * - PackedInt32Array
 * - PackedInt64Array
 * - PackedFloat32Array
 * - PackedFloat64Array
 * - PackedVector2Array
 * - PackedVector3Array
 * - PackedVector4Array
 * - PackedColorArray
 * - PackedStringArray
 * 
 * @tparam T uint8_t, int32_t, int64_t, float, double, Vector2, Vector3, Vector4, Color or std::string.
 */
template <typename T>
struct PackedArray {
	constexpr PackedArray() {}

	/// @brief Create a PackedArray from a Variant.
	/// @param v The Variant.
	PackedArray(const Variant& v);

	/// @brief Create a PackedArray from a vector of data.
	/// @param data The initial data.
	PackedArray(const std::vector<T> &data);

	/// @brief Create a PackedArray from an array of data.
	/// @param data The initial data.
	/// @param size The size of the data in elements.
	PackedArray(const T *data, size_t size);

	/// @brief Retrieve the host-side array data.
	/// @return std::vector<T> The host-side array data.
	std::vector<T> fetch() const;

	/// @brief Store a vector of data into the host-side array.
	/// @param data The data to store.
	void store(const std::vector<T> &data);

	/// @brief Store an array of data into the host-side array.
	/// @param data The data to store.
	void store(const T *data, size_t size);

	/// @brief Call a method on the packed array.
	/// @tparam Args The method arguments.
	template <typename... Args>
	Variant operator () (std::string_view method, Args&&... args);

	/// @brief Cast the PackedArray to a Variant.
	/// @return Variant The Variant.
	operator Variant() const;

	/// @brief Create a PackedArray from a host-side Variant index.
	/// @param idx The host-side Variant index.
	/// @return PackedArray<T> The PackedArray.
	static PackedArray<T> from_index(unsigned idx) { PackedArray<T> pa{}; pa.m_idx = idx; return pa; }
	unsigned get_variant_index() const noexcept { return m_idx; }

private:
	unsigned m_idx = INT32_MIN; // Host-side Variant index.

	static_assert(std::is_same_v<T, uint8_t> || std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t> || std::is_same_v<T, float> || std::is_same_v<T, double>
		|| std::is_same_v<T, Vector2> || std::is_same_v<T, Vector3> || std::is_same_v<T, Vector4> || std::is_same_v<T, Color> || std::is_same_v<T, std::string>,
		"PackedArray type must be uint8_t, int32_t, int64_t, float, double, Vector2, Vector3 or Color.");
};
