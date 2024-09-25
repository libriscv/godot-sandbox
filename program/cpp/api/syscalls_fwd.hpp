#pragma once
#include <cstddef>
#include <cstdint>
struct Variant;

#define EXTERN_SYSCALL(rval, name, ...) \
	extern "C" rval name(__VA_ARGS__);

EXTERN_SYSCALL(void, sys_print, const Variant *, size_t);
EXTERN_SYSCALL(void, sys_throw, const char *, size_t, const char *, size_t, const Variant *);

inline __attribute__((noreturn)) void api_throw(std::string_view type, std::string_view msg, const Variant *srcVar = nullptr) {
	sys_throw(type.data(), type.size(), msg.data(), msg.size(), srcVar);
	__builtin_unreachable();
}

extern "C" __attribute__((noreturn)) void fast_exit();

// Helper method to call a method on any type that can be wrapped in a Variant
#define CREATE_METHOD(name) \
	template <typename... Args> \
	inline Variant name(Args&&... args) { \
		return operator() (#name, std::forward<Args>(args)...); \
	}

// Alias for CREATE_METHOD
#define METHOD(name) CREATE_METHOD(name)
