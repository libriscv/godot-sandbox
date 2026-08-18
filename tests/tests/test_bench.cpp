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

// Benchmark: hammer ECALL_VCALL through a Variant that holds an Object, which is the
// path that ends up in Object::call() just like obj.call() above.
PUBLIC Variant bench_vcall_obj(Variant vnode, Variant iterations) {
	const long n = iterations;
	Variant last;
	for (long i = 0; i < n; i++) {
		last = vnode("get_child_count");
	}
	return last;
}

// Benchmark: ECALL_VCALL on a built-in Variant type, which takes the other branch and
// dispatches through Variant::callp() instead.
PUBLIC Variant bench_vcall_builtin(Variant iterations) {
	const long n = iterations;
	Variant array = Variant::new_array();
	Variant last;
	for (long i = 0; i < n; i++) {
		last = array("size");
	}
	return last;
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

// The exact shape of the demo project's benchmark: a single host->guest call that takes
// a Node argument and returns its name. Measures the whole roundtrip, not just the syscall.
PUBLIC Variant bench_single_get_name(Node node) {
	return node.get_name();
}

// Same roundtrip, but with no Godot work in the middle: isolates the vmcall + object
// argument marshalling from the syscall itself.
PUBLIC Variant bench_single_nothing(Node node) {
	(void)node;
	return Nil;
}

// Same result as bench_single_get_name(), but expressed as sugar over ECALL_OBJ_CALLP
// instead of the dedicated ECALL_NODE op. This is the measurement that decides whether
// the generic path can replace the specialised ones outright.
PUBLIC Variant bench_single_get_name_call(Node node) {
	return node.call("get_name");
}
