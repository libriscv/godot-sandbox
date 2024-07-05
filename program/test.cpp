#include "syscalls.h"
#include <cstddef>
#include <string_view>
#include <cstdio>

/// --> Fucking ugly side <-- ///

__attribute__((noreturn)) void halt()
{
	asm (".insn i SYSTEM, 0, x0, x0, 0x7ff");
	__builtin_unreachable();
}

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

static inline void print(const char* text, size_t len)
{
	register const char* a0 asm("a0") = text;
	register size_t a1 asm("a1") = len;
	register int    a7 asm("a7") = ECALL_PRINT;

	asm volatile("ecall"
		:
		: "r"(a0), "m"(*a0), "r"(a1), "r"(a7)
		: "memory");
}

/// --> Nice side <-- ///

struct UtilityFunctions
{
	static void print(std::string_view text)
	{
		::print(text.data(), text.size());
	}
};

int main()
{
	UtilityFunctions::print("Hello, world!\n");

	// do shit here...

	halt();
}

extern "C"
void my_function()
{
	UtilityFunctions::print("Hello, world!\n");
}
