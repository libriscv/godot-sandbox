extends GutTest

var _cases: Array = []

func sgd_paths() -> PackedStringArray:
	return PackedStringArray()

func before_all() -> void:
	_cases = _collect_cases()
	if _cases.is_empty():
		_cases = [{"path": "", "test": ""}]

func test_safegdscript(case = use_parameters(_cases)) -> void:
	var path: String = case["path"]
	if path.is_empty():
		pass_test("No .sgd scripts with @test functions were found.")
		return
	var total: Dictionary = SafeGDScriptLanguage.run_tests(
		PackedStringArray([path]), PackedStringArray([case["test"]]), true)
	var rows: Array = []
	for report in total["scripts"]:
		rows.append_array(report["tests"])
	if rows.is_empty():
		fail_test("%s::%s did not run." % [path.get_file(), case["test"]])
		return
	for row in rows:
		var label: String = "%s::%s" % [path.get_file(), row["name"]]
		if row["status"] == "passed":
			assert_true(true, label)
			continue
		var location: String = row["location"]
		fail_test("%s %s: %s%s" % [label, row["status"], row["message"],
			"" if location.is_empty() else " at " + location])
		var message: String = row["message"]
		if not message.is_empty():
			for error in get_errors():
				if error.contains_text(message):
					error.handled = true

func _collect_cases() -> Array:
	var scripts := PackedStringArray()
	var requested := sgd_paths()
	if requested.is_empty():
		_collect("res://", scripts)
	else:
		for entry in requested:
			if DirAccess.dir_exists_absolute(entry):
				_collect(entry, scripts)
			elif _has_tests(entry):
				scripts.push_back(entry)
	var cases: Array = []
	for path in scripts:
		var script: SafeGDScript = ResourceLoader.load(path, "SafeGDScript")
		if script == null:
			continue
		for name in script.get_test_functions():
			cases.append({"path": path, "test": name})
	return cases

func _collect(directory: String, scripts: PackedStringArray) -> void:
	var dir := DirAccess.open(directory)
	if dir == null:
		return
	dir.list_dir_begin()
	var name := dir.get_next()
	while name != "":
		var path := directory.path_join(name)
		if dir.current_is_dir():
			if name != "." and name != ".." and name != ".godot":
				_collect(path, scripts)
		elif _has_tests(path):
			scripts.push_back(path)
		name = dir.get_next()
	dir.list_dir_end()

func _has_tests(path: String) -> bool:
	if path.get_extension().to_lower() not in ["sgd", "safegd"]:
		return false
	return FileAccess.get_file_as_string(path).contains("@test")
