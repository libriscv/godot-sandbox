#include "api.hpp"

PUBLIC Variant test_shm(float* array, size_t size) {
	// This function is a placeholder for shared memory operations.
	// It assumes that the array is already allocated in shared memory.
	if (array == nullptr || size == 0) {
		return Nil;
	}

	for (size_t i = 0; i < size; ++i) {
		array[i] *= 2.0f; // Example operation: double each element
	}

	return PackedArray<float>(array, size);
}
