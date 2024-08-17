#pragma once
#include <cstddef>
#include <cstdint>
struct Variant;

#define EXTERN_SYSCALL(number, rval, name, ...) \
	extern "C" rval name(__VA_ARGS__);

EXTERN_SYSCALL(ECALL_PRINT, void, sys_print, const Variant *, size_t);
EXTERN_SYSCALL(ECALL_THROW, void, sys_throw, const char *, size_t, const char *, size_t, const Variant *);

inline __attribute__((noreturn)) void api_throw(std::string_view type, std::string_view msg, const Variant *srcVar = nullptr) {
	sys_throw(type.data(), type.size(), msg.data(), msg.size(), srcVar);
	__builtin_unreachable();
}

extern "C" __attribute__((noreturn)) void fast_exit();
