# The logic CPU, compiled two ways.
#
# res://tests/test_cpu.sgd is a register machine whose execute step is one
# `match` on the opcode -- the shape the jump-table lowering was written for, and
# the shape people build with this. The same file is run here by SafeGDScript,
# compiled to RISC-V and executed in a Sandbox, and by the engine's own GDScript,
# on the same program and the same fuel. One source string feeds both.
extends "res://bench/bench_harness.gd"

const CPU_SOURCE_PATH := "res://tests/test_cpu.sgd"

# Opcodes, as test_cpu.sgd numbers them.
const OP_HALT := 0
const OP_LOADI := 1
const OP_MOV := 2
const OP_ADD := 3
const OP_SUB := 4
const OP_MUL := 5
const OP_AND := 6
const OP_OR := 7
const OP_XOR := 8
const OP_SHL := 9
const OP_SHR := 10
const OP_LT := 11
const OP_JMP := 12
const OP_JZ := 13
const OP_JNZ := 14
const OP_OUT := 15

# The workload: OUTER passes over an inner loop of LIMIT - 1 iterations. Every
# field of an instruction word is 8 bits, so both stay under 256, and the
# emulated program has to be nested to reach a size worth timing.
const LIMIT := 200
const OUTER := 20

func _encode(op: int, dst: int, src: int, imm: int) -> int:
	return op | (dst << 8) | (src << 16) | (imm << 24)

# A program that touches eleven of the sixteen arms per inner iteration, rather
# than a two-instruction spin: a dispatch benchmark whose subject is always the
# same opcode measures the branch predictor.
#
#   r0 acc   r1 i   r2 limit (also a mask)   r3 one   r4 temp   r5 outer
func _make_program() -> Array:
	var p : Array = []
	p.append(_encode(OP_LOADI, 0, 0, 0))      #  0: acc = 0
	p.append(_encode(OP_LOADI, 5, 0, OUTER))  #  1: outer = OUTER
	p.append(_encode(OP_LOADI, 3, 0, 1))      #  2: one = 1
	p.append(_encode(OP_LOADI, 2, 0, LIMIT))  #  3: limit = LIMIT
	p.append(_encode(OP_LOADI, 1, 0, 1))      #  4: i = 1          <- outer head
	p.append(_encode(OP_MOV,   4, 1, 0))      #  5: t = i          <- inner head
	p.append(_encode(OP_LT,    4, 2, 0))      #  6: t = t < limit
	p.append(_encode(OP_JZ,    4, 0, 17))     #  7: if !t goto 17
	p.append(_encode(OP_ADD,   0, 1, 0))      #  8: acc += i
	p.append(_encode(OP_MOV,   4, 0, 0))      #  9: t = acc
	p.append(_encode(OP_SHR,   4, 0, 1))      # 10: t >>= 1
	p.append(_encode(OP_XOR,   0, 4, 0))      # 11: acc ^= t
	p.append(_encode(OP_MUL,   0, 3, 0))      # 12: acc *= one
	p.append(_encode(OP_AND,   0, 2, 0))      # 13: acc &= limit
	p.append(_encode(OP_OR,    0, 3, 0))      # 14: acc |= one
	p.append(_encode(OP_ADD,   1, 3, 0))      # 15: i += one
	p.append(_encode(OP_JMP,   0, 0, 5))      # 16: goto 5
	p.append(_encode(OP_SUB,   5, 3, 0))      # 17: outer -= one   <- inner exit
	p.append(_encode(OP_JNZ,   5, 0, 4))      # 18: if outer goto 4
	p.append(_encode(OP_OUT,   0, 0, 0))      # 19: emit acc
	p.append(_encode(OP_HALT,  0, 0, 0))      # 20: stop
	return p

# Emulated instructions the program above executes: the four-instruction setup,
# then per outer pass one LOADI, twelve per full inner iteration, three for the
# check that ends the inner loop and two to close the outer one, then OUT and
# HALT. Asserted against the fuel counter below rather than trusted.
func _instruction_count() -> int:
	return 4 + OUTER * (12 * LIMIT - 6) + 2

var _source := ""
var _elf : PackedByteArray = PackedByteArray()

func before_all():
	_source = _load_source()
	if _source != "":
		_elf = _compile(_source)

func _load_source() -> String:
	var file := FileAccess.open(CPU_SOURCE_PATH, FileAccess.READ)
	assert_not_null(file, "test_cpu.sgd should be readable")
	if file == null:
		return ""
	var source := file.get_as_text()
	file.close()
	return source

