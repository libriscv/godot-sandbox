#include "syscalls.h"
#include <cstddef>
#include <string_view>
#include <cstdio>

/// --> Fucking ugly side <-- ///

extern "C" __attribute__((used, retain, noreturn)) void fast_exit()
{
	asm (".insn i SYSTEM, 0, x0, x0, 0x7ff");
	__builtin_unreachable();
}

MAKE_SYSCALL(ECALL_PRINT, void, sys_print, const char*, size_t);

/// --> Nice side <-- ///

struct UtilityFunctions
{
	static void print(std::string_view text)
	{
		sys_print(text.data(), text.size());
	}
};

int main()
{
	UtilityFunctions::print("Hello, world!\n");

	// do shit here...

	fast_exit();
}

extern "C"
void my_function()
{
	UtilityFunctions::print("Hello, world!\n");
}
