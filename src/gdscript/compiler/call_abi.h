#pragma once
#include <cstddef>

namespace gdscript {

// SafeGDScript's boxed call convention. a0 is the return Variant pointer,
// a1-a7 hold the first seven argument pointers, and the remaining pointers
// start at 0(sp) on entry. The existing Sandbox boxed-call bridge supports
// sixteen arguments, so keep the compiler and the host-side bridges on the
// same explicit limit.
struct CallABI {
	static constexpr size_t REGISTER_ARGUMENTS = 7;
	static constexpr size_t MAX_ARGUMENTS = 16;
	static constexpr size_t STACK_SLOT_SIZE = 8;

	static constexpr size_t overflow_arguments(size_t count) {
		return count > REGISTER_ARGUMENTS ? count - REGISTER_ARGUMENTS : 0;
	}
};

} // namespace gdscript
