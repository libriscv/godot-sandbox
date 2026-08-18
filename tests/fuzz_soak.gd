extends SceneTree

# Long-running soak driver for Sandbox.assault(), for use outside the test suite. The
# GUT test in tests/test_fuzz.gd covers the same ground at a size that fits a CI run;
# this is what you reach for when hunting a crash.
#
#   godot --path tests --headless -s fuzz_soak.gd -- --iterations=400000
#   godot --path tests --headless -s fuzz_soak.gd -- --target=syscalls/517 --seed=106567540
#
# The seed is printed before the run starts, so a crash mid-run still names the seed
# that reproduces it. Narrow a failure down with --target=syscalls/<number>, then read
# the coverage table to see which handlers the run actually got into.

const NAMES := {
	500: "PRINT", 501: "VCALL", 502: "VEVAL", 503: "VASSIGN", 504: "GET_OBJ",
	505: "OBJ", 506: "OBJ_CALLP", 507: "GET_NODE", 508: "NODE", 509: "NODE2D",
	510: "NODE3D", 511: "THROW", 512: "IS_EDITOR", 513: "SINCOS", 514: "VEC2_LENGTH",
	515: "VEC2_NORMALIZED", 516: "VEC2_ROTATED", 517: "VCREATE", 518: "VCLONE",
	519: "VFETCH", 520: "VSTORE", 521: "ARRAY_OPS", 522: "ARRAY_AT", 523: "ARRAY_SIZE",
	524: "DICTIONARY_OPS", 525: "STRING_CREATE", 526: "STRING_OPS", 527: "STRING_AT",
	528: "STRING_SIZE", 529: "STRING_APPEND", 530: "TIMER_PERIODIC", 531: "TIMER_STOP",
	532: "NODE_CREATE", 533: "MATH_OP32", 534: "MATH_OP64", 535: "LERP_OP32",
	536: "LERP_OP64", 537: "VEC3_OPS", 538: "CALLABLE_CREATE", 539: "LOAD",
	540: "TRANSFORM_2D_OPS", 541: "TRANSFORM_3D_OPS", 542: "BASIS_OPS", 543: "VEC2_OPS",
	544: "QUAT_OPS", 545: "OBJ_PROP_GET", 546: "OBJ_PROP_SET", 547: "SANDBOX_ADD",
	548: "PACKED_ARRAY_OPS",
}

func _init():
	var iterations := 20000
	var target := "syscalls"
	var fuzz_seed := randi()
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--iterations="):
			iterations = int(a.substr(13))
		elif a.begins_with("--target="):
			target = a.substr(9)
		elif a.begins_with("--seed="):
			fuzz_seed = int(a.substr(7))
	# Printed and flushed before the run, so a crash mid-run still names its seed.
	print("SEED %d TARGET %s ITER %d" % [fuzz_seed, target, iterations])

	var s := Sandbox.new()
	s.set_binary_translation_bg_compilation(false)
	s.set_program(load("res://tests/tests.elf"))
	root.add_child(s)

	var r: Dictionary = s.assault("%s:%d" % [target, fuzz_seed], iterations)
	print("seed=%s iterations=%s exceptions=%s" % [r.get("seed"), r.get("iterations"), r.get("exceptions")])
	var cov: Dictionary = r.get("coverage", {})
	var dead := []
	for k in cov.keys():
		var c: Array = cov[k]
		var name: String = NAMES.get(k, str(k))
		if c[1] == 0:
			dead.append(name)
		print("  %-4d %-18s invoked=%-6d returned=%-6d" % [k, name, c[0], c[1]])
	if not dead.is_empty():
		print("NEVER RETURNED: ", ", ".join(dead))
	quit()
