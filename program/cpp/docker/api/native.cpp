#include "syscalls.h"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

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
#define CREATE_SYSCALL(name, syscall_id)                 \
	__asm__(".pushsection .text\n"                  \
			".global " #name "\n"            \
			".type " #name ", @function\n"   \
			"" #name ":\n"                   \
			"	li a7, " STR(syscall_id) "\n"       \
			"	ecall\n" \
			"	ret\n"   \
			".popsection .text\n")
#define CREATE_SYSCALL_STRCMP(name, syscall_id) \
	__asm__(".pushsection .text\n"              \
			".global " #name "\n"            \
			".type " #name ", @function\n"   \
			"" #name ":\n"                   \
			"   li a2, 4096\n"               \
			"	li a7, " STR(syscall_id) "\n"       \
			"	ecall\n" \
			"	ret\n"   \
			".popsection .text\n")

// clang-format on

#ifdef ZIG_COMPILER
#define WRAP_FANCY 0
#else
#define WRAP_FANCY 1
#endif

#if !WRAP_FANCY
CREATE_SYSCALL(malloc, SYSCALL_MALLOC);
CREATE_SYSCALL(calloc, SYSCALL_CALLOC);
CREATE_SYSCALL(__sandbox_realloc, SYSCALL_REALLOC);
CREATE_SYSCALL(__sandbox_free, SYSCALL_FREE);
CREATE_SYSCALL(memset, SYSCALL_MEMSET);
CREATE_SYSCALL(memcpy, SYSCALL_MEMCPY);
CREATE_SYSCALL(memmove, SYSCALL_MEMMOVE);
CREATE_SYSCALL(memcmp, SYSCALL_MEMCMP);
CREATE_SYSCALL(strlen, SYSCALL_STRLEN);
CREATE_SYSCALL_STRCMP(strcmp, SYSCALL_STRCMP);
CREATE_SYSCALL(strncmp, SYSCALL_STRCMP);

extern "C" void *__wrap_malloc(size_t size) {
	register void *ret __asm__("a0");
	register size_t a0 __asm__("a0") = size;
	register long syscall_id __asm__("a7") = SYSCALL_MALLOC;

	asm volatile("ecall"
				 : "=m"(*(char(*)[size])ret), "=r"(ret)
				 : "r"(a0), "r"(syscall_id));
	return ret;
}
extern "C" void free(void *ptr);
extern "C" void __wrap_free(void *ptr) {
	free(ptr);
}
#else // WRAP_FANCY

CREATE_SYSCALL(__wrap_malloc, SYSCALL_MALLOC);
CREATE_SYSCALL(__wrap_calloc, SYSCALL_CALLOC);
CREATE_SYSCALL(__sandbox_realloc, SYSCALL_REALLOC);
CREATE_SYSCALL(__sandbox_free, SYSCALL_FREE);
CREATE_SYSCALL(__wrap_memset, SYSCALL_MEMSET);
CREATE_SYSCALL(__wrap_memcpy, SYSCALL_MEMCPY);
CREATE_SYSCALL(__wrap_memmove, SYSCALL_MEMMOVE);
CREATE_SYSCALL(__wrap_memcmp, SYSCALL_MEMCMP);
CREATE_SYSCALL(__wrap_strlen, SYSCALL_STRLEN);
CREATE_SYSCALL_STRCMP(__wrap_strcmp, SYSCALL_STRCMP);
// CREATE_SYSCALL_STRCMP here would clobber a2, maxlen
CREATE_SYSCALL(__wrap_strncmp, SYSCALL_STRCMP);

extern "C" void *__wrap_malloc(size_t size);
extern "C" void __wrap_free(void *ptr);
#endif // WRAP_FANCY

extern "C" void *__sandbox_realloc(void *ptr, size_t size);
extern "C" void __sandbox_free(void *ptr);

// extern "C" void *__wrap_memset(void *vdest, const int ch, size_t size) {
// 	register char *a0 __asm__("a0") = (char *)vdest;
// 	register int a1 __asm__("a1") = ch;
// 	register size_t a2 __asm__("a2") = size;
// 	register long syscall_id __asm__("a7") = SYSCALL_MEMSET;

// 	asm volatile("ecall"
// 				 : "=m"(*(char(*)[size])a0)
// 				 : "r"(a0), "r"(a1), "r"(a2), "r"(syscall_id));
// 	return vdest;
// }
// extern "C" void *__wrap_memcpy(void *vdest, const void *vsrc, size_t size) {
// 	register char *a0 __asm__("a0") = (char *)vdest;
// 	register const char *a1 __asm__("a1") = (const char *)vsrc;
// 	register size_t a2 __asm__("a2") = size;
// 	register long syscall_id __asm__("a7") = SYSCALL_MEMCPY;

