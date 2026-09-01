extends GutTest

# The system call handlers are parsers of eight attacker-controlled machine words, and
# Sandbox.assault() drives them directly with hostile register values. The pass condition
# is that the process is still alive afterwards; the counters are there to prove the run
# actually reached the handlers rather than bailing out somewhere early.
#
# Keep the iteration count modest here. For a real soak, call assault() with a few hundred
# thousand iterations, and pass the reported seed back as "syscalls:<seed>" to replay a
# failure, or "syscalls/<number>:<seed>" to narrow it to one system call.

var Sandbox_TestsTests = load("res://tests/tests.elf")

const ITERATIONS := 50000

# ECALL_LAST. Every handler below it is driven and checked; raising the syscall
# range without raising this leaves the new one unchecked.
const SYSCALL_LAST := 565

# Restrictions are enabled for the duration of a run and every callback answers "no", so
# these are the system calls that are supposed to be refused every single time. A handler
# that starts returning from this list has lost its restriction check.
const ALWAYS_REFUSED := {
	505: "OBJ",
	506: "OBJ_CALLP",
	508: "NODE",
	509: "NODE2D",
	510: "NODE3D",
	530: "TIMER_PERIODIC",
	532: "NODE_CREATE",
	539: "LOAD",
}

# OBJ_PROP_GET/SET are not in the list above, and the distinction is the point:
# they carry two things. On an object handle they are an object capability, and
# a restricted guest never gets a handle to begin with -- every syscall that
# hands one out is in ALWAYS_REFUSED. On anything else they are a built-in's own
# member (`transform.origin`, `basis.x`), which is value arithmetic: no object,
# no engine state, nothing the allowlist has an opinion about. So under
# restrictions these two do reach Godot, for the half that was never a
# capability.

func _make_sandbox() -> Sandbox:
	var s := Sandbox.new()
	# Background translation would still be compiling when the test tears the sandbox down.
	s.set_binary_translation_bg_compilation(false)
	s.set_program(Sandbox_TestsTests)
	add_child_autofree(s)
	return s

func test_fuzz_guest_variants():
	var s := _make_sandbox()
	var r: Dictionary = s.assault("variants", ITERATIONS)
	assert_eq(r["iterations"], ITERATIONS, "every GuestVariant was converted")
	assert_gt(r["exceptions"], 0, "hostile GuestVariants are rejected, not accepted")

func test_fuzz_syscalls():
	var s := _make_sandbox()
	var r: Dictionary = s.assault("syscalls", ITERATIONS)
	assert_eq(r["iterations"], ITERATIONS, "every system call was driven")

	var coverage: Dictionary = r["coverage"]
	assert_gt(coverage.size(), 40, "the run reached almost every system call")

	# Seed %s is printed so a failure below can be replayed with "syscalls:<seed>".
	gut.p("fuzzing seed: %s" % r["seed"])

	# Every handler has to be reached, or the run proves nothing about it. The
	# bound is ECALL_LAST: a system call added without extending it here is one
	# the fuzzer drives and nothing checks.
	for syscall in range(500, SYSCALL_LAST):
		assert_true(coverage.has(syscall), "system call %d was invoked" % syscall)

	# Being invoked is not the same as being tested. A handler validates its arguments
	# before it does anything, and a fuzzer whose arguments never agree with each other
	# spends the whole run in the first three lines of every one of them -- thousands of
	# invocations, nothing reached. So the run also has to show that each handler returned
	# at least once, which it only does when the arguments got past the validation.
	#
	# The exceptions are the handlers that are supposed to refuse everything here:
	# ALWAYS_REFUSED is refused by the restrictions, THROW throws by definition, and
	# TIMER_STOP is not implemented.
	var never_returns := ALWAYS_REFUSED.keys()
	never_returns.append(511) # THROW
	never_returns.append(531) # TIMER_STOP
	# AWAIT_RESTORE only means anything inside a resume, which is a call the host makes
	# and not something a guest can ask for. Its guest-controlled arguments -- where to
	# put the frame and how big it is -- are refused here every time, which is the point;
	# the blob it copies is the host's own, and the guest bytes that become one are
	# consumed by AWAIT, which this run does reach.
	never_returns.append(553) # AWAIT_RESTORE
	# OBJ_PROP_GET/SET reach Godot only for the built-in-member half (see below),
	# and only when a random name happens to be a member the random type has.
	# That is a coin the fuzzer flips, not something a run can be asked to hit.
	never_returns.append(545) # OBJ_PROP_GET
	never_returns.append(546) # OBJ_PROP_SET
	# CLASS_BIND is initialization-only, and VCALL_SUPER needs a legitimate object
	# plus call-site metadata. Hostile registers cannot satisfy either contract.
	never_returns.append(560) # CLASS_BIND
	never_returns.append(561) # VCALL_SUPER
	# ARRAY_BATCH needs a valid scoped Array and a writable sixteen-Variant guest
	# buffer at once; hostile independent registers cannot satisfy that contract.
	never_returns.append(564) # ARRAY_BATCH
	for syscall in range(500, SYSCALL_LAST):
		if syscall in never_returns:
			continue
		var counts: Array = coverage.get(syscall, [0, 0])
		assert_gt(counts[1], 0,
			"system call %d was reached past its argument validation" % syscall)

