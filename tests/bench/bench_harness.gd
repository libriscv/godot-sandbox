# Shared harness for the benchmark suite.
#
# The suite lives outside res://tests/, which is what .gutconfig points GUT at,
# so nothing here runs in CI: benchmarks measure the machine they run on, and a
# number that moves with the runner is not a test result. Run them with
# tests/run_benchmarks.sh.
#
# A bench file registers every implementation of the same work with _case(),
# calls _measure() to time them, and _report() to print the group. Every case in
# a group must do the same number of work units, so that the ns/op column
# compares.
#
# Three things keep the numbers still enough to publish:
#
#   - A sample is repeated until it lasts at least _min_sample_usec. Below a few
#     tens of milliseconds the reading is the timer and the scheduler rather
#     than the code, and no amount of averaging recovers it.
#   - Samples are taken in interleaved rounds -- every case once, then every
#     case again -- so that frequency scaling and thermal drift over the group
#     land on all cases equally instead of on whichever ran last.
#   - Each case reports min, P50 and P90 rather than a single number, so the
#     spread is visible, and keeps its individual samples so that several runs
#     of the suite can be pooled into one distribution. Sampling within a
#     process is stable to about a percent; process-to-process layout is worth
#     ten times that on some kernels, and only repeated runs can see it.
extends GutTest

const RESULTS_DIR := "res://bench/results"

# The case every group is compared against, unless _report() is told otherwise.
const REFERENCE := "GDScript (engine)"

# An execute segment that libriscv has already translated is cached and handed to
# the next load of the same binary, so one process measures one execution mode:
# a guest loaded with the JIT off makes every later load of that ELF interpreted
# too. The interpreter is therefore a whole run of the suite -- GDSC_BENCH_NO_JIT,
# which run_benchmarks.sh sets for --no-jit -- and it keeps its own results file,
# which bench_report.py reads beside the JIT one to build the comparison tables.
var _no_jit := OS.get_environment("GDSC_BENCH_NO_JIT") == "1"

# Set by the runner when it is going to run the suite more than once: the
# per-group table below is the view for someone watching a single run, and
# repeating it six times buries the report that is the actual output.
var _quiet := OS.get_environment("GDSC_BENCH_QUIET") == "1"

# Recorded rounds per case. Enough that P90 is a rank rather than an
# extrapolation: with 12 samples it is the second-slowest.
var _samples := maxi(5, _env_int("GDSC_BENCH_SAMPLES", 12))

# How long one sample must last. A case whose single call is faster than this is
# called repeatedly inside the sample until it is not.
var _min_sample_usec := maxi(1000, _env_int("GDSC_BENCH_MIN_SAMPLE_US", 30000))

# A case whose one call already exceeds this is timed as-is and said so: nothing
# can be done about it here except spend less time in the suite.
const SLOW_CALL_USEC := 2000000

# Execution mode of the guest, as observed rather than as requested. Set by
# _mode(), which every bench file calls once per group.
var _run_mode := ""

# group -> { unit, cases, order, notes, reference }
var _groups := {}
var _group_order : Array[String] = []

# Cases registered but not yet measured, per group.
var _pending := {}

func _env_int(name: String, fallback: int) -> int:
	var raw := OS.get_environment(name)
	return int(raw) if raw.is_valid_int() else fallback

func _results_path() -> String:
	return RESULTS_DIR + ("/latest-nojit.json" if _no_jit else "/latest.json")

func _baseline_path() -> String:
	return "res://bench/" + ("baseline-nojit.json" if _no_jit else "baseline.json")

# Register one implementation. `fn` performs `ops` units of work per call; the
# harness decides how many times to call it per sample.
func _case(group: String, label: String, ops: int, fn: Callable, unit: String = "op") -> void:
	if not _groups.has(group):
		_groups[group] = {"unit": unit, "cases": {}, "order": [], "notes": {}, "reference": REFERENCE}
		_group_order.append(group)
		_pending[group] = []
	_pending[group].append({"label": label, "ops": ops, "fn": fn})

