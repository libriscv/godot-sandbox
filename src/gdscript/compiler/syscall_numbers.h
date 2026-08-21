#pragma once
// Fixed contract; numbers cannot be reassigned without breaking existing ELFs.
#include "../../syscalls.h"

namespace gdscript {

constexpr int array_op(Array_Op op) { return static_cast<int>(op); }
constexpr int dictionary_op(Dictionary_Op op) { return static_cast<int>(op); }

} // namespace gdscript
