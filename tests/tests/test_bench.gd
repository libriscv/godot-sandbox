extends GutTest

var Sandbox_TestsTests = load("res://tests/tests.elf")

const N = 50000
const RUNS = 2

func _bench(name : String, callable : Callable) -> float:
	# Warmup, then report the best of several runs: the mean is dominated by scheduling
	# noise, while the minimum is a stable estimate of the actual cost per call.
	callable.call(10000)
	var best := 1e30
	for run in range(RUNS):
		var t0 = Time.get_ticks_usec()
		callable.call(N)
		var t1 = Time.get_ticks_usec()
		best = min(best, float(t1 - t0))
	var ns := best * 1000.0 / float(N)
	print("%-28s %8.1f ns/call" % [name, ns])
	return ns

func _callp_source(padding: int) -> String:
	var source := "extends Node\n"
	for i in padding:
		source += "func pad%d(value: int) -> int:\n\treturn value + %d\n" % [i, i]
	source += "func f0():\n\tpass\nfunc f1i(value: int) -> int:\n\treturn value\n"
	return source

func test_bench_obj_callp():
	var node = Node.new()
	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)
	s.set_instructions_max(0) # unlimited

	# The binary translator compiles the guest in the background, so run every case once
	# before measuring anything: otherwise the first case measured is the only one that
	# runs interpreted, and reads ~15% slow.
	for warmup in range(2):
		s.vmcallv("bench_empty_loop", 100000)
		s.vmcallv("bench_minimal_syscall", 100000)
		s.vmcallv("bench_obj_call", node, 100000)
		s.vmcallv("bench_obj_voidcall", node, 100000)
		s.vmcallv("bench_obj_call_arg", node, 100000)
		s.vmcallv("bench_vcall_obj", node, 100000)
		s.vmcallv("bench_vcall_builtin", 100000)

	print("--- obj_callp benchmark, N=%d ---" % N)

	_bench("guest empty loop", func(n): s.vmcallv("bench_empty_loop", n))
	_bench("minimal syscall", func(n): s.vmcallv("bench_minimal_syscall", n))
	_bench("sandbox obj.call()", func(n): s.vmcallv("bench_obj_call", node, n))
	_bench("sandbox obj.voidcall()", func(n): s.vmcallv("bench_obj_voidcall", node, n))
	_bench("sandbox obj.call(1 arg)", func(n): s.vmcallv("bench_obj_call_arg", node, n))
	_bench("sandbox vcall obj", func(n): s.vmcallv("bench_vcall_obj", node, n))
	_bench("sandbox vcall builtin", func(n): s.vmcallv("bench_vcall_builtin", n))
	_bench("gdscript direct call", func(n):
		for i in range(n):
			node.get_child_count())
	_bench("gdscript .call(name)", func(n):
		for i in range(n):
			node.call("get_child_count"))
	_bench("gdscript direct 1 arg", func(n):
		for i in range(n):
			node.set_process_priority(0))

	_bench("gdscript node.get_name()", func(n):
		for i in range(n):
			node.get_name())

	# Full host->guest->host roundtrips, which is what the demo project measures.
	var call_get_name : Callable = s.vmcallable("bench_single_get_name")
	var call_nothing : Callable = s.vmcallable("bench_single_nothing")
	var call_name_via : Callable = s.vmcallable("bench_single_get_name_call")
	_bench("roundtrip: empty", func(n):
		for i in range(n):
			call_nothing.call(node))
	_bench("roundtrip: get_name", func(n):
		for i in range(n):
			call_get_name.call(node))
	_bench("roundtrip: get_name/callp", func(n):
		for i in range(n):
			call_name_via.call(node))
	_bench("gdscript equivalent", func(n):
		for i in range(n):
			_gds_get_name(node))

	# ScriptInstance::callp itself. Keep a padded script alongside the small one
	# so a linear method lookup is immediately visible in this benchmark.
	var short_script := SafeGDScript.new()
	short_script.set_source_code(_callp_source(0))
	var short_node := Node.new()
	short_node.set_script(short_script)
	var padded_script := SafeGDScript.new()
	padded_script.set_source_code(_callp_source(30))
	var padded_node := Node.new()
	padded_node.set_script(padded_script)
	for warmup in range(10000):
		short_node.f0()
		short_node.f1i(3)
		padded_node.f1i(3)
	_bench(".sgd callp 0 args", func(n):
		for i in range(n):
			short_node.f0())
	_bench(".sgd callp 1 int", func(n):
		for i in range(n):
			short_node.f1i(3))
	_bench(".sgd callp 30 ahead", func(n):
		for i in range(n):
			padded_node.f1i(3))
	short_node.free()
	padded_node.free()

	assert_true(true)
	s.queue_free()
	node.queue_free()

func _gds_get_name(obj):
	return obj.get_name()
