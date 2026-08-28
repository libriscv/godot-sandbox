extends SceneTree

func _initialize() -> void:
	if not Engine.is_editor_hint():
		push_error("editor smoke test was not started in editor mode")
		quit(1)
		return
	var language := SafeGDScriptLanguage.new()
	var validation := language.editor_validate(
		"func broken(\n\nfunc later(value):\n\treturn value\n",
		"res://partial.sgd", true, true, true, true)
	var marker := String.chr(0xffff)
	var source := "func calculate(amount: int):\n\tvar doubled = amount * 2\n\t" + marker
	var completion := language.editor_complete(source, "res://scope.sgd")
	var displays: Array[String] = []
	for option in completion.options:
		displays.push_back(option.display)
	var lookup := language.editor_lookup(source.replace(marker, ""), "doubled", "res://scope.sgd")
	var found_later := false
	for entry in validation.functions:
		found_later = found_later or String(entry).begins_with("later:")
	var passed: bool = not validation.valid and validation.errors.size() > 0 \
		and validation.has("warnings") and validation.has("safe_lines") \
		and found_later \
		and displays.has("amount") and displays.has("doubled") \
		and lookup.result == OK and lookup.location == 2
	language.free()
	if passed:
		print("[editor-smoke] PASSED")
		quit(0)
	else:
		push_error("editor validation/completion/lookup smoke test failed")
		quit(1)
