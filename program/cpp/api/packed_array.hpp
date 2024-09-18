#pragma once
#include <cstdint>
#include <vector>

/**
 * @brief A reference to a host-side Packed Array.
 * Supported:
 * - PackedByteArray
 * - PackedInt32Array
 * - PackedInt64Array
 * - PackedFloat32Array
 * - PackedFloat64Array
 * 
 * @tparam T uint8_t, int32_t, int64_t, float, or double.
 */
template <typename T>
struct PackedArray {
	constexpr PackedArray() {}

	/// @brief Create a PackedArray from a vector of data.
	/// @param data The initial data.
	PackedArray(const std::vector<T> &data);

	/// @brief Retrieve the host-side array data.
	/// @return std::vector<T> The host-side array data.
	std::vector<T> fetch() const;

	/// @brief Store a vector of data into the host-side array.
	/// @param data The data to store.
	void store(const std::vector<T> &data);

	/// @brief Store an array of data into the host-side array.
	/// @param data The data to store.
	void store(const T *data, size_t size);

	/// @brief Create a PackedArray from a host-side Variant index.
	/// @param idx The host-side Variant index.
	/// @return PackedArray<T> The PackedArray.
	static PackedArray<T> from_index(unsigned idx) { PackedArray<T> pa{}; pa.m_idx = idx; return pa; }
	unsigned get_variant_index() const noexcept { return m_idx; }

private:
	unsigned m_idx = -1; // Host-side Variant index.

	static_assert(std::is_same_v<T, uint8_t> || std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t> || std::is_same_v<T, float> || std::is_same_v<T, double>, "PackedArray type must be uint8_t, int32_t, int64_t, float, or double.");
};