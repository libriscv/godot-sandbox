#include "api.hpp"

#include "syscalls.h"

/* void sys_print(const Variant *, size_t) */
MAKE_SYSCALL(ECALL_PRINT, void, sys_print, const Variant *, size_t);

/* fast_exit */
extern "C" __attribute__((used, retain, noreturn)) void fast_exit() {
	asm(".insn i SYSTEM, 0, x0, x0, 0x7ff");
	__builtin_unreachable();
}