// 	asm volatile("ecall"
// 				 : "=m"(*(char(*)[size])a0), "+r"(a0)
// 				 : "r"(a1), "m"(*(const char(*)[size])a1),
// 				 "r"(a2), "r"(syscall_id));
// 	return vdest;
// }
// extern "C" void *__wrap_memmove(void *vdest, const void *vsrc, size_t size) {
// 	// An assumption is being made here that since vsrc might be
// 	// inside vdest, we cannot assume that vsrc is const anymore.
// 	register char *a0 __asm__("a0") = (char *)vdest;
// 	register char *a1 __asm__("a1") = (char *)vsrc;
// 	register size_t a2 __asm__("a2") = size;
// 	register long syscall_id __asm__("a7") = SYSCALL_MEMMOVE;

// 	asm volatile("ecall"
// 				 : "=m"(*(char(*)[size])a0), "=m"(*(char(*)[size])a1)
// 				 : "r"(a0), "r"(a1), "r"(a2), "r"(syscall_id));
// 	return vdest;
// }
// extern "C" int __wrap_memcmp(const void *m1, const void *m2, size_t size) {
// 	register const char *a0 __asm__("a0") = (const char *)m1;
// 	register const char *a1 __asm__("a1") = (const char *)m2;
// 	register size_t a2 __asm__("a2") = size;
// 	register long syscall_id __asm__("a7") = SYSCALL_MEMCMP;
// 	register int a0_out __asm__("a0");

// 	asm volatile("ecall"
// 				 : "=r"(a0_out)
// 				 : "r"(a0), "m"(*(const char(*)[size])a0),
// 				 "r"(a1), "m"(*(const char(*)[size])a1),
// 				 "r"(a2), "r"(syscall_id));
// 	return a0_out;
// }
// extern "C" size_t __wrap_strlen(const char *str) {
// 	register const char *a0 __asm__("a0") = str;
// 	register size_t a0_out __asm__("a0");
// 	register long syscall_id __asm__("a7") = SYSCALL_STRLEN;

// 	asm volatile("ecall"
// 				 : "=r"(a0_out)
// 				 : "r"(a0), "m"(*(const char(*)[4096])a0), "r"(syscall_id));
// 	return a0_out;
// }
// extern "C" int __wrap_strcmp(const char *str1, const char *str2) {
// 	register const char *a0 __asm__("a0") = str1;
// 	register const char *a1 __asm__("a1") = str2;
// 	register size_t a2 __asm__("a2") = 4096;
// 	register size_t a0_out __asm__("a0");
// 	register long syscall_id __asm__("a7") = SYSCALL_STRCMP;

// 	asm volatile("ecall"
// 				 : "=r"(a0_out)
// 				 : "r"(a0), "m"(*(const char(*)[4096])a0),
// 				 "r"(a1), "m"(*(const char(*)[4096])a1),
// 				 "r"(a2), "r"(syscall_id));
// 	return a0_out;
// }
// extern "C" int __wrap_strncmp(const char *str1, const char *str2, size_t maxlen) {
// 	register const char *a0 __asm__("a0") = str1;
// 	register const char *a1 __asm__("a1") = str2;
// 	register size_t a2 __asm__("a2") = maxlen;
// 	register size_t a0_out __asm__("a0");
// 	register long syscall_id __asm__("a7") = SYSCALL_STRCMP;

// 	asm volatile("ecall"
// 				 : "=r"(a0_out)
// 				 : "r"(a0), "m"(*(const char(*)[maxlen])a0),
// 				 "r"(a1), "m"(*(const char(*)[maxlen])a1),
// 				 "r"(a2), "r"(syscall_id));
// 	return a0_out;
// }

