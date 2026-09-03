extends SceneTree
## Headless runner for the `@test` functions of a project's `.sgd` scripts.
##
##     godot --headless --path . -s addons/godot_sandbox/run_sgd_tests.gd -- [paths]
##
## With no paths it walks `res://` for `.sgd` files that mention `@test`. Each
## path may be a script or a directory. The exit code is the number of failures
## plus errors, capped at 125, so CI fails on the count without parsing output.

const EXIT_CODE_CAP := 125

func _init() -> void:
	# A one-shot headless process exits while a background translation is still
	# compiling, and the teardown crash reads as a failure of the tests.
	ProjectSettings.set_setting("sandbox/binary_translation/auto_bake", false)

	var paths := _requested_paths()
	if paths.is_empty():
		print("No .sgd scripts with @test functions found.")
		quit(0)
		return

	var total: Dictionary = SafeGDScriptLanguage.run_tests(paths)
	var failed: int = int(total["failed"]) + int(total["errors"])
	print("%d passed, %d failed, %d errors across %d script(s)" % [
		total["passed"], total["failed"], total["errors"], total["scripts"].size()])
	quit(mini(failed, EXIT_CODE_CAP))

## Command-line paths after `--`, expanded; the whole project when there are none.
func _requested_paths() -> PackedStringArray:
	var requested := OS.get_cmdline_user_args()
	var paths := PackedStringArray()
	if requested.is_empty():
		_collect("res://", paths)
		return paths
	for entry in requested:
		var path: String = entry if entry.begins_with("res://") else "res://" + entry
		if DirAccess.dir_exists_absolute(path):
			_collect(path, paths)
		elif _has_tests(path):
			paths.push_back(path)
		else:
			push_warning("Skipping %s: not a .sgd script with @test functions." % path)
	return paths

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
