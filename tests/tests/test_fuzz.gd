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
const SYSCALL_LAST := 555

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
	545: "OBJ_PROP_GET",
	546: "OBJ_PROP_SET",
}

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
	for syscall in range(500, SYSCALL_LAST):
		if syscall in never_returns:
			continue
		var counts: Array = coverage.get(syscall, [0, 0])
		assert_gt(counts[1], 0,
			"system call %d was reached past its argument validation" % syscall)

func test_fuzz_syscalls_respect_restrictions():
	var s := _make_sandbox()
	var r: Dictionary = s.assault("syscalls", ITERATIONS)
	var coverage: Dictionary = r["coverage"]

	for syscall in ALWAYS_REFUSED:
		var counts: Array = coverage.get(syscall, [0, 0])
		assert_eq(counts[1], 0,
			"%s reached Godot %d times with all restrictions enabled" % [ALWAYS_REFUSED[syscall], counts[1]])
