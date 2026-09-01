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

func string_iterate(n : int) -> int:
	var text : String = "x".repeat(n)
	var acc : int = 0
	for c in text:
		acc += c.length()
	return acc

func _number(pick : int):
	if pick == 0:
		return 1.5
	return 3

func untyped_float(n : int) -> float:
	var acc = _number(0)
	var step = _number(0)
	var i : int = 0
	while i < n:
		acc = acc + step
		i += 1
	return acc

func untyped_float_compare(n : int) -> int:
	var a = _number(0)
	var b = _number(1)
	var acc : int = 0
	var i : int = 0
	while i < n:
		if a < b:
			acc += 1
		i += 1
	return acc

# The append never runs -- acc only grows -- but neither compiler can know that,
# and without something in the loop that could touch the Array the sandbox hoists
# both size calls out of it and the kernel measures an empty loop. Both sides pay
# the same compare and branch for it.
func container_size(n : int) -> int:
	var a : Array = [1, 2, 3]
	var d : Dictionary = {"a": 1}
	var acc : int = 0
	var i : int = 0
	while i < n:
		acc += a.size() + d.size()
		if acc < 0:
			a.append(i)
		i += 1
	return acc
"""

# `struct` is a SafeGDScript extension, so this pair is intentionally equivalent
# rather than byte-identical: the engine side uses the Dictionary representation
# that crosses the sandbox boundary.
const STRUCT_SOURCE := """
struct Point:
	var x: int = 1
	var y: int = 2

func struct_read(n: int) -> int:
	var point = Point()
	var acc: int = 0
	var i: int = 0
	while i < n:
		acc += point.x + point.y
		i += 1
	return acc

func struct_construct(n: int) -> int:
	var acc: int = 0
	var i: int = 0
	while i < n:
		var point = Point(i, i + 1)
		acc += point.x
		i += 1
	return acc

func struct_read_escaped(n: int) -> int:
	var points: Array[Point] = [Point()]
	var point = points[0]
	var acc: int = 0
	var i: int = 0
	while i < n:
		acc += point.x + point.y
		i += 1
	return acc
"""

const STRUCT_GDSCRIPT_SOURCE := """
func struct_read(n: int) -> int:
	var point = {"x": 1, "y": 2}
	var acc: int = 0
	var i: int = 0
	while i < n:
		acc += point.x + point.y
		i += 1
	return acc

func struct_construct(n: int) -> int:
	var acc: int = 0
	var i: int = 0
	while i < n:
		var point = {"x": i, "y": i + 1}
		acc += point.x
		i += 1
	return acc

func struct_read_escaped(n: int) -> int:
	var points: Array = [{"x": 1, "y": 2}]
	var point = points[0]
	var acc: int = 0
	var i: int = 0
	while i < n:
		acc += point.x + point.y
		i += 1
	return acc
"""

# One row per kernel: the work unit is what `n` counts, except for fib, whose
# unit is a call and whose count is the size of the recursion tree. How many
# times a kernel is called per sample is the harness's business -- it repeats
# each case until a sample is long enough to be worth timing -- so `n` is chosen
# only for what the kernel should exercise.
#
# Loop scope release reclaims scoped variants per pass, so large n is safe.
# `string iterate` n is the character count.
const KERNELS := [
	{"group": "int loop", "fn": "loop_int", "n": 100000, "unit": "iteration"},
	{"group": "float loop", "fn": "loop_float", "n": 100000, "unit": "iteration"},
	{"group": "recursion", "fn": "fib", "n": 20, "unit": "call"},
	{"group": "array append + index", "fn": "array_sum", "n": 20000, "unit": "element"},
	{"group": "dictionary set + get", "fn": "dict_ops", "n": 20000, "unit": "op"},
	{"group": "string build", "fn": "string_ops", "n": 2000, "unit": "string"},
	{"group": "string iterate", "fn": "string_iterate", "n": 8000, "unit": "character"},
	{"group": "untyped float math", "fn": "untyped_float", "n": 100000, "unit": "operation"},
	{"group": "untyped float compare", "fn": "untyped_float_compare", "n": 100000, "unit": "comparison"},
	{"group": "container size", "fn": "container_size", "n": 100000, "unit": "size call pair"},
]

var _elf : PackedByteArray = PackedByteArray()
var _struct_elf : PackedByteArray = PackedByteArray()

func before_all():
	_elf = _compile(SOURCE)
	_struct_elf = _compile(STRUCT_SOURCE)

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
		var ops : int = _fib_calls(n) if name == "fib" else n

		var guest = sandbox.vmcallv(name, n)
		var engine = gds.call(name, n)
		assert_eq(guest, engine, "%s should return the same value in both" % name)

		var in_sandbox := func(): sandbox.vmcallv(name, n)
		var in_engine := func(): gds.call(name, n)
		_case(group, "SafeGDScript (sandbox)", ops, in_sandbox, kernel["unit"])
		_case(group, "GDScript (engine)", ops, in_engine, kernel["unit"])
		_measure(group)
		_mode(sandbox)
		_note(group, "n", n)
		_report(group)

	var struct_sandbox := _load_elf(_struct_elf, "struct Sandbox ELF")
	var struct_gds := _as_gdscript(STRUCT_GDSCRIPT_SOURCE)
	if struct_gds != null:
		for kernel in [
			{"group": "struct field read", "fn": "struct_read", "n": 20000, "unit": "iteration"},
			{"group": "struct field read (escaped)", "fn": "struct_read_escaped", "n": 20000, "unit": "iteration"},
			{"group": "struct construction", "fn": "struct_construct", "n": 10000, "unit": "instance"},
		]:
			var name: String = kernel["fn"]
			var n: int = kernel["n"]
			var group: String = kernel["group"]
			assert_eq(struct_sandbox.vmcallv(name, n), struct_gds.call(name, n),
				"%s should return the same value in both" % name)
			_case(group, "SafeGDScript (sandbox)", n,
				func(): struct_sandbox.vmcallv(name, n), kernel["unit"])
			_case(group, "GDScript (engine)", n,
				func(): struct_gds.call(name, n), kernel["unit"])
			_measure(group)
			_mode(struct_sandbox)
			_note(group, "n", n)
			_report(group)
	struct_sandbox.free()

	sandbox.free()

func after_all():
	_persist()