# Time every registered case of a group.
func _measure(group: String) -> void:
	if not _pending.has(group) or _pending[group].is_empty():
		return
	var cases : Array = _pending[group]
	_pending[group] = []

	# Calibration doubles as the warmup: by the time a case has run long enough
	# to be timed, the sandbox has finished translating the guest in the
	# background and every code path has been through the branch predictor once.
	for c in cases:
		c["reps"] = _calibrate(c["fn"], c["label"])
		c["samples"] = []

	# Interleaved rounds, not case-at-a-time.
	for round_index in range(_samples):
		for c in cases:
			var reps : int = c["reps"]
			var fn : Callable = c["fn"]
			var t0 := Time.get_ticks_usec()
			for r in range(reps):
				fn.call()
			var t1 := Time.get_ticks_usec()
			var per_op := float(t1 - t0) * 1000.0 / float(c["ops"] * reps)
			c["samples"].append(per_op)

	for c in cases:
		var s : Array = c["samples"]
		s.sort()
		var p50 := _percentile(s, 0.50)
		var p90 := _percentile(s, 0.90)
		_groups[group]["cases"][c["label"]] = {
			"min": s[0],
			"p50": p50,
			"p90": p90,
			# What the row costs in trust: how far the slow tail sits above the
			# middle, as a fraction of the middle.
			"spread": (p90 - p50) / p50 if p50 > 0.0 else 0.0,
			"reps": c["reps"],
			"ops": c["ops"],
			"samples": s.size(),
			# Kept so that repeated runs can be pooled: sampling inside one
			# process is stable to about a percent, but where the loader and the
			# allocator put things moves some kernels by ten times that, and a
			# P90 taken from a single process cannot see it. bench_report.py
			# pools these across runs.
			"samples_ns": s,
		}
		_groups[group]["order"].append(c["label"])

# Grow the repeat count until one sample lasts _min_sample_usec.
func _calibrate(fn: Callable, label: String) -> int:
	var reps := 1
	for attempt in range(24):
		var t0 := Time.get_ticks_usec()
		for r in range(reps):
			fn.call()
		var elapsed := Time.get_ticks_usec() - t0
		if elapsed >= _min_sample_usec:
			if reps == 1 and elapsed > SLOW_CALL_USEC:
				print("(one call of '%s' takes %.0f ms; the group is slow by construction)" % [label, elapsed / 1000.0])
			break
		if reps >= 1 << 22:
			break
		if elapsed <= 0:
			reps *= 16
		else:
			# Aim slightly past the target so this converges in two steps.
			var scaled := int(ceil(float(reps) * float(_min_sample_usec) / float(elapsed) * 1.15))
			reps = clampi(scaled, reps + 1, reps * 32)
	return reps

# Linear interpolation between ranks, on an already sorted array.
func _percentile(sorted_samples: Array, p: float) -> float:
	if sorted_samples.is_empty():
		return 0.0
	if sorted_samples.size() == 1:
		return sorted_samples[0]
	var idx := p * float(sorted_samples.size() - 1)
	var lo := int(floor(idx))
	var hi := int(ceil(idx))
	var t := idx - float(lo)
	return float(sorted_samples[lo]) * (1.0 - t) + float(sorted_samples[hi]) * t

# A note recorded beside a group's numbers: the n a kernel was given, mostly.
# Which execution mode the guest ran in is a property of the whole run and is
# recorded once, in the environment.
func _note(group: String, key: String, value) -> void:
	if _groups.has(group):
		_groups[group]["notes"][key] = value

func _mode(s: Sandbox) -> String:
	var m := "interpreter"
	if s.is_jit():
		m = "JIT"
	elif s.is_binary_translated():
		m = "binary translated"
	_run_mode = m
	return m

func _environment() -> Dictionary:
	return {
		"godot": Engine.get_version_info()["string"],
		"cpu": OS.get_processor_name(),
		"mode": _run_mode if _run_mode != "" else ("interpreter" if _no_jit else "JIT"),
		"jit_available": Sandbox.has_feature_jit(),
		"jit_enabled": Sandbox.is_jit_enabled(),
		"jit_requested": not _no_jit,
		"binary_translation_available": Sandbox.has_feature_binary_translation(),
		"samples": _samples,
		"min_sample_usec": _min_sample_usec,
		"cpus": OS.get_environment("GDSC_BENCH_CPUS"),
	}

# Print one group as a table. This is the per-run view, for someone watching a
# single run; the tables the suite exists to produce are built afterwards by
# bench_report.py from both runs' results files.
func _report(group: String, reference: String = REFERENCE) -> void:
	if not _groups.has(group):
		return
	_groups[group]["reference"] = reference
	if _quiet:
		return
	var cases : Dictionary = _groups[group]["cases"]
	var unit : String = _groups[group]["unit"]
	var base_cases := _comparable_baseline(group)

	print("")
	print("--- %s (%s) ---" % [group, _run_mode if _run_mode != "" else "?"])
	print("%-34s %10s %10s %10s %8s %8s %9s" % ["case", "min", "p50", "p90", "spread", "vs ref", "vs base"])
	var ref_ns : float = cases[reference]["p50"] if cases.has(reference) else 0.0
	for label in _groups[group]["order"]:
		var st : Dictionary = cases[label]
		var vs_ref := "-"
		if ref_ns > 0.0 and st["p50"] > 0.0:
			vs_ref = "%.2fx" % (ref_ns / st["p50"])
		var vs_base := "-"
		if base_cases.has(label):
			var b : float = _base_p50(base_cases[label])
			if b > 0.0:
				vs_base = "%+.1f%%" % ((st["p50"] - b) / b * 100.0)
		print("%-34s %10.1f %10.1f %10.1f %7.1f%% %8s %9s" % [
			label, st["min"], st["p50"], st["p90"], st["spread"] * 100.0, vs_ref, vs_base])
	print("(ns/%s; vs ref: higher is faster than '%s'; vs base: negative is faster)" % [unit, reference])

