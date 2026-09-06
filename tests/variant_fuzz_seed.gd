#   godot --headless --path . -s variant_fuzz_seed.gd -- 21
#   godot --headless --path . -s variant_fuzz_seed.gd -- 21 --source
extends SceneTree

const VariantFuzz = preload("res://tests/variant_fuzz.gd")


func _init() -> void:
	var arguments := OS.get_cmdline_user_args()
	var seed_value := 0
	var source_only := false
	for argument in arguments:
		if argument == "--source":
			source_only = true
		elif argument.is_valid_int():
			seed_value = argument.to_int()

	var table := VariantFuzz.load_table()
	if table.is_empty():
		print("could not load ", VariantFuzz.TABLE_PATH)
		quit(1)
		return

	var program: Dictionary = VariantFuzz.new(seed_value, table).generate()
	print(program["source"])
	if source_only:
		quit()
		return

	var engine := VariantFuzz.compile_engine(program["source"])
	if not engine["ok"]:
		print("GDScript refused the program: ", engine["error"])
		quit(1)
		return
	var sgd := VariantFuzz.compile_sgd(program["source"])
	if not sgd["ok"]:
		print("the compiler refused the program: ", sgd["error"])
		quit(1)
		return

	print("-- seed %d --" % seed_value)
	for name in program["functions"]:
		var expected = engine["object"].callv(name, [])
		var actual = sgd["object"].callv(name, [])
		var difference := VariantFuzz.difference(expected, actual)
		if difference == "":
			print("%s  same: %s" % [name, expected])
		else:
			print("%s  DIFFERS (%s)\n    GDScript: %s\n    compiled: %s"
				% [name, difference, expected, actual])
	VariantFuzz.release(sgd)
	quit()
