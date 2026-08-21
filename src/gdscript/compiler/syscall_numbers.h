#pragma once
// The syscall numbers and operation codes the backend emits.
//
// These are a fixed contract between the guest and the host: a number that
// moves breaks every program already compiled against it. src/syscalls.h is
// where they are declared, and it is plain macros and enums with no Godot
// dependency, so the compiler includes it directly rather than restating the
// numbers -- which is how ECALL_UTILITY came to be spelled 549 in one file and
// 500-plus-49 in another.
//
// The relative path holds for both builds of this library: the CMake one under
// src/gdscript/compiler/build, and the guest one that compiles the compiler
// into gdscript.elf.
#include "../../syscalls.h"

namespace gdscript {

// The op selector these two syscalls take in a0. Spelled as ints because that
// is what emit_li() takes.
constexpr int array_op(Array_Op op) { return static_cast<int>(op); }
constexpr int dictionary_op(Dictionary_Op op) { return static_cast<int>(op); }

} // namespace gdscript
