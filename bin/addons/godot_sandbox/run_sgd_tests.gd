extends SceneTree
## Headless runner for the `@test` functions of a project's `.sgd` scripts.
##
##     godot --headless --path . -s addons/godot_sandbox/run_sgd_tests.gd -- [options] [paths]
##
## With no paths it runs every `.sgd` file that mentions `@test`. The exit code
## is the number of failures plus errors (capped at 125).

const EXIT_CODE_CAP := 125

const USAGE := """Usage: godot --headless --path . -s addons/godot_sandbox/run_sgd_tests.gd -- [options] [paths]

Runs every @test function of the given .sgd scripts. A path may be a script or a
directory; with no path the whole project is walked. The exit code is the number
of failures plus errors, capped at %d.

Options:
  --only <name>     Run only the named @test. Repeatable.
  --list            List the scripts and their @test functions, then exit.
  --json            Write the result report to stdout as JSON, then exit.
  --junit <path>    Write a JUnit XML report to <path>.
  --fail-fast       Stop after the first script that fails.
  --help, -h        Show this text.""" % EXIT_CODE_CAP

var only := PackedStringArray()
var junit_path := ""
var list_only := false
var as_json := false
var fail_fast := false

func _init() -> void:
	# A one-shot headless process exits while a background translation is still
	# compiling, and the teardown crash reads as a failure of the tests.
	ProjectSettings.set_setting("sandbox/binary_translation/auto_bake", false)

	var paths := PackedStringArray()
	if not _parse_arguments(paths):
		quit(2)
		return
	if paths.is_empty():
		if not as_json:
			print("No .sgd scripts with @test functions found.")
		else:
			print(JSON.stringify(_empty_report(), "\t"))
		_write_junit(_empty_report())
		quit(0)
		return
	if list_only:
		_list(paths)
		quit(0)
		return

	var total := _run(paths)
	var failed: int = int(total["failed"]) + int(total["errors"])
	if as_json:
		print(JSON.stringify(total, "\t"))
	else:
		print("%d passed, %d failed, %d errors across %d script(s)" % [
			total["passed"], total["failed"], total["errors"], total["scripts"].size()])
	_write_junit(total)
	quit(mini(failed, EXIT_CODE_CAP))

func _run(paths: PackedStringArray) -> Dictionary:
	if not fail_fast:
		return SafeGDScriptLanguage.run_tests(paths, only, as_json)
	var total := _empty_report()
	for path in paths:
		var one: Dictionary = SafeGDScriptLanguage.run_tests(
			PackedStringArray([path]), only, as_json)
		total["scripts"].append_array(one["scripts"])
		total["passed"] = int(total["passed"]) + int(one["passed"])
		total["failed"] = int(total["failed"]) + int(one["failed"])
		total["errors"] = int(total["errors"]) + int(one["errors"])
		if int(one["failed"]) + int(one["errors"]) > 0:
			break
	return total

func _empty_report() -> Dictionary:
	return {"scripts": [], "passed": 0, "failed": 0, "errors": 0}

func _list(paths: PackedStringArray) -> void:
	var listed := {}
	for path in paths:
		var script: SafeGDScript = ResourceLoader.load(path, "SafeGDScript")
		var names := script.get_test_functions() if script != null else PackedStringArray()
		listed[path] = names
		if not as_json:
			print(path)
			for name in names:
				if only.is_empty() or only.has(name):
					print("  %s" % name)
	if as_json:
		print(JSON.stringify(listed, "\t"))

func _parse_arguments(paths: PackedStringArray) -> bool:
	var requested := OS.get_cmdline_user_args()
	var index := 0
	var explicit := PackedStringArray()
	while index < requested.size():
		var argument: String = requested[index]
		index += 1
		match argument:
			"--help", "-h":
				print(USAGE)
				quit(0)
				return false
			"--list":
				list_only = true
			"--json":
				as_json = true
			"--fail-fast":
				fail_fast = true
			"--only", "--junit":
				if index >= requested.size():
					printerr("%s needs a value." % argument)
					print(USAGE)
					return false
				if argument == "--only":
					only.push_back(requested[index])
				else:
					junit_path = requested[index]
				index += 1
			_:
				if argument.begins_with("-"):
					printerr("Unknown option: %s" % argument)
					print(USAGE)
					return false
				explicit.push_back(argument)
	if explicit.is_empty():
		_collect("res://", paths)
		return true
	for entry in explicit:
		var path: String = entry if entry.begins_with("res://") else "res://" + entry
		if DirAccess.dir_exists_absolute(path):
			_collect(path, paths)
		elif _has_tests(path):
			paths.push_back(path)
		else:
			push_warning("Skipping %s: not a .sgd script with @test functions." % path)
	return true

func _collect(directory: String, paths: PackedStringArray) -> void:
	var dir := DirAccess.open(directory)
	if dir == null:
		return
	dir.list_dir_begin()
	var name := dir.get_next()
	while name != "":
		var path := directory.path_join(name)
		if dir.current_is_dir():
			# Godot's own import cache holds no source.
			if name != "." and name != ".." and name != ".godot":
				_collect(path, paths)
		elif _has_tests(path):
			paths.push_back(path)
		name = dir.get_next()
	dir.list_dir_end()

## Textual, like the editor's context menu: finding tests must not compile.
func _has_tests(path: String) -> bool:
	if path.get_extension().to_lower() not in ["sgd", "safegd"]:
		return false
	var source := FileAccess.get_file_as_string(path)
	return source.contains("@test")

func _write_junit(total: Dictionary) -> void:
	if junit_path.is_empty():
		return
	var out := FileAccess.open(junit_path, FileAccess.WRITE)
	if out == null:
		printerr("Cannot write %s: %s" % [junit_path, error_string(FileAccess.get_open_error())])
		return
	var scripts: Array = total["scripts"]
	var cases := 0
	for report in scripts:
		cases += (report["tests"] as Array).size()
	out.store_line('<?xml version="1.0" encoding="UTF-8"?>')
	out.store_line('<testsuites name="SafeGDScript" tests="%d" failures="%d" errors="%d">' % [
		cases, total["failed"], total["errors"]])
	for report in scripts:
		var rows: Array = report["tests"]
		out.store_line('\t<testsuite name="%s" tests="%d" failures="%d" errors="%d" time="%s">' % [
			_xml(report["path"]), rows.size(), report["failed"], report["errors"],
			_seconds(report["elapsed_usec"])])
		for row in rows:
			out.store_line('\t\t<testcase name="%s" classname="%s" line="%d" time="%s">' % [
				_xml(row["name"]), _xml(report["path"]), row["line"],
				_seconds(row["elapsed_usec"])])
			if row["status"] != "passed":
				var tag := "failure" if row["status"] == "failed" else "error"
				out.store_line('\t\t\t<%s message="%s">%s</%s>' % [
					tag, _xml(row["message"]), _xml(row["location"]), tag])
			out.store_line('\t\t</testcase>')
		out.store_line('\t</testsuite>')
	out.store_line('</testsuites>')

func _seconds(usec: int) -> String:
	return "%.3f" % (float(usec) / 1000000.0)

func _xml(text: String) -> String:
	return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;") \
		.replace('"', "&quot;").replace("'", "&apos;")
