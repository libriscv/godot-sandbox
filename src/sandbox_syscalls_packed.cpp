// ECALL_PACKED_ACQUIRE and ECALL_PACKED_RELEASE (syscalls.h), after JNI's
// Get<Type>ArrayElements / Release<Type>ArrayElements. handle_exception() and the
// end of a call store and free the copies a region left behind.
#include "guest_datatypes.h"
#include "syscalls.h"

#include <godot_cpp/variant/variant.hpp>
#include <cstring>
#include <optional>
#include <random>
#include <string_view>
#include <tuple>
#include "syscalls_helpers.hpp"

// As in sandbox_syscalls.cpp: charge the instruction budget unless profiling.
#define PENALIZE(x) \
	if (!emu.get_profiling()) { \
		machine.penalize(x); \
	}

namespace riscv {

namespace {

static constexpr int64_t MAX_PACKED_ELEMENTS = 16'777'216;
// Copying an element costs a small fraction of one host call: copy the array
// when at least one access per this many elements is expected.
static constexpr int64_t ELEMENTS_PER_ACCESS = 64;

// Guest ABI. The identity is a keyed hash of the shared storage: no host address leaks.
struct PackedDescriptor {
	uint64_t data;
	uint64_t size;
	uint64_t identity;
	uint64_t dirty;
};
static_assert(sizeof(PackedDescriptor) == 32, "PackedDescriptor is guest ABI");

uint64_t packed_identity(const void *storage) {
	static const uint64_t key = [] {
		std::random_device rd;
		return (uint64_t(rd()) << 32) ^ uint64_t(rd()) ^ 0x9E3779B97F4A7C15ull;
	}();
	uint64_t x = uint64_t(uintptr_t(storage)) ^ key; // splitmix64 finalizer
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBull;
	x ^= x >> 31;
	return x;
}

// The engine's own Packed*Array inside a Variant, shared by every copy of that
// Variant, or null when the Variant is not of `type`.
void *packed_storage(const Variant &var, int type) {
	// Only packed arrays have an internal getter; NIL's is null.
	if (type < Variant::PACKED_BYTE_ARRAY || type > Variant::PACKED_VECTOR4_ARRAY || int(variant_type(var)) != type) {
		return nullptr;
	}
	const GDExtensionVariantGetInternalPtrFunc getter =
			internal::gdextension_interface_variant_get_ptr_internal_getter(GDExtensionVariantType(type));
	return getter((GDExtensionVariantPtr)var._native_ptr());
}

template <typename Packed, typename Element>
int64_t acquire_typed(machine_t &machine, void *storage, gaddr_t desc, int64_t expected, PackedDescriptor &out) {
	const Packed &array = *reinterpret_cast<const Packed *>(storage);
	const int64_t size = array.size();
	// size > expected * ELEMENTS_PER_ACCESS, without overflowing on a guest's expected.
	if (expected >= 0 && expected < (size + ELEMENTS_PER_ACCESS - 1) / ELEMENTS_PER_ACCESS) {
		return 1;
	}
	if (size > MAX_PACKED_ELEMENTS || !machine.has_arena()) {
		return 3;
	}
	// Before anything is allocated, so a descriptor the guest cannot write leaks no copy.
	machine.memory.writable_memview(desc, sizeof(PackedDescriptor), sizeof(PackedDescriptor));
	const size_t bytes = size_t(size) * sizeof(Element);
	gaddr_t data = 0;
	if (bytes > 0) {
		data = machine.arena().malloc(bytes);
		if (data == 0) {
			return 3;
		}
		machine.memory.memcpy(data, array.ptr(), bytes);
	}
	out = PackedDescriptor{ data, uint64_t(size), packed_identity(storage), 0 };
	machine.memory.memcpy(desc, &out, sizeof(out));
	return 0;
}

template <typename Packed, typename Element>
void release_typed(machine_t &machine, void *storage, const PackedDescriptor &descriptor) {
	if (descriptor.size > uint64_t(MAX_PACKED_ELEMENTS)) {
		throw std::runtime_error("PackedArray release: too many elements");
	}
	const size_t bytes = size_t(descriptor.size) * sizeof(Element);
	// Checked before the array is touched: a copy the guest cannot supply leaves it as it was.
	const std::string_view source = machine.memory.memview(descriptor.data, bytes, bytes);
	Packed &array = *reinterpret_cast<Packed *>(storage);
	if (uint64_t(array.size()) != descriptor.size) {
		array.resize(int64_t(descriptor.size));
		if (uint64_t(array.size()) != descriptor.size) {
			throw std::runtime_error("PackedArray release: the array could not be resized");
		}
	}
	if (bytes > 0) {
		// ptrw() on the shared storage: the engine's copy-on-write applies exactly
		// as it does for a single element set.
		std::memcpy(array.ptrw(), source.data(), bytes);
	}
}

#define PACKED_TYPES(X)                                            \
	X(Variant::PACKED_BYTE_ARRAY, PackedByteArray, uint8_t)        \
	X(Variant::PACKED_INT32_ARRAY, PackedInt32Array, int32_t)      \
	X(Variant::PACKED_INT64_ARRAY, PackedInt64Array, int64_t)      \
	X(Variant::PACKED_FLOAT32_ARRAY, PackedFloat32Array, float)    \
	X(Variant::PACKED_FLOAT64_ARRAY, PackedFloat64Array, double)   \
	X(Variant::PACKED_VECTOR2_ARRAY, PackedVector2Array, Vector2)  \
	X(Variant::PACKED_VECTOR3_ARRAY, PackedVector3Array, Vector3)  \
	X(Variant::PACKED_VECTOR4_ARRAY, PackedVector4Array, Vector4)  \
	X(Variant::PACKED_COLOR_ARRAY, PackedColorArray, Color)

void free_copy(machine_t &machine, gaddr_t copy) {
	if (copy != 0 && machine.has_arena()) {
		machine.arena().free(copy);
	}
}

// The Packed*Array of `type` a scoped index holds now, or null.
void *packed_storage_at(const Sandbox &emu, int32_t index, int type) {
	const std::optional<const Variant *> var = emu.get_scoped_variant(index);
	return var.has_value() && var.value() != nullptr ? packed_storage(*var.value(), type) : nullptr;
}

// Write a dirty copy into `storage`, the array of `type` it was taken from.
void store_typed(machine_t &machine, void *storage, int type, const PackedDescriptor &descriptor) {
	switch (type) {
#define RELEASE_CASE(m_type, m_packed, m_element)                     \
	case m_type:                                                      \
		release_typed<m_packed, m_element>(machine, storage, descriptor); \
		return;
		PACKED_TYPES(RELEASE_CASE)
#undef RELEASE_CASE
		default:
			throw std::runtime_error("PackedArray release: unsupported Packed*Array type");
	}
}

} // namespace

APICALL(api_packed_acquire) {
	const std::tuple<const GuestVariant *, int, gaddr_t, int64_t> args =
			machine.sysargs<const GuestVariant *, int, gaddr_t, int64_t>();
	const GuestVariant *g_subject = std::get<0>(args);
	const int flagged_type = std::get<1>(args);
	const gaddr_t desc = std::get<2>(args);
	const int64_t expected = std::get<3>(args);
	const bool written = (flagged_type & PACKED_WRITTEN) != 0;
	const int type = flagged_type & ~PACKED_WRITTEN;
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("packed_acquire", g_subject, type, desc, expected);

	if (int(g_subject->type) != type) {
		machine.set_result(2);
		return;
	}
	const Variant *var = g_subject->toVariantPtr(emu);
	void *storage = var != nullptr ? packed_storage(*var, type) : nullptr;
	if (storage == nullptr) {
		machine.set_result(2);
		return;
	}
	PackedDescriptor descriptor{};
	int64_t status = 2;
	switch (type) {
#define ACQUIRE_CASE(m_type, m_packed, m_element)                                                \
	case m_type:                                                                                 \
		status = acquire_typed<m_packed, m_element>(machine, storage, desc, expected, descriptor); \
		break;
		PACKED_TYPES(ACQUIRE_CASE)
#undef ACQUIRE_CASE
		default:
			break;
	}
	if (status == 0) {
		emu.packed_acquired(Sandbox::PackedAcquisition{ &emu.state(), int32_t(g_subject->v.i), type,
				descriptor.identity, desc, gaddr_t(descriptor.data), descriptor.size, written });
	}
	machine.set_result(status);
}

APICALL(api_packed_release) {
	const std::tuple<const GuestVariant *, int, gaddr_t> args = machine.sysargs<const GuestVariant *, int, gaddr_t>();
	const GuestVariant *g_subject = std::get<0>(args);
	const int flagged_type = std::get<1>(args);
	const gaddr_t desc = std::get<2>(args);
	const bool written = (flagged_type & PACKED_WRITTEN) != 0;
	const int type = flagged_type & ~PACKED_WRITTEN;
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("packed_release", g_subject, type, desc);

	// Forgotten first: if the store below throws, handle_exception() must not try
	// it a second time. Only the copy acquired here is freed, never an address the guest names.
	const gaddr_t copy = emu.packed_released(desc);
	PackedDescriptor descriptor;
	machine.memory.memcpy_out(&descriptor, desc, sizeof(descriptor));
	if (written || descriptor.dirty != 0) {
		try {
			const Variant *var = int(g_subject->type) == type ? g_subject->toVariantPtr(emu) : nullptr;
			void *storage = var != nullptr ? packed_storage(*var, type) : nullptr;
			if (storage == nullptr) {
				throw std::runtime_error("PackedArray release: the Variant is no longer that Packed*Array");
			}
			store_typed(machine, storage, type, descriptor);
		} catch (...) {
			free_copy(machine, copy);
			throw;
		}
	}
	free_copy(machine, copy);
	const PackedDescriptor cleared{ 0, 0, descriptor.identity, 0 };
	machine.memory.memcpy(desc, &cleared, sizeof(cleared));
	machine.set_result(0);
}

} // namespace riscv

