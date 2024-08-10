#include <cstddef>
#include <cstdint>
struct Variant;

#define EXTERN_SYSCALL(number, rval, name, ...) \
	extern "C" rval name(__VA_ARGS__);

EXTERN_SYSCALL(ECALL_PRINT, void, sys_print, const Variant *, size_t);

extern "C" __attribute__((noreturn)) void fast_exit();