func test_bench_cpu_dispatch():
	if _elf.is_empty():
		return
	var sandbox := _load_elf(_elf)
	var gds := _as_gdscript(_source)
	if gds == null:
		sandbox.free()
		return

	var program := _make_program()
	var ops := _instruction_count()
	var fuel := ops

	# What the machine computes matters only in that both machines compute it:
	# a benchmark of two implementations that disagree is a benchmark of nothing.
	var expected = gds.call("run", program, fuel)
	assert_eq(expected.size(), 1, "the program should emit exactly one value")
	assert_eq(sandbox.vmcallv("run", program, fuel), expected,
		"the sandbox and the engine should agree on what the machine computes")

	# And that the instruction count above is the real one: with two fewer units
	# of fuel the machine never reaches its OUT, so the trace comes back empty.
	assert_eq(gds.call("run", program, ops - 2), [],
		"the instruction count should be exactly the fuel the program needs")

	print("logic CPU: %d emulated instructions per call, %d outer x %d inner" % [ops, OUTER, LIMIT - 1])

	var in_sandbox := func(): sandbox.vmcallv("run", program, fuel)
	var in_engine := func(): gds.call("run", program, fuel)
	_case("logic CPU dispatch", "SafeGDScript (sandbox)", ops, in_sandbox, "emulated instr")
	_case("logic CPU dispatch", "GDScript (engine)", ops, in_engine, "emulated instr")

	# The same guest through the .sgd loader: a Node with the script attached,
	# which is how the file is reached in a project, and which adds the script
	# instance's own dispatch to every call.
	var node := Node.new()
	node.set_script(load(CPU_SOURCE_PATH))
	node.set_instructions_max(0)
	assert_eq(node.call("run", program, fuel), expected,
		"the .sgd loader should reach the same guest")
	var in_script := func(): node.call("run", program, fuel)
	_case("logic CPU dispatch", "SafeGDScript (.sgd script)", ops, in_script, "emulated instr")

	_measure("logic CPU dispatch")
	print("guest execution mode: %s" % _mode(sandbox))
	_report("logic CPU dispatch")

	node.free()
	sandbox.free()

func test_bench_cpu_single_step():
	# Per-call step: measures entry/decode/dispatch/exit overhead.
	if _elf.is_empty():
		return
	var sandbox := _load_elf(_elf)
	var gds := _as_gdscript(_source)
	if gds == null:
		sandbox.free()
		return

	var program := _make_program()
	var steps := 20000

	# Sanity check: both machines must agree.
	sandbox.vmcallv("reset", program)
	gds.call("reset", program)
	for i in range(64):
		assert_eq(sandbox.vmcallv("step"), gds.call("step"),
			"stepping should agree with the engine at step " + str(i))

	var stepped_in_sandbox := func():
		sandbox.vmcallv("reset", program)
		for i in range(steps):
			sandbox.vmcallv("step")
	var stepped_in_engine := func():
		gds.call("reset", program)
		for i in range(steps):
			gds.call("step")

	# Loop inside the guest, for comparison.
	var looped_in_sandbox := func():
		sandbox.vmcallv("reset", program)
		sandbox.vmcallv("step_until_halted", steps)

	_case("single-instruction step", "SafeGDScript (sandbox)", steps, stepped_in_sandbox, "emulated instr")
	_case("single-instruction step", "GDScript (engine)", steps, stepped_in_engine, "emulated instr")
	_case("single-instruction step", "SafeGDScript (loop in guest)", steps, looped_in_sandbox, "emulated instr")

	_measure("single-instruction step")
	_mode(sandbox)
	_report("single-instruction step")

	sandbox.free()

func test_bench_cpu_call_overhead():
	# encode() is four shifts and three ors: near enough to nothing that the
	# number is the cost of getting into the callee and back. It is the floor
	# under every other row in this suite.
	if _elf.is_empty():
		return
	var sandbox := _load_elf(_elf)
	var gds := _as_gdscript(_source)
	if gds == null:
		sandbox.free()
		return

	var calls := 20000
	assert_eq(sandbox.vmcallv("encode", 3, 1, 2, 7), gds.call("encode", 3, 1, 2, 7),
		"both should encode the same word")

	var via_vmcallv := func():
		for i in range(calls):
			sandbox.vmcallv("encode", 3, 1, 2, 7)
	var via_gdscript := func():
		for i in range(calls):
			gds.call("encode", 3, 1, 2, 7)
	# vmcallable() binds the guest function once, which is what a caller that makes
	# the same call in a loop should be using.
	var bound : Callable = sandbox.vmcallable("encode")
	var via_callable := func():
		for i in range(calls):
			bound.call(3, 1, 2, 7)

	_case("call overhead", "SafeGDScript (sandbox)", calls, via_vmcallv, "call")
	_case("call overhead", "GDScript (engine)", calls, via_gdscript, "call")
	_case("call overhead", "SafeGDScript (vmcallable)", calls, via_callable, "call")

	_measure("call overhead")
	_mode(sandbox)
	_report("call overhead")

	sandbox.free()

func after_all():
	_persist()
