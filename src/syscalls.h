#pragma once
#define GAME_API_BASE 500

// System calls written in assembly
#define ECALL_PRINT          (GAME_API_BASE+0)
#define ECALL_VCALL          (GAME_API_BASE+1)
#define ECALL_VEVAL          (GAME_API_BASE+2)
#define ECALL_LAST           (GAME_API_BASE+3)

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

#define MAKE_SYSCALL(number, rval, name, ...) \
	__asm__(".pushsection .text\n" \
		".global " #name "\n" \
		".type " #name ", @function\n" \
		"" #name ":\n" \
		"	li a7, " STRINGIFY(number) "\n" \
		"   ecall\n" \
		"   ret\n"   \
		".popsection .text\n"); \
		extern "C" rval name(__VA_ARGS__);
