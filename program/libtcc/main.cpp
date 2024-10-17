#include <libtcc.h>
#include <api.hpp>
#include <cstdarg>
#include <cstring>
EXTERN_SYSCALL(void, sys_vstore, unsigned, const void *, size_t);
EXTERN_SYSCALL(void, sys_vfetch, unsigned, void *, int);

#define VERBOSE_COMPILE 0

static TCCState *ctx;
static Variant do_compile(const std::string &source_code, const std::string &entry) {
#if VERBOSE_COMPILE
	printf("Compiling C code: %s\n", source_code.c_str());
	fflush(stdout);
#endif

	// Create a new TCC context
	ctx = tcc_new();
	if (!ctx) {
		fprintf(stderr, "Failed to create TCC context\n");
		fflush(stdout);
		return Nil;
	}

	// Set the output type to memory
	tcc_set_output_type(ctx, TCC_OUTPUT_MEMORY);
	// Set freestanding/nostdlib mode
	tcc_set_options(ctx, "-ffreestanding -nostdlib");

	// Add the API
	tcc_add_symbol(ctx, "memcpy", (void *)memcpy);
	tcc_add_symbol(ctx, "memmove", (void *)memmove);
	tcc_add_symbol(ctx, "memset", (void *)memset);
	tcc_add_symbol(ctx, "memcmp", (void *)memcmp);
	tcc_add_symbol(ctx, "malloc", (void *)malloc);
	tcc_add_symbol(ctx, "free", (void *)free);
	tcc_add_symbol(ctx, "sys_vstore", (void *)sys_vstore);
	tcc_add_symbol(ctx, "sys_vfetch", (void *)sys_vfetch);

	// Debugging API
	// clang-format off
	tcc_add_symbol(ctx, "print_int", (void *)(void(*)(int))[](int i) {
		printf("Int: %d", i);
		fflush(stdout);
	});
	tcc_add_symbol(ctx, "print_float", (void *)(void(*)(float))[](float f) {
		printf("Float: %f", f);
		fflush(stdout);
	});
	tcc_add_symbol(ctx, "print_string", (void *)(void(*)(const char*))[](const char *s) {
		printf("String: %s", s);
		fflush(stdout);
	});
	tcc_add_symbol(ctx, "print_ptr", (void *)(void(*)(void*))[](void *p) {
		printf("Pointer: %p", p);
		fflush(stdout);
	});
	// clang-format on

	// Add our own API
	std::string code = R"(
	extern void sys_vfetch(unsigned, void *, int);
	extern void sys_vstore(unsigned *, int, const void *, unsigned);
	extern void print_int(int);
	extern void print_float(float);
	extern void print_string(const char*);
	extern void print_ptr(void*);
	extern void *malloc(unsigned long);
	extern void free(void*);
	struct Variant {
		long type;
		long value;
	};
	struct VectorF32 {
		float *f_begin;
		float *f_end;
		float *f_cap;
	};
)";
	code += source_code;

	// Compile the code
	if (tcc_compile_string(ctx, code.c_str()) == -1) {
		fprintf(stderr, "Failed to compile code\n");
		fflush(stdout);
		return Nil;
	}

	// Link the code
	if (tcc_relocate(ctx, TCC_RELOCATE_AUTO) < 0) {
		fprintf(stderr, "Failed to link code\n");
		fflush(stdout);
		return Nil;
	}

#if VERBOSE_COMPILE
	printf("Code compiled successfully\n");
	fflush(stdout);
#endif

	// Get the entry function address
	Variant (*fun_addr)() = NULL;
	fun_addr = (Variant(*)())tcc_get_symbol(ctx, entry.c_str());
	if (!fun_addr) {
		fprintf(stderr, "Function %s not found\n", entry.c_str());
		fflush(stdout);
		return Nil;
	}

#if VERBOSE_COMPILE
	printf("Function %s found, address %p\n", entry.c_str(), fun_addr);
	fflush(stdout);
#endif

	// Return a callable function
	return Callable::Create<Variant()>(fun_addr);
}

extern "C" Variant compile(String code, String entry) {
	const std::string utf = code.utf8();
	const std::string entry_utf = entry.utf8();

	return do_compile(utf, entry);
}