# The assault cannot tell the two halves of OBJ_PROP_GET/SET apart -- it drives
# raw registers and counts returns -- so the half that is a capability is pinned
# here instead, deterministically: a restricted guest holding a legitimate handle
# still cannot read or write a property through it, and the built-in half it
# shares a system call with keeps working.
func test_object_properties_stay_refused_under_restrictions():
	var source := """
func builtin_member():
	var t = Transform2D()
	t.origin = Vector2(3, 4)
	return t.origin

func read(n):
	return n.name

func read_indexed(n):
	return n["name"]

func write(n):
	n.name = "Renamed"
"""
	var compiler := _make_sandbox()
	var elf: PackedByteArray = compiler.vmcall("compile_to_elf", source)
	assert_false(elf.is_empty(), "the probe program should compile")
	if elf.is_empty():
		return

	var target := Node.new()
	target.name = "Target"
	add_child_autofree(target)

	var s := Sandbox.new()
	s.load_buffer(elf)
	s.set_instructions_max(40000000)
	add_child_autofree(s)

	# Unrestricted first, so a refusal below is the restrictions and not the program.
	assert_eq(s.vmcallv("read", target), "Target", "the property reads before restrictions")
	assert_eq(s.vmcallv("read_indexed", target), "Target",
		"the indexed property reads before restrictions")

	s.restrictions = true
	var before := s.get_exceptions()
	assert_eq(s.vmcallv("builtin_member"), Vector2(3, 4),
		"a built-in's own member is value arithmetic and stays reachable")
	assert_eq(s.get_exceptions(), before, "and raises nothing")

	before = s.get_exceptions()
	assert_eq(s.vmcallv("read", target), null,
		"reading an object property is refused however the guest got the handle")
	assert_eq(s.get_exceptions(), before + 1, "the read raised")
	assert_engine_error("Banned property accessed: name")
	assert_engine_error("Exception: Banned property accessed: name")

	before = s.get_exceptions()
	assert_eq(s.vmcallv("read_indexed", target), null,
		"indexed object access passes through the same property restriction")
	assert_eq(s.get_exceptions(), before + 1, "the indexed read raised")
	assert_engine_error("Banned property accessed through Variant index")
	assert_engine_error("Exception: Banned property accessed through Variant index")

	before = s.get_exceptions()
	s.vmcallv("write", target)
	assert_eq(s.get_exceptions(), before + 1, "the write raised")
	assert_engine_error("Banned property set: name")
	assert_engine_error("Exception: Banned property set: name")
	assert_eq(target.name, "Target", "and the property never changed")

func test_fuzz_syscalls_respect_restrictions():
	var s := _make_sandbox()
	var r: Dictionary = s.assault("syscalls", ITERATIONS)
	var coverage: Dictionary = r["coverage"]

	for syscall in ALWAYS_REFUSED:
		var counts: Array = coverage.get(syscall, [0, 0])
		assert_eq(counts[1], 0,
			"%s reached Godot %d times with all restrictions enabled" % [ALWAYS_REFUSED[syscall], counts[1]])
