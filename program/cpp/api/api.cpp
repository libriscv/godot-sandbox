#include "api.hpp"

#include "syscalls.h"
#include <cstring>
#include <exception>

/* void sys_print(const Variant *, size_t) */
MAKE_SYSCALL(ECALL_PRINT, void, sys_print, const Variant *, size_t);
MAKE_SYSCALL(ECALL_THROW, void, sys_throw, const char *, size_t, const char *, size_t, const Variant *);
EXTERN_SYSCALL(ECALL_NODE_CREATE, uint64_t, sys_node_create, Node_Create_Shortlist, const char *, size_t, const char *, size_t);

/* Default main: Do nothing */
__attribute__((weak)) int main() {
	halt(); // Prevent closing pipes, calling global destructors etc.
}

/* fast_exit */
extern "C" __attribute__((used, retain, noreturn)) void fast_exit() {
	asm(".insn i SYSTEM, 0, x0, x0, 0x7ff");
	__builtin_unreachable();
}

// Various singletons
Object Engine::get_singleton() {
	return Object("Engine");
}

Object Input::get_singleton() {
	return Object("Input");
}

Object Time::get_singleton() {
	return Object("Time");
}

// ClassDB::instantiate
Object ClassDB::instantiate(std::string_view class_name, std::string_view name) {
	return Object(sys_node_create(Node_Create_Shortlist::CREATE_CLASSDB, class_name.data(), class_name.size(), name.data(), name.size()));
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

#define STR2(x) #x
#define STR(x) STR2(x)
__asm__(".pushsection .comment\n\t"
		".string \"Godot C++ API v" STR(VERSION) "\"\n\t"
												 ".popsection");