# The saved baseline's cases for this group, or nothing when the baseline does
# not describe the same measurement -- another machine, or a run whose guest
# executed in a different mode.
func _comparable_baseline(group: String) -> Dictionary:
	var baseline := _load_json(_baseline_path())
	var groups : Dictionary = baseline.get("groups", {})
	if not groups.has(group):
		return {}
	var base_env : Dictionary = baseline.get("_environment", {})
	if base_env.get("cpu", "") != OS.get_processor_name():
		print("(baseline was measured on '%s', this machine is '%s' -- not compared)" % [base_env.get("cpu", "?"), OS.get_processor_name()])
		return {}
	if _run_mode != "" and base_env.get("mode", "") != _run_mode:
		print("(baseline ran in '%s' mode, this run in '%s' -- not compared)" % [base_env.get("mode", "?"), _run_mode])
		return {}
	return groups[group].get("cases", {})

func _base_p50(entry) -> float:
	# Baselines written by the pre-percentile harness held a bare number.
	if typeof(entry) == TYPE_DICTIONARY:
		return float(entry.get("p50", 0.0))
	if typeof(entry) == TYPE_FLOAT or typeof(entry) == TYPE_INT:
		return float(entry)
	return 0.0

# Merge this file's groups into the run's results file. Each bench file is its
# own GUT script, so the file is read back and updated rather than rewritten,
# and every group carries the time it was measured -- a results file may hold
# groups from more than one run.
func _persist() -> void:
	if _groups.is_empty():
		return
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(RESULTS_DIR))
	var all := _load_json(_results_path())
	all["_environment"] = _environment()
	if not all.has("groups"):
		all["groups"] = {}
	var stamp := Time.get_datetime_string_from_system()
	for group in _group_order:
		var entry : Dictionary = _groups[group].duplicate(true)
		entry["measured"] = stamp
		all["groups"][group] = entry
	var file := FileAccess.open(_results_path(), FileAccess.WRITE)
	if file == null:
		print("bench: could not write %s" % _results_path())
		return
	file.store_string(JSON.stringify(all, "\t", true))
	file.close()

func _load_json(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		return {}
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	var text := file.get_as_text()
	file.close()
	var parsed = JSON.parse_string(text)
	if typeof(parsed) != TYPE_DICTIONARY:
		return {}
	return parsed

# Compile GDScript source the way a user reaches the compiler: inside a Sandbox
# running the compiler ELF. Once per bench file, not once per case -- compiling
# per case would measure the compiler as much as the guest.
func _compile(source: String) -> PackedByteArray:
	var compiler : Sandbox = Sandbox.new()
	compiler.set_binary_translation_bg_compilation(false)
	compiler.set_program(load("res://tests/tests.elf"))
	compiler.restrictions = true
	var elf : PackedByteArray = compiler.vmcall("compile_to_elf", source)
	compiler.free()
	assert_false(elf.is_empty(), "compilation should succeed")
	return elf

func _load_elf(elf: PackedByteArray) -> Sandbox:
	if _no_jit:
		Sandbox.set_jit_enabled(false)
	var s := Sandbox.new()
	# Translate at load, not on a background thread while the first samples are
	# being taken: background compilation live-patches the decoder cache under a
	# running guest, which makes the early rounds a mix of interpreted and
	# translated code and leaves a bimodal tail in the numbers. It also makes
	# is_jit() true for a segment that is merely on its way to being compiled.
	s.set_binary_translation_bg_compilation(false)
	s.load_buffer(elf)
	s.set_instructions_max(0)  # unlimited: a benchmark is not a timeout test
	# The compiler emits the Variant ABI: every argument arrives as a pointer to
	# a Variant, and a guest handed an unboxed integer faults reading it. vmcallv()
	# forces this per call; vmcallable() and a Callable bound from it do not, so
	# the sandbox itself is put in that mode.
	s.set_unboxed_arguments(false)
	return s

# The same source as a GDScript object, compiled by the engine at run time. Both
# sides of every comparison in this suite come from one source string, so a
# difference in the numbers cannot be a difference in the programs.
func _as_gdscript(source: String) -> Object:
	var script := GDScript.new()
	script.source_code = source
	var err := script.reload()
	assert_eq(err, OK, "the same source should compile as GDScript")
	if err != OK:
		return null
	return script.new()
