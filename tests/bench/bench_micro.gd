# Micro-kernels: one loop each, so a change in the numbers points at one thing.
#
# The CPU bench measures a program of the shape people write; these measure the
# pieces it is built out of, which is what makes a regression legible. Every
# kernel is compiled from one source string twice -- once to RISC-V by the
# SafeGDScript compiler, once by the engine -- and asserted to return the same
# answer before either is timed.
extends "res://bench/bench_harness.gd"

const SOURCE := """
func loop_int(n : int) -> int:
	var acc : int = 0
	var i : int = 0
	while i < n:
		acc += i * 3 - (i >> 2)
		i += 1
	return acc

func loop_float(n : int) -> float:
	var acc : float = 0.0
	var i : int = 0
	while i < n:
		acc += float(i) * 0.5 - 0.25
		i += 1
	return acc

func fib(n : int) -> int:
	if n < 2:
		return n
	return fib(n - 1) + fib(n - 2)

func array_sum(n : int) -> int:
	var a : Array = []
	var i : int = 0
	while i < n:
		a.append(i)
		i += 1
	var acc : int = 0
	var j : int = 0
	while j < n:
		acc += a[j]
		j += 1
	return acc

func dict_ops(n : int) -> int:
	var d : Dictionary = {}
	var i : int = 0
	while i < n:
		d[i & 63] = i
		i += 1
	var acc : int = 0
	var j : int = 0
	while j < 64:
		acc += d[j]
		j += 1
	return acc

func string_ops(n : int) -> int:
	var acc : int = 0
	var i : int = 0
	while i < n:
		var s : String = "value " + str(i)
		acc += s.length()
		i += 1
	return acc
"""

# One row per kernel: the work unit is what `n` counts, except for fib, whose
# unit is a call and whose count is the size of the recursion tree. `reps` is how
# many times the kernel is called per sample, for kernels that cannot be given a
# large n.
#
# string_ops is one of those: every String a guest builds is a scoped variant on
# the host side, and Sandbox::MAX_REFS caps those at 100 for the duration of a
# call -- its iteration spends three, on str(i), the concatenation and the
# length. A guest loop that builds more than the cap aborts, so the loop is kept
# well under it and the call is repeated instead. This is a property of the
# sandbox, not of the compiler: raising it is set_max_refs().
const KERNELS := [
	{"group": "int loop", "fn": "loop_int", "n": 100000, "reps": 1, "unit": "iteration"},
	{"group": "float loop", "fn": "loop_float", "n": 100000, "reps": 1, "unit": "iteration"},
	{"group": "recursion", "fn": "fib", "n": 20, "reps": 1, "unit": "call"},
	{"group": "array append + index", "fn": "array_sum", "n": 20000, "reps": 1, "unit": "element"},
	{"group": "dictionary set + get", "fn": "dict_ops", "n": 20000, "reps": 1, "unit": "op"},
	{"group": "string build", "fn": "string_ops", "n": 20, "reps": 500, "unit": "string"},
]

var _elf : PackedByteArray = PackedByteArray()

func before_all():
	_elf = _compile(SOURCE)

# fib(n) calls itself once per node of its recursion tree: 2 * F(n + 1) - 1.
func _fib_calls(n: int) -> int:
	var a := 0
	var b := 1
	for i in range(n + 1):
		var t := a + b
		a = b
		b = t
	return 2 * a - 1

func test_bench_micro_kernels():
	if _elf.is_empty():
		return
	var sandbox := _load_elf(_elf)
	var gds := _as_gdscript(SOURCE)
	if gds == null:
		sandbox.free()
		return

	for kernel in KERNELS:
		var name : String = kernel["fn"]
		var n : int = kernel["n"]
		var group : String = kernel["group"]
		var reps : int = kernel["reps"]
		var per_call : int = _fib_calls(n) if name == "fib" else n
		var ops : int = per_call * reps

		var guest = sandbox.vmcallv(name, n)
		var engine = gds.call(name, n)
		assert_eq(guest, engine, "%s should return the same value in both" % name)

		var in_sandbox := func():
			for r in range(reps):
				sandbox.vmcallv(name, n)
		var in_engine := func():
			for r in range(reps):
				gds.call(name, n)
		_bench(group, "SafeGDScript (sandbox)", ops, in_sandbox, kernel["unit"])
		_bench(group, "GDScript (engine)", ops, in_engine, kernel["unit"])
		_note(group, "mode", _mode(sandbox))
		_note(group, "n", n)
		_note(group, "reps", reps)
		_report(group, "GDScript (engine)")

	sandbox.free()

func after_all():
	_persist()