// The host heap only frees the exact pointer it returned, so over-aligned
// blocks are carved out of a larger one and their base is kept here.
namespace {
struct AlignedBlock {
	uintptr_t ptr;
	void *base;
	size_t size;
};
AlignedBlock *aligned_table = nullptr;
size_t aligned_capacity = 0;
size_t aligned_count = 0;

size_t aligned_slot(uintptr_t ptr) {
	return size_t(uint64_t(ptr >> 5) * 0x9E3779B97F4A7C15ull >> 32) & (aligned_capacity - 1);
}

AlignedBlock *aligned_find(uintptr_t ptr) {
	for (size_t i = aligned_slot(ptr);; i = (i + 1) & (aligned_capacity - 1)) {
		if (aligned_table[i].ptr == 0) {
			return nullptr;
		}
		if (aligned_table[i].ptr == ptr) {
			return &aligned_table[i];
		}
	}
}

void aligned_place(const AlignedBlock &block) {
	size_t i = aligned_slot(block.ptr);
	while (aligned_table[i].ptr != 0) {
		i = (i + 1) & (aligned_capacity - 1);
	}
	aligned_table[i] = block;
}

bool aligned_insert(uintptr_t ptr, void *base, size_t size) {
	if ((aligned_count + 1) * 2 > aligned_capacity) {
		const size_t capacity = aligned_capacity ? aligned_capacity * 2 : 64;
		AlignedBlock *table = static_cast<AlignedBlock *>(__wrap_malloc(capacity * sizeof(AlignedBlock)));
		if (table == nullptr) {
			return false;
		}
		for (size_t i = 0; i < capacity; i++) {
			table[i].ptr = 0;
		}
		AlignedBlock *old = aligned_table;
		const size_t old_capacity = aligned_capacity;
		aligned_table = table;
		aligned_capacity = capacity;
		for (size_t i = 0; i < old_capacity; i++) {
			if (old[i].ptr != 0) {
				aligned_place(old[i]);
			}
		}
		__sandbox_free(old);
	}
	aligned_place({ ptr, base, size });
	aligned_count++;
	return true;
}

// Backward-shift deletion keeps probe chains intact without tombstones.
void *aligned_erase(AlignedBlock *block) {
	void *base = block->base;
	const size_t mask = aligned_capacity - 1;
	size_t hole = block - aligned_table;
	for (size_t i = (hole + 1) & mask; aligned_table[i].ptr != 0; i = (i + 1) & mask) {
		const size_t home = aligned_slot(aligned_table[i].ptr);
		if (((i - home) & mask) >= ((i - hole) & mask)) {
			aligned_table[hole] = aligned_table[i];
			hole = i;
		}
	}
	aligned_table[hole].ptr = 0;
	aligned_count--;
	return base;
}
} // namespace

#if WRAP_FANCY
#define SANDBOX_FREE __wrap_free
#define SANDBOX_REALLOC __wrap_realloc
#else
#define SANDBOX_FREE free
#define SANDBOX_REALLOC realloc
#endif

extern "C" void SANDBOX_FREE(void *ptr) {
	if (aligned_count != 0) {
		if (AlignedBlock *block = aligned_find(uintptr_t(ptr))) {
			ptr = aligned_erase(block);
		}
	}
	__sandbox_free(ptr);
}

extern "C" void *SANDBOX_REALLOC(void *ptr, size_t size) {
	if (aligned_count != 0) {
		if (AlignedBlock *block = aligned_find(uintptr_t(ptr))) {
			void *result = __wrap_malloc(size);
			if (result == nullptr) {
				return nullptr;
			}
			std::memcpy(result, ptr, block->size < size ? block->size : size);
			__sandbox_free(aligned_erase(block));
			return result;
		}
	}
	return __sandbox_realloc(ptr, size);
}

extern "C" void *memalign(size_t alignment, size_t size) {
	if (alignment <= 16) {
		return __wrap_malloc(size);
	}
	if ((alignment & (alignment - 1)) != 0 || size > SIZE_MAX - alignment) {
		return nullptr;
	}
	void *base = __wrap_malloc(size + alignment - 1);
	if (base == nullptr) {
		return nullptr;
	}
	const uintptr_t ptr = (uintptr_t(base) + alignment - 1) & ~uintptr_t(alignment - 1);
	if (ptr != uintptr_t(base) && !aligned_insert(ptr, base, size)) {
		__sandbox_free(base);
		return nullptr;
	}
	return reinterpret_cast<void *>(ptr);
}
extern "C" void *aligned_alloc(size_t alignment, size_t size) {
	return memalign(alignment, size);
}
extern "C" int posix_memalign(void **memptr, size_t alignment, size_t size) {
	if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) {
		return EINVAL;
	}
	void *result = memalign(alignment, size);
	if (result == nullptr) {
		return ENOMEM;
	}
	*memptr = result;
	return 0;
}

void *operator new(size_t size) noexcept(false) {
	return __wrap_malloc(size);
}
void *operator new[](size_t size) noexcept(false) {
	return __wrap_malloc(size);
}
void operator delete(void *ptr) noexcept(true) {
	__wrap_free(ptr);
}
void operator delete[](void *ptr) noexcept(true) {
	__wrap_free(ptr);
}
void *operator new(size_t size, size_t alignment) noexcept(false) {
	return memalign(alignment, size);
}
void *operator new[](size_t size, size_t alignment) noexcept(false) {
	return memalign(alignment, size);
}
void operator delete(void *ptr, size_t alignment) noexcept(true) {
	__wrap_free(ptr);
}
void operator delete[](void *ptr, size_t alignment) noexcept(true) {
	__wrap_free(ptr);
}
