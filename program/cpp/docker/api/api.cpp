#include "api.hpp"

#include "syscalls.h"
#include <cstring>
#include <exception>

/* void sys_print(const Variant *, size_t) */
MAKE_SYSCALL(ECALL_PRINT, void, sys_print, const Variant *, size_t);
MAKE_SYSCALL(ECALL_THROW, void, sys_throw, const char *, size_t, const char *, size_t, ...);
EXTERN_SYSCALL(uint64_t, sys_node_create, Node_Create_Shortlist, const char *, size_t, const char *, size_t);
MAKE_SYSCALL(ECALL_LOAD, void, sys_load, const char *, size_t, Variant *);
MAKE_SYSCALL(ECALL_SANDBOX_ADD, void, sys_sandbox_add, int, ...);

/* Default main: Do nothing */
__attribute__((weak)) int main() {
	halt(); // Prevent closing pipes, calling global destructors etc.
}

/* fast_exit */
extern "C" __attribute__((used, retain, noreturn)) void fast_exit() {
	asm(".insn i SYSTEM, 0, x0, x0, 0x7ff");
	__builtin_unreachable();
}

// ClassDB::instantiate
Object ClassDB::instantiate(std::string_view class_name, std::string_view name) {
	return Object(sys_node_create(Node_Create_Shortlist::CREATE_CLASSDB, class_name.data(), class_name.size(), name.data(), name.size()));
}

// Resource loader
Variant loadv(std::string_view path) {
	Variant result;
	sys_load(path.data(), path.size(), &result);
	return result;
}

__attribute__((constructor, used)) void setup_native_stuff() {
	/* Set exit address to fast_exit */
	sys_sandbox_add(2, &fast_exit);
	/* Handle uncaught C++ exceptions */
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

// clang-format off
#define STR2(x) #x
#define STR(x) STR2(x)
__asm__(".pushsection .comment\n\t"
		".string \"Godot C++ API v" STR(VERSION) "\"\n\t"
		".popsection");
// clang-format on
