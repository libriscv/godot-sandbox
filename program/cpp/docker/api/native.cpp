#include "syscalls.h"
#include <array>
#include <cstddef>
#include <cstdint>

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
// interactor-dress-on: realloc and free are C functions further down that
// know about aligned blocks; these are the heap's own.
CREATE_SYSCALL(sandbox_native_realloc, SYSCALL_REALLOC);
CREATE_SYSCALL(sandbox_native_free, SYSCALL_FREE);
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
// interactor-dress-on: __wrap_realloc and __wrap_free are C functions
// further down that know about aligned blocks; these are the heap's own.
CREATE_SYSCALL(sandbox_native_realloc, SYSCALL_REALLOC);
CREATE_SYSCALL(sandbox_native_free, SYSCALL_FREE);
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

extern "C" void *sandbox_native_realloc(void *ptr, size_t size);
extern "C" void sandbox_native_free(void *ptr);

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

// Aligned allocations.
//
// interactor-dress-on: the host heap hands out 16-byte-aligned blocks and has
// no aligned entry point. Upstream's fallback retried malloc(size) up to 16
// times for an aligned block and, when every try missed, returned the last
// block after it had already freed it (the loop freed each miss). At 4096
// alignment that is nearly every call; Geogram's 64-byte blocks hit it too
// (Gate 4: "Possible double-free for freed pointer" in ~Delaunay3d, then
// glibc's pthread_mutex_lock assertion).
//
// Now: over-allocate by alignment - 1 bytes and return the aligned address
// inside the block. The host heap keeps its bookkeeping outside guest memory
// and only accepts the pointer it returned, so the base cannot live in a
// header below the block: a small open-addressing table maps each aligned
// pointer that differs from its base to that base and its size. free and
// realloc (the --wrap targets, or the plain names under zig) consult it
// first; with no aligned block live the table is freed and the check is one
// load. A base that happens to be aligned already is returned as a plain
// block and never enters the table.
namespace {
struct AlignedEntry {
	uintptr_t user; // 0 empty, 1 tombstone; else an aligned pointer (>= 32-aligned)
	void *raw; // what the host heap returned
	size_t size; // the size asked for, for realloc
};
constexpr uintptr_t AL_EMPTY = 0;
constexpr uintptr_t AL_TOMB = 1;
AlignedEntry *al_tab = nullptr;
size_t al_cap = 0; // a power of two, or 0 with no table
size_t al_live = 0;
size_t al_used = 0; // live + tombstones

inline size_t al_slot(uintptr_t user) {
	return size_t((uint64_t(user) >> 5) * 0x9E3779B97F4A7C15ull >> 17) & (al_cap - 1);
}

AlignedEntry *al_find(uintptr_t user) {
	if (al_live == 0) {
		return nullptr;
	}
	for (size_t i = al_slot(user);; i = (i + 1) & (al_cap - 1)) {
		if (al_tab[i].user == user) {
			return &al_tab[i];
		}
		if (al_tab[i].user == AL_EMPTY) {
			return nullptr;
		}
	}
}

void al_put(uintptr_t user, void *raw, size_t size) {
	size_t i = al_slot(user);
	while (al_tab[i].user != AL_EMPTY && al_tab[i].user != AL_TOMB) {
		i = (i + 1) & (al_cap - 1);
	}
	if (al_tab[i].user == AL_EMPTY) {
		al_used++;
	}
	al_tab[i] = { user, raw, size };
	al_live++;
}

bool al_insert(uintptr_t user, void *raw, size_t size) {
	if ((al_used + 1) * 2 > al_cap) { // keep the load under 1/2, tombstones included
		size_t cap = 64;
		while (cap < (al_live + 1) * 4) {
			cap *= 2;
		}
		AlignedEntry *tab = static_cast<AlignedEntry *>(__wrap_malloc(cap * sizeof(AlignedEntry)));
		if (tab == nullptr) {
			return false;
		}
		for (size_t i = 0; i < cap; i++) {
			tab[i] = { AL_EMPTY, nullptr, 0 };
		}
		AlignedEntry *old = al_tab;
		const size_t old_cap = al_cap;
		al_tab = tab;
		al_cap = cap;
		al_live = 0;
		al_used = 0;
		for (size_t i = 0; i < old_cap; i++) {
			if (old[i].user != AL_EMPTY && old[i].user != AL_TOMB) {
				al_put(old[i].user, old[i].raw, old[i].size);
			}
		}
		if (old != nullptr) {
			sandbox_native_free(old);
		}
	}
	al_put(user, raw, size);
	return true;
}

// Drops the entry and answers the base to hand back to the host heap.
void *al_remove(AlignedEntry *e) {
	void *raw = e->raw;
	e->user = AL_TOMB;
	e->raw = nullptr;
	if (--al_live == 0) {
		sandbox_native_free(al_tab);
		al_tab = nullptr;
		al_cap = 0;
		al_used = 0;
	}
	return raw;
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
	if (ptr != nullptr && al_live != 0) {
		if (AlignedEntry *e = al_find(uintptr_t(ptr))) {
			sandbox_native_free(al_remove(e));
			return;
		}
	}
	sandbox_native_free(ptr);
}

// An aligned block reallocates into a plain one (C does not keep the
// alignment across realloc either).
extern "C" void *SANDBOX_REALLOC(void *ptr, size_t size) {
	if (ptr != nullptr && al_live != 0) {
		if (AlignedEntry *e = al_find(uintptr_t(ptr))) {
			if (size == 0) {
				sandbox_native_free(al_remove(e));
				return nullptr;
			}
			void *q = __wrap_malloc(size);
			if (q == nullptr) {
				return nullptr; // the old block stays valid
			}
			const size_t keep = e->size < size ? e->size : size;
			for (size_t i = 0; i < keep; i++) {
				static_cast<unsigned char *>(q)[i] = static_cast<const unsigned char *>(ptr)[i];
			}
			sandbox_native_free(al_remove(e));
			return q;
		}
	}
	return sandbox_native_realloc(ptr, size);
}

extern "C" void *memalign(size_t alignment, size_t size) {
	if (alignment <= 16) {
		return __wrap_malloc(size);
	}
	if ((alignment & (alignment - 1)) != 0) {
		return nullptr;
	}
	if (size == 0) {
		size = 1;
	}
	if (size > SIZE_MAX - alignment) {
		return nullptr;
	}
	void *raw = __wrap_malloc(size + alignment - 1);
	if (raw == nullptr) {
		return nullptr;
	}
	const uintptr_t user = (uintptr_t(raw) + alignment - 1) & ~uintptr_t(alignment - 1);
	if (user == uintptr_t(raw)) {
		return raw; // aligned as it came: a plain block
	}
	if (!al_insert(user, raw, size)) {
		sandbox_native_free(raw);
		return nullptr;
	}
	return reinterpret_cast<void *>(user);
}
extern "C" void *aligned_alloc(size_t alignment, size_t size) {
	return memalign(alignment, size);
}
extern "C" int posix_memalign(void **memptr, size_t alignment, size_t size) {
	if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) {
		return 22; // EINVAL
	}
	void *result = memalign(alignment, size);
	if (result) {
		*memptr = result;
		return 0;
	}
	return 12; // ENOMEM
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
