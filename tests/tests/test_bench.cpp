#include "api.hpp"

// Benchmark: hammer ECALL_OBJ_CALLP with a trivial native method.
PUBLIC Variant bench_obj_call(Variant vnode, Variant iterations) {
	Object obj = vnode.as_object();
	const long n = iterations;
	Variant last;
	for (long i = 0; i < n; i++) {
		last = obj.call("get_child_count");
	}
	return last;
}

// Benchmark: same, but discarding the return value (voidcall path).
PUBLIC Variant bench_obj_voidcall(Variant vnode, Variant iterations) {
	Object obj = vnode.as_object();
	const long n = iterations;
	for (long i = 0; i < n; i++) {
		obj.voidcall("get_child_count");
	}
	return Nil;
}

// Benchmark: one integer argument.
PUBLIC Variant bench_obj_call_arg(Variant vnode, Variant iterations) {
	Object obj = vnode.as_object();
	const long n = iterations;
	for (long i = 0; i < n; i++) {
		obj.voidcall("set_process_priority", 0);
	}
	return Nil;
}

// Baseline: the floor for any system call, doing no Godot work at all.
PUBLIC Variant bench_minimal_syscall(Variant iterations) {
	const long n = iterations;
	long sum = 0;
	for (long i = 0; i < n; i++) {
		sum += is_editor();
	}
	return sum;
}

// Baseline: measure pure guest loop overhead with no system call at all.
PUBLIC Variant bench_empty_loop(Variant iterations) {
	const long n = iterations;
	long sum = 0;
	for (long i = 0; i < n; i++) {
		asm volatile("" : "+r"(sum));
		sum += i;
	}
	return sum;
}
