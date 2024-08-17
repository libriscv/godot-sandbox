#include "api.hpp"

#include "syscalls.h"
#include <exception>
#include <cstring>

/* void sys_print(const Variant *, size_t) */
MAKE_SYSCALL(ECALL_PRINT, void, sys_print, const Variant *, size_t);
MAKE_SYSCALL(ECALL_THROW, void, sys_throw, const char *, size_t, const char *, size_t, const Variant *);

/* Default main: Do nothing */
__attribute__((weak)) int main() {
	halt(); // Prevent closing pipes, calling global destructors etc.
}

/* fast_exit */
extern "C" __attribute__((used, retain, noreturn)) void fast_exit() {
	asm(".insn i SYSTEM, 0, x0, x0, 0x7ff");
	__builtin_unreachable();
}

/* Handle uncaught C++ exceptions */
__attribute__((constructor)) void handle_unhandled_exceptions() {
	std::set_terminate([] {
		try {
			std::rethrow_exception(std::current_exception());
		} catch (const std::exception &e) {
			const auto *name = typeid(e).name();
			sys_throw(name, strlen(name), e.what(), strlen(e.what()), nullptr);
			__builtin_unreachable();
		}
	});
}