void Sandbox::packed_acquired(const PackedAcquisition &acquisition) {
	// A guest that never released this descriptor: the older copy is freed, not leaked.
	for (std::vector<PackedAcquisition>::iterator it = m_packed_acquisitions.begin(); it != m_packed_acquisitions.end(); ++it) {
		if (it->descriptor == acquisition.descriptor) {
			if (it->data != 0 && it->data != acquisition.data && machine().has_arena()) {
				machine().arena().free(it->data);
			}
			m_packed_acquisitions.erase(it);
			break;
		}
	}
	m_packed_acquisitions.push_back(acquisition);
}

gaddr_t Sandbox::packed_released(gaddr_t descriptor) {
	for (std::vector<PackedAcquisition>::iterator it = m_packed_acquisitions.begin(); it != m_packed_acquisitions.end(); ++it) {
		if (it->descriptor == descriptor) {
			const gaddr_t data = it->data;
			m_packed_acquisitions.erase(it);
			return data;
		}
	}
	return 0;
}

void Sandbox::commit_packed_acquisitions() noexcept {
	if (m_packed_acquisitions.empty()) {
		return;
	}
	try {
		// This call level and deeper: the regions of calls still running stay.
		std::vector<PackedAcquisition> unwound;
		for (std::vector<PackedAcquisition>::iterator it = m_packed_acquisitions.begin(); it != m_packed_acquisitions.end();) {
			if (it->level >= m_current_state) {
				unwound.push_back(*it);
				it = m_packed_acquisitions.erase(it);
			} else {
				++it;
			}
		}
		for (std::vector<PackedAcquisition>::reverse_iterator it = unwound.rbegin(); it != unwound.rend(); ++it) {
			try {
				riscv::PackedDescriptor descriptor;
				machine().memory.memcpy_out(&descriptor, it->descriptor, sizeof(descriptor));
				// The descriptor still describes this copy unless the guest overwrote it.
				if (descriptor.data == it->data && descriptor.size == it->size &&
						(it->written || descriptor.dirty != 0)) {
					// Resolved again: the guest may have replaced or freed that Variant since.
					void *storage = it->level == m_current_state ? riscv::packed_storage_at(*this, it->index, it->type) : nullptr;
					if (storage == nullptr || riscv::packed_identity(storage) != it->identity) {
						ERR_PRINT("PackedArray: a region's array was replaced before the region ended; its writes are dropped");
					} else {
						riscv::store_typed(machine(), storage, it->type, descriptor);
					}
				}
			} catch (const std::exception &e) {
				ERR_PRINT(("PackedArray: could not store a region unwound by an exception: " + std::string(e.what())).c_str());
			}
			try {
				riscv::free_copy(machine(), it->data);
			} catch (const std::exception &) {
			}
		}
	} catch (...) {
		ERR_PRINT("PackedArray: could not end a region's copies");
	}
}

#undef PACKED_TYPES

void Sandbox::initialize_syscalls_packed() {
	using namespace riscv;
	machine_t::install_syscall_handlers({
			{ ECALL_PACKED_ACQUIRE, api_packed_acquire },
			{ ECALL_PACKED_RELEASE, api_packed_release },
	});
}
