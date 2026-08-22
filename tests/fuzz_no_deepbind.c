// Strip RTLD_DEEPBIND from Godot's GDExtension dlopen.
//
// Godot loads a GDExtension with RTLD_DEEPBIND, which makes the library prefer its own
// copy of any symbol over one already loaded. That is the one thing a sanitizer runtime
// cannot survive: it needs its interceptors -- malloc above all -- to be the definition
// everybody reaches, and DEEPBIND sends the addon back to libc's. The runtime notices and
// stops the process, which is why an ASAN build otherwise dies before the first test.
//
// Preloaded ahead of the addon this clears the one flag and changes nothing else. Only
// for sanitizer runs; see tests/fuzz_sanitize.sh.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>

void *dlopen(const char *file, int mode) {
	static void *(*real_dlopen)(const char *, int);
	if (real_dlopen == NULL)
		real_dlopen = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
	return real_dlopen(file, mode & ~RTLD_DEEPBIND);
}
