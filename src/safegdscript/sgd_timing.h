// Env-gated wall-clock accounting for the two costs a .sgd pays over a .gd:
// compiling the source to a RISC-V ELF, and loading that ELF into libriscv.
// Enabled by SGD_TIMING=1; totals print to stderr at library teardown.
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace sgd_timing {

struct Totals {
	unsigned long compiles = 0;
	unsigned long compile_ns = 0;
	unsigned long loads = 0;
	unsigned long load_ns = 0;
	unsigned long elf_bytes = 0;

	~Totals() {
		if (compiles == 0 && loads == 0) {
			return;
		}
		std::fprintf(stderr,
				"SGD_TIMING: compile %lu calls %.1f ms | load_buffer %lu calls %.1f ms | "
				"total %.1f ms | ELF %.1f KiB\n",
				compiles, compile_ns / 1e6, loads, load_ns / 1e6,
				(compile_ns + load_ns) / 1e6, elf_bytes / 1024.0);
	}
};

inline Totals &totals() {
	static Totals t;
	return t;
}

inline bool enabled() {
	static const bool on = std::getenv("SGD_TIMING") != nullptr;
	return on;
}

// Adds its lifetime to *p_accumulator when SGD_TIMING is set; otherwise free.
struct Scope {
	unsigned long *accumulator;
	unsigned long *counter;
	std::chrono::steady_clock::time_point start;

	Scope(unsigned long *p_accumulator, unsigned long *p_counter) :
			accumulator(enabled() ? p_accumulator : nullptr), counter(p_counter) {
		if (accumulator) {
			start = std::chrono::steady_clock::now();
		}
	}
	~Scope() {
		if (accumulator) {
			*accumulator += std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - start)
									.count();
			*counter += 1;
		}
	}
};

} // namespace sgd_timing

#define SGD_TIME_COMPILE() \
	sgd_timing::Scope _sgd_timing_scope(&sgd_timing::totals().compile_ns, &sgd_timing::totals().compiles)
#define SGD_TIME_LOAD() \
	sgd_timing::Scope _sgd_timing_scope(&sgd_timing::totals().load_ns, &sgd_timing::totals().loads)
