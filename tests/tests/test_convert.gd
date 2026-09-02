extends GutTest

# .gd <-> .sgd conversion (SafeGDScriptLanguage.convert_script_path).
# The editor half (open tabs, scene reload) needs a running editor and is
# covered by tests/editor_convert_probe.gd.

const SOURCE := "extends Node\nvar value: int = 7\nfunc read():\n\treturn value\n"
const UID := "uid://c3q7abcdefgh1"
# Spelled in pieces: a quoted path in this file would be reported as a reference.
const GD := "res://tests/convert_probe" + ".gd"
const SGD := "res://tests/convert_probe" + ".sgd"
const SCENE := "res://tests/convert_probe.tscn"
const USER := "res://tests/convert_probe_user.gd"

func _write(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(text)
	file.close()

func _remove(path: String) -> void:
	if FileAccess.file_exists(path):
		DirAccess.remove_absolute(path)

func _scene_text(script_path: String) -> String:
	return ("[gd_scene load_steps=2 format=3 uid=\"uid://probe_scene\"]\n\n"
		+ "[ext_resource type=\"Script\" uid=\"%s\" path=\"%s\" id=\"1_a\"]\n\n" % [UID, script_path]
		+ "[node name=\"Probe\" type=\"Node\"]\nscript = ExtResource(\"1_a\")\n")

func after_each():
	for path in [GD, SGD, GD + ".uid", SGD + ".uid", SCENE, USER]:
		_remove(path)

func test_converted_script_path_toggles_the_extension():
	assert_eq(SafeGDScriptLanguage.converted_script_path("res://a/b.gd"), "res://a/b.sgd")
	assert_eq(SafeGDScriptLanguage.converted_script_path("res://a/b.sgd"), "res://a/b.gd")
	assert_eq(SafeGDScriptLanguage.converted_script_path("res://a/b.safegd"), "res://a/b.gd")
	assert_eq(SafeGDScriptLanguage.converted_script_path("res://a/b.tscn"), "")

func test_gd_to_sgd_moves_source_uid_and_scene_references():
	_write(GD, SOURCE)
	_write(GD + ".uid", UID + "\n")
	_write(SCENE, _scene_text(GD))
	_write(USER, "var s = preload(\"%s\")\n" % GD)

	var result := SafeGDScriptLanguage.convert_script_path(GD)
	assert_eq(result.get("path"), SGD)
	assert_false(FileAccess.file_exists(GD))
	assert_false(FileAccess.file_exists(GD + ".uid"))
	assert_eq(FileAccess.get_file_as_string(SGD), SOURCE)
	assert_eq(FileAccess.get_file_as_string(SGD + ".uid"), UID + "\n")
	assert_eq(Array(result.get("scenes")), [SCENE])
	assert_eq(FileAccess.get_file_as_string(SCENE), _scene_text(SGD))
	# Scripts naming the path are reported, never edited.
	assert_eq(Array(result.get("scripts")), [USER])
	assert_true(FileAccess.get_file_as_string(USER).contains(GD))
	assert_engine_error("names res://tests/convert_probe.gd in a string")

	# The converted file is a working SafeGDScript.
	var script: Script = load(SGD)
	assert_true(script is SafeGDScript)
	var node := Node.new()
	node.set_script(script)
	assert_eq(node.call("read"), 7)
	node.set_script(null)
	node.free()

func test_sgd_to_gd_round_trips():
	_write(SGD, SOURCE)
	_write(SGD + ".uid", UID + "\n")
	_write(SCENE, _scene_text(SGD))
	var result := SafeGDScriptLanguage.convert_script_path(SGD)
	assert_eq(result.get("path"), GD)
	assert_eq(FileAccess.get_file_as_string(GD), SOURCE)
	assert_eq(FileAccess.get_file_as_string(GD + ".uid"), UID + "\n")
	assert_eq(FileAccess.get_file_as_string(SCENE), _scene_text(GD))
	assert_eq(Array(result.get("scripts")), [])
	var back := SafeGDScriptLanguage.convert_script_path(GD)
	assert_eq(back.get("path"), SGD)
	assert_eq(FileAccess.get_file_as_string(SGD), SOURCE)
	assert_eq(FileAccess.get_file_as_string(SCENE), _scene_text(SGD))

func test_conversion_refuses_to_overwrite_or_invent():
	_write(GD, SOURCE)
	_write(SGD, "stale\n")
	assert_true(SafeGDScriptLanguage.convert_script_path(GD).is_empty())
	assert_engine_error("Refusing to overwrite")
	assert_eq(FileAccess.get_file_as_string(GD), SOURCE)
	assert_eq(FileAccess.get_file_as_string(SGD), "stale\n")
	assert_true(SafeGDScriptLanguage.convert_script_path("res://tests/missing" + ".gd").is_empty())
	assert_engine_error("No such script")
	assert_true(SafeGDScriptLanguage.convert_script_path(SCENE).is_empty())
	assert_engine_error("Not a GDScript or SafeGDScript file")
