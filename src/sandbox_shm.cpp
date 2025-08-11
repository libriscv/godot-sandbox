#include "sandbox.h"
static constexpr bool VERBOSE_SHM = false;

gaddr_t Sandbox::share_array_internal(void* data, size_t bytes, bool allow_write)
{
	if (this->is_in_vmcall()) {
		ERR_PRINT("Cannot share array while a VM call is in progress.");
		return 0;
	}
#ifdef RISCV_LIBTCC
		if (this->m_bintr_automatic_nbit_as) {
			ERR_PRINT("Cannot share array while the program is in automatic N-bit mode. Virtual memory is disabled.");
			return 0;
		}
#endif

	const gaddr_t vaddr = this->m_shared_memory_base;
	const size_t  vsize = (bytes + 0xFFFLL) & ~0xFFFLL; // Align to 4KB
	// The address space is practically endless, so we can just keep allocating
	this->m_shared_memory_base += vsize;

	// Figure out the page-sized portion of the data
	const size_t valignsize = bytes & ~0xFFFLL; // Align to 4KB

	try {
		// If the data is larger than a page, we can directly insert it as non-owned memory
		if (valignsize > 0) {
			if constexpr (VERBOSE_SHM) {
				printf("Inserting %zu bytes of data into shared memory at address 0x%lx\n", valignsize, vaddr);
			}
			machine().memory.insert_non_owned_memory(
				vaddr, data, valignsize, riscv::PageAttributes{
					.read = true,
					.write = allow_write,
					.exec = false,
					.is_cow = false,
				});
		}
		// The remaining bytes must be copied into the end of shared memory
		const size_t remaining = bytes - valignsize;
		if (remaining > 0) {
			if constexpr (VERBOSE_SHM) {
				printf("Copying remaining %zu bytes of data into shared memory at address 0x%lx\n", remaining, vaddr + valignsize);
			}
			machine().memory.memcpy(vaddr + valignsize, static_cast<const uint8_t *>(data) + valignsize, remaining);
			machine().memory.set_page_attr(
				vaddr + valignsize, riscv::Page::size(), riscv::PageAttributes{
					.read = true,
					.write = allow_write,
					.exec = false,
				}); // Set the attributes for the remaining bytes
			// And the remaining bytes internal to the page are already zeroed (or guest-owned).
		}

		// Add the new range to the shared memory ranges (we need the real bytes)
		this->m_shared_memory_ranges.emplace_back(vaddr, bytes, data);
		return vaddr;

	} catch (const std::exception &e) {
		ERR_PRINT(String("Failed to share array: ") + e.what());

		// If we failed to share the array, we need to reset the base address
		this->m_shared_memory_base -= vsize;
		return 0;
	}
}

bool Sandbox::unshare_array(gaddr_t address) {
	if (this->is_in_vmcall()) {
		ERR_PRINT("Cannot unshare array while a VM call is in progress.");
		return false;
	}

	auto it = std::find_if(this->m_shared_memory_ranges.begin(), this->m_shared_memory_ranges.end(),
		[address](const auto &range) { return range.contains(address); });

	if (it == this->m_shared_memory_ranges.end()) {
		ERR_PRINT("Address is not in a shared memory range.");
		return false;
	}

	// Copy back the remaining bytes (overflow on the last page) if any
	const size_t remaining = it->size & (riscv::Page::size() - 1);
	if (remaining > 0) {
		if constexpr (VERBOSE_SHM) {
			printf("Copying remaining %zu bytes from shared memory at address 0x%lx\n", remaining, it->start + it->size - remaining);
		}
		// Get the base pointer to the shared memory range by getting the page data at start
		uint8_t *base_ptr = (uint8_t *)it->base_ptr;

		const gaddr_t offset = it->size - remaining;
		machine().copy_from_guest(
			base_ptr + offset, it->start + offset, remaining);
	}

	// Remove the range from the shared memory ranges
	if constexpr (VERBOSE_SHM) {
		printf("Freeing pages from shared memory range: start=0x%lx, size=0x%lx\n", it->start, it->size);
	}
	// Align up the size to page size
	const size_t aligned_size = (it->size + riscv::Page::size() - 1) & ~(riscv::Page::size() - 1);
	// Free the pages in the range
	machine().memory.free_pages(it->start, aligned_size);

	this->m_shared_memory_ranges.erase(it);
	return true;
}


gaddr_t Sandbox::share_byte_array(bool allow_write, const PackedByteArray &array) {
	return this->share_array_internal((void*)array.ptr(), array.size(), allow_write);
}
gaddr_t Sandbox::share_float32_array(bool allow_write, const PackedFloat32Array &array) {
	return this->share_array_internal((void*)array.ptr(), array.size() * sizeof(float), allow_write);
}
gaddr_t Sandbox::share_float64_array(bool allow_write, const PackedFloat64Array &array) {
	return this->share_array_internal((void*)array.ptr(), array.size() * sizeof(double), allow_write);
}
gaddr_t Sandbox::share_int32_array(bool allow_write, const PackedInt32Array &array) {
	return this->share_array_internal((void*)array.ptr(), array.size() * sizeof(int32_t), allow_write);
}
gaddr_t Sandbox::share_int64_array(bool allow_write, const PackedInt64Array &array) {
	return this->share_array_internal((void*)array.ptr(), array.size() * sizeof(int64_t), allow_write);
}
gaddr_t Sandbox::share_vec2_array(bool allow_write, const PackedVector2Array &array) {
	return this->share_array_internal((void*)array.ptr(), array.size() * sizeof(Vector2), allow_write);
}
gaddr_t Sandbox::share_vec3_array(bool allow_write, const PackedVector3Array &array) {
	return this->share_array_internal((void*)array.ptr(), array.size() * sizeof(Vector3), allow_write);
}
gaddr_t Sandbox::share_vec4_array(bool allow_write, const PackedVector4Array &array) {
	return this->share_array_internal((void*)array.ptr(), array.size() * sizeof(Vector4), allow_write);
}
