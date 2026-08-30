extends GutTest

func _language() -> SafeGDScriptLanguage:
	return SafeGDScriptLanguage.new()

func test_editor_validation_returns_partial_functions_and_requested_channels():
	if not Engine.is_editor_hint():
		pass_test("requires an editor-hint test process")
		return
	var language := _language()
	var answer := language.editor_validate(
			"func broken(\n\nfunc later(value):\n\treturn value\n",
			"res://partial.sgd", true, true, true, true)
	assert_false(answer.valid)
	assert_gt(answer.errors.size(), 0)
	assert_true(answer.has("warnings"))
	assert_true(answer.has("safe_lines"))
	var found_later := false
	for entry in answer.functions:
		found_later = found_later or String(entry).begins_with("later:")
	assert_true(found_later)
	language.free()

func test_editor_warnings_include_the_range_based_godot_47_fields():
	if not Engine.is_editor_hint():
		pass_test("requires an editor-hint test process")
		return
	var language := _language()
	var answer := language.editor_validate(
			"func example():\n\tvar unused = 1\n",
			"res://warning.sgd", false, true, true, false)
	assert_gt(answer.warnings.size(), 0)
	if answer.warnings.size() > 0:
		var warning: Dictionary = answer.warnings[0]
		assert_true(warning.has("start_line"))
		assert_true(warning.has("end_line"))
		assert_true(warning.has("string_code"))
		assert_true(warning.has("message"))
		assert_eq(warning.start_line, 2)
		assert_eq(warning.end_line, 2)
	language.free()

func test_editor_completion_and_lookup_share_lexical_scope():
	if not Engine.is_editor_hint():
		pass_test("requires an editor-hint test process")
		return
	var language := _language()
	var marker := String.chr(0xffff)
	var source := "func calculate(amount: int):\n\tvar doubled = amount * 2\n\t" + marker
	var completion := language.editor_complete(source, "res://scope.sgd")
	var displays: Array[String] = []
	for option in completion.options:
		displays.push_back(option.display)
	assert_true(displays.has("amount"))
	assert_true(displays.has("doubled"))
	var lookup := language.editor_lookup(source.replace(marker, ""), "doubled", "res://scope.sgd")
	assert_eq(lookup.result, OK)
	assert_eq(lookup.location, 2)
	language.free()

func test_script_property_metadata_exists_without_an_instance():
	var script := SafeGDScript.new()
	script.set_source_code("@export_range(0, 10) var count: int = 3\nvar runtime_value = make_value()\nfunc make_value():\n\treturn 9\n")
	assert_eq(script.get_compile_error(), "")
	var names: Array[StringName] = []
	for property in script.get_script_property_list():
		names.push_back(property.name)
	assert_true(names.has(&"count"))
	assert_true(names.has(&"runtime_value"))
	assert_eq(script.get_property_default_value(&"count"), 3)

func test_soft_reload_preserves_each_instance_and_hard_reload_resets():
	var script := SafeGDScript.new()
	script.set_source_code("var value: int = 1\nfunc read():\n\treturn value\n")
	var first := Node.new()
	var second := Node.new()
	first.set_script(script)
	second.set_script(script)
	first.set("value", 10)
	second.set("value", 20)
	script.set_source_code("var value: int = 2\nvar added: int = 7\nfunc read():\n\treturn value\n")
	assert_eq(script.reload(true), OK)
	assert_eq(first.call("read"), 10)
	assert_eq(second.call("read"), 20)
	assert_eq(first.get("added"), 7)
	assert_eq(script.reload(false), OK)
	assert_eq(first.call("read"), 2)
	assert_eq(second.call("read"), 2)
	first.set_script(null)
	second.set_script(null)
	first.free()
	second.free()

func test_failed_reload_keeps_the_last_program_and_state():
	var script := SafeGDScript.new()
	script.set_source_code("var value: int = 4\nfunc read():\n\treturn value\n")
	var node := Node.new()
	node.set_script(script)
	node.set("value", 19)
	script.set_source_code("func broken(:\n")
	assert_ne(script.get_compile_error(), "")
	assert_eq(node.call("read"), 19)
	assert_engine_error("Unclosed '('")
	node.set_script(null)
	node.free()
