#include "syscalls.h"
#include <cstddef>

#define NATIVE_MEM_FUNCATTR /* */
#define NATIVE_SYSCALLS_BASE 480 /* libc starts at 480 */

#define SYSCALL_MALLOC (NATIVE_SYSCALLS_BASE + 0)
#define SYSCALL_CALLOC (NATIVE_SYSCALLS_BASE + 1)
#define SYSCALL_REALLOC (NATIVE_SYSCALLS_BASE + 2)
#define SYSCALL_FREE (NATIVE_SYSCALLS_BASE + 3)
#define SYSCALL_MEMINFO (NATIVE_SYSCALLS_BASE + 4)

#define SYSCALL_MEMCPY (NATIVE_SYSCALLS_BASE + 5)
#define SYSCALL_MEMSET (NATIVE_SYSCALLS_BASE + 6)
#define SYSCALL_MEMMOVE (NATIVE_SYSCALLS_BASE + 7)
#define SYSCALL_MEMCMP (NATIVE_SYSCALLS_BASE + 8)

#define SYSCALL_STRLEN (NATIVE_SYSCALLS_BASE + 10)
#define SYSCALL_STRCMP (NATIVE_SYSCALLS_BASE + 11)

#define SYSCALL_BACKTRACE (NATIVE_SYSCALLS_BASE + 19)

#define STR1(x) #x
#define STR(x) STR1(x)

// clang-format off
#define WRAP_FUNC(name, syscall_id)                 \
	__asm__(".pushsection .text\n"                  \
			".global __wrap_" #name "\n"            \
			".type __wrap_" #name ", @function\n"   \
			"__wrap_" #name ":\n"                   \
			"	li a7, " STR(syscall_id) "\n"       \
			"	ecall\n" \
			"	ret\n"   \
			".popsection .text\n")
// clang-format on

WRAP_FUNC(calloc, SYSCALL_CALLOC);
WRAP_FUNC(realloc, SYSCALL_REALLOC);

#define WRAP_FANCY 1

#if !WRAP_FANCY
WRAP_FUNC(malloc, SYSCALL_MALLOC);
WRAP_FUNC(free, SYSCALL_FREE);
WRAP_FUNC(memset, SYSCALL_MEMSET);
WRAP_FUNC(memcpy, SYSCALL_MEMCPY);
WRAP_FUNC(memmove, SYSCALL_MEMMOVE);
WRAP_FUNC(memcmp, SYSCALL_MEMCMP);
WRAP_FUNC(strlen, SYSCALL_STRLEN);
WRAP_FUNC(strcmp, SYSCALL_STRCMP);
WRAP_FUNC(strncmp, SYSCALL_STRCMP);

#else // WRAP_FANCY

extern "C" void *__wrap_malloc(size_t size) {
	register void *ret __asm__("a0");
	register size_t a0 __asm__("a0") = size;
	register long syscall_id __asm__("a7") = SYSCALL_MALLOC;

	asm volatile("ecall"
				 : "=m"(*(char(*)[size])ret), "=r"(ret)
				 : "r"(a0), "r"(syscall_id));
	return ret;
}
extern "C" void __wrap_free(void *ptr) {
	register void *a0 __asm__("a0") = ptr;
	register long syscall_id __asm__("a7") = SYSCALL_FREE;

	asm volatile("ecall"
				 :
				 : "r"(a0), "r"(syscall_id));
}

extern "C" void *__wrap_memset(void *vdest, const int ch, size_t size) {
	register char *a0 __asm__("a0") = (char *)vdest;
	register int a1 __asm__("a1") = ch;
	register size_t a2 __asm__("a2") = size;
	register long syscall_id __asm__("a7") = SYSCALL_MEMSET;

	asm volatile("ecall"
				 : "=m"(*(char(*)[size])a0)
				 : "r"(a0), "r"(a1), "r"(a2), "r"(syscall_id));
	return vdest;
}
extern "C" void *__wrap_memcpy(void *vdest, const void *vsrc, size_t size) {
	register char *a0 __asm__("a0") = (char *)vdest;
	register const char *a1 __asm__("a1") = (const char *)vsrc;
	register size_t a2 __asm__("a2") = size;
	register long syscall_id __asm__("a7") = SYSCALL_MEMCPY;

	asm volatile("ecall"
				 : "=m"(*(char(*)[size])a0), "+r"(a0)
				 : "r"(a1), "m"(*(const char(*)[size])a1),
				 "r"(a2), "r"(syscall_id));
	return vdest;
}
extern "C" void *__wrap_memmove(void *vdest, const void *vsrc, size_t size) {
	// An assumption is being made here that since vsrc might be
	// inside vdest, we cannot assume that vsrc is const anymore.
	register char *a0 __asm__("a0") = (char *)vdest;
	register char *a1 __asm__("a1") = (char *)vsrc;
	register size_t a2 __asm__("a2") = size;
	register long syscall_id __asm__("a7") = SYSCALL_MEMMOVE;

	asm volatile("ecall"
				 : "=m"(*(char(*)[size])a0), "=m"(*(char(*)[size])a1)
				 : "r"(a0), "r"(a1), "r"(a2), "r"(syscall_id));
	return vdest;
}
extern "C" int __wrap_memcmp(const void *m1, const void *m2, size_t size) {
	register const char *a0 __asm__("a0") = (const char *)m1;
	register const char *a1 __asm__("a1") = (const char *)m2;
	register size_t a2 __asm__("a2") = size;
	register long syscall_id __asm__("a7") = SYSCALL_MEMCMP;
	register int a0_out __asm__("a0");

	asm volatile("ecall"
				 : "=r"(a0_out)
				 : "r"(a0), "m"(*(const char(*)[size])a0),
				 "r"(a1), "m"(*(const char(*)[size])a1),
				 "r"(a2), "r"(syscall_id));
	return a0_out;
}
extern "C" size_t __wrap_strlen(const char *str) {
	register const char *a0 __asm__("a0") = str;
	register size_t a0_out __asm__("a0");
	register long syscall_id __asm__("a7") = SYSCALL_STRLEN;

	asm volatile("ecall"
				 : "=r"(a0_out)
				 : "r"(a0), "m"(*(const char(*)[4096])a0), "r"(syscall_id));
	return a0_out;
}
extern "C" int __wrap_strcmp(const char *str1, const char *str2) {
	register const char *a0 __asm__("a0") = str1;
	register const char *a1 __asm__("a1") = str2;
	register size_t a2 __asm__("a2") = 4096;
	register size_t a0_out __asm__("a0");
	register long syscall_id __asm__("a7") = SYSCALL_STRCMP;

	asm volatile("ecall"
				 : "=r"(a0_out)
				 : "r"(a0), "m"(*(const char(*)[4096])a0),
				 "r"(a1), "m"(*(const char(*)[4096])a1),
				 "r"(a2), "r"(syscall_id));
	return a0_out;
}
extern "C" int __wrap_strncmp(const char *str1, const char *str2, size_t maxlen) {
	register const char *a0 __asm__("a0") = str1;
	register const char *a1 __asm__("a1") = str2;
	register size_t a2 __asm__("a2") = maxlen;
	register size_t a0_out __asm__("a0");
	register long syscall_id __asm__("a7") = SYSCALL_STRCMP;

	asm volatile("ecall"
				 : "=r"(a0_out)
				 : "r"(a0), "m"(*(const char(*)[maxlen])a0),
				 "r"(a1), "m"(*(const char(*)[maxlen])a1),
				 "r"(a2), "r"(syscall_id));
	return a0_out;
}

#endif // WRAP_FANCY
