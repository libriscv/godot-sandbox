extends GutTest

const OLD_UID := "uid://b0000000000a"
const SCENE := "res://tests/set_uid_probe.tscn"
const UNKNOWN := "res://tests/set_uid_probe.foo"

func after_each() -> void:
	for path in [SCENE, UNKNOWN]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)

func _write(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(text)
	file.close()

func test_set_uid_reaches_the_text_scene_saver():
	_write(SCENE, '[gd_scene format=3 uid="%s"]\n\n[node name="Root" type="Node"]\n' % OLD_UID)
	var uid := ResourceUID.create_id()
	assert_eq(ResourceSaver.set_uid(SCENE, uid), OK, "A text scene takes a new uid")
	var text := FileAccess.get_file_as_string(SCENE)
	assert_string_contains(text, ResourceUID.id_to_text(uid), "The scene carries the new uid")
	assert_false(text.contains(OLD_UID), "The old uid is gone")
	assert_string_contains(text, '[node name="Root" type="Node"]', "The rest of the scene is kept")

func test_set_uid_does_not_claim_a_file_no_saver_owns():
	_write(UNKNOWN, "not a resource")
	var err := ResourceSaver.set_uid(UNKNOWN, ResourceUID.create_id())
	assert_ne(err, OK, "No saver owns a .foo file")
	assert_eq(FileAccess.get_file_as_string(UNKNOWN), "not a resource", "The file is untouched")
