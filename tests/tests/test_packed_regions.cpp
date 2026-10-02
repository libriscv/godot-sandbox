#include "api.hpp"
#include <cstdint>
#include <cstdlib>
#include <vector>

// Packed array regions driven by hand, the way a hostile guest could, against the host's
// ECALL_PACKED_ACQUIRE and ECALL_PACKED_RELEASE. tests/test_packed_regions.gd checks the host.
MAKE_SYSCALL(ECALL_PACKED_ACQUIRE, long, test_packed_acquire, const Variant *, int, void *, long);
MAKE_SYSCALL(ECALL_PACKED_RELEASE, long, test_packed_release, const Variant *, int, void *);
MAKE_SYSCALL(ECALL_VASSIGN, unsigned, test_packed_vassign, unsigned, unsigned);

namespace {
struct PackedDescriptor {
	uint64_t data;
	uint64_t size;
	uint64_t identity;
	uint64_t dirty;
};
PackedDescriptor descriptor;
constexpr int F32 = int(Variant::PACKED_FLOAT32_ARRAY);

void fill(float value) {
	float *copy = reinterpret_cast<float *>(descriptor.data);
	for (uint64_t i = 0; i < descriptor.size; i++) {
		copy[i] = value;
	}
}
} // namespace

PUBLIC Variant packed_region_replaced_then_fault(Array holder) {
	Variant subject = holder.at(0);
	descriptor = {};
	if (test_packed_acquire(&subject, F32 | PACKED_WRITTEN, &descriptor, -1) != 0) {
		return "acquire refused";
	}
	fill(99.0f);
	Variant replacement(PackedArray<float>(std::vector<float>{ 1.0f, 2.0f, 3.0f }));
	test_packed_vassign(subject.get_internal_index(), replacement.get_internal_index());
	__builtin_trap();
	return "not reached";
}

PUBLIC Variant packed_region_freed_then_fault() {
	Variant subject(PackedArray<float>(std::vector<float>{ 1.0f, 2.0f, 3.0f, 4.0f }));
	descriptor = {};
	if (test_packed_acquire(&subject, F32 | PACKED_WRITTEN, &descriptor, -1) != 0) {
		return "acquire refused";
	}
	fill(7.0f);
	Variant replacement(PackedArray<float>(std::vector<float>{ 5.0f }));
	test_packed_vassign(subject.get_internal_index(), replacement.get_internal_index());
	__builtin_trap();
	return "not reached";
}

PUBLIC Variant packed_region_left_unreleased(Array holder) {
	Variant subject = holder.at(0);
	descriptor = {};
	if (test_packed_acquire(&subject, F32 | PACKED_WRITTEN, &descriptor, -1) != 0) {
		return "acquire refused";
	}
	fill(42.0f);
	return "returned without a release";
}

PUBLIC Variant packed_release_past_guest_memory(Array holder) {
	Variant subject = holder.at(0);
	descriptor = {};
	if (test_packed_acquire(&subject, F32, &descriptor, -1) != 0) {
		return "acquire refused";
	}
	descriptor.data = 0x10;
	descriptor.size += 1000;
	descriptor.dirty = 1;
	test_packed_release(&subject, F32, &descriptor);
	return "not reached";
}

PUBLIC Variant packed_acquire_with_huge_expected(Array holder) {
	Variant subject = holder.at(0);
	descriptor = {};
	const long status = test_packed_acquire(&subject, F32, &descriptor, INT64_MAX);
	if (status == 0) {
		test_packed_release(&subject, F32, &descriptor);
	}
	return int64_t(status);
}

PUBLIC Variant packed_release_frees_only_its_copy(Array holder) {
	Variant subject = holder.at(0);
	void *block = std::malloc(64);
	descriptor = {};
	descriptor.data = uint64_t(uintptr_t(block));
	test_packed_release(&subject, F32, &descriptor);
	std::free(block);
	return "freed once";
}
