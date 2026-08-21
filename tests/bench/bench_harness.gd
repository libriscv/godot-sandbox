# Shared harness for the benchmark suite.
#
# The suite lives outside res://tests/, which is what .gutconfig points GUT at,
# so nothing here runs in CI: benchmarks measure the machine they run on, and a
# number that moves with the runner is not a test result. Run them with
# tests/run_benchmarks.sh.
#
# A bench file extends this script and calls _bench() once per implementation of
# the same work, then _report() to print the group. Every case in a group must do
# the same number of work units, so that the ns/op column compares.
extends GutTest

const RESULTS_DIR := "res://bench/results"

# An execute segment that libriscv has already translated is cached and handed to
# the next load of the same binary, so one process measures one execution mode:
# a guest loaded with the JIT off makes every later load of that ELF interpreted
# too. The interpreter is therefore a whole run of the suite -- GDSC_BENCH_NO_JIT,
# which run_benchmarks.sh sets for --no-jit -- and it keeps its own results and
# its own baseline, so the two are never subtracted from each other.
var _no_jit := OS.get_environment("GDSC_BENCH_NO_JIT") == "1"

func _results_path() -> String:
	return RESULTS_DIR + ("/latest-nojit.json" if _no_jit else "/latest.json")

func _baseline_path() -> String:
	return "res://bench/" + ("baseline-nojit.json" if _no_jit else "baseline.json")

# The best of several samples, not the mean: the mean measures the scheduler as
# much as the code, while the minimum is a stable estimate of the work itself.
const SAMPLES := 5

# group -> { "unit": String, "cases": { label -> ns_per_op } }
var _groups := {}
var _group_order : Array[String] = []

func _bench(group: String, label: String, ops: int, fn: Callable, unit: String = "op") -> float:
	# `fn` performs `ops` units of work per call. It is called once to warm up --
	# the sandbox translates the guest in the background, so the first call of a
	# case is the only one that may run interpreted -- and then SAMPLES times.
	fn.call()
	var best := 1e30
	for sample in range(SAMPLES):
		var t0 := Time.get_ticks_usec()
		fn.call()
		var t1 := Time.get_ticks_usec()
		best = min(best, float(t1 - t0))
	var ns := best * 1000.0 / float(ops)
	if not _groups.has(group):
		_groups[group] = {"unit": unit, "cases": {}}
		_group_order.append(group)
	_groups[group]["cases"][label] = ns
	return ns

# A note recorded beside a group's numbers: which execution mode the guest ran
# in, mostly. Two runs are only comparable if this matches.
func _note(group: String, key: String, value) -> void:
	if _groups.has(group):
		_groups[group][key] = value

func _mode(s: Sandbox) -> String:
	if s.is_jit():
		return "JIT"
	if s.is_binary_translated():
		return "binary translated"
	return "interpreter"

func _environment() -> Dictionary:
	return {
		"godot": Engine.get_version_info()["string"],
		"cpu": OS.get_processor_name(),
		"jit_available": Sandbox.has_feature_jit(),
		"jit_enabled": Sandbox.is_jit_enabled(),
		"jit_requested": not _no_jit,
		"binary_translation_available": Sandbox.has_feature_binary_translation(),
	}

# Print one group as a table. `reference` names the case everything is compared
# against -- GDScript, for the comparisons this suite exists to make -- and the
# baseline column is this machine's own last saved run, which is the only way a
# number here means anything over time.
func _report(group: String, reference: String = "") -> void:
	if not _groups.has(group):
		return
	var cases : Dictionary = _groups[group]["cases"]
	var unit : String = _groups[group]["unit"]
	var baseline := _load_json(_baseline_path())
	var base_cases := {}
	if baseline.has(group):
		base_cases = baseline[group].get("cases", {})
	# A baseline from another machine, or from a run where the guest ran in a
	# different mode, compares two different things.
	var comparable := true
	if not base_cases.is_empty():
		var base_env : Dictionary = baseline.get("_environment", {})
		if base_env.get("cpu", "") != OS.get_processor_name():
			print("(baseline was measured on '%s', this machine is '%s')" % [base_env.get("cpu", "?"), OS.get_processor_name()])
			comparable = false
		if baseline[group].get("mode", "") != _groups[group].get("mode", ""):
			print("(baseline ran in '%s' mode, this run in '%s')" % [baseline[group].get("mode", "?"), _groups[group].get("mode", "?")])
			comparable = false
	if not comparable:
		print("(the 'vs base' column below is not a like-for-like comparison)")

	print("")
	print("--- %s ---" % group)
	print("%-34s %12s %10s %10s" % ["case", "ns/" + unit, "vs ref", "vs base"])
	var ref_ns : float = cases.get(reference, 0.0)
	for label in cases:
		var ns : float = cases[label]
		var vs_ref := "-"
		if ref_ns > 0.0:
			vs_ref = "%.2fx" % (ref_ns / ns)
		var vs_base := "-"
		if base_cases.has(label):
			var b : float = base_cases[label]
			if b > 0.0:
				vs_base = "%+.1f%%" % ((ns - b) / b * 100.0)
		print("%-34s %12.1f %10s %10s" % [label, ns, vs_ref, vs_base])
	if ref_ns > 0.0:
		print("(vs ref: higher is faster than '%s')" % reference)

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
	var stamp := Time.get_datetime_string_from_system()
	for group in _group_order:
		var entry : Dictionary = _groups[group].duplicate(true)
		entry["measured"] = stamp
		all[group] = entry
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
