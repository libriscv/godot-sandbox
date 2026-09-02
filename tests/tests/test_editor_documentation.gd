extends GutTest


const FIXTURE_DIR := "res://tests/editor/"

func _page(pages: Array, name: String) -> Dictionary:
	for page in pages:
		if String(page.get("name", "")) == name:
			return page
	return {}

func _entry(entries, name: String) -> Dictionary:
	for entry in entries:
		if String(entry.get("name", "")) == name:
			return entry
	return {}

func _compiled(source: String) -> SafeGDScript:
	var script := SafeGDScript.new()
	script.set_source_code(source)
	assert_eq(script.get_compile_error(), "")
	return script

func test_a_script_page_names_itself_its_base_and_its_documented_members():
	var path := FIXTURE_DIR + "base.sgd"
	var script: SafeGDScript = load(path)
	var pages: Array = script.editor_documentation()
	assert_eq(pages.size(), 1)
	var page: Dictionary = pages[0]
	assert_eq(String(page.name), String(script.get_global_name()))
	assert_eq(String(page.inherits), "Node")
	assert_true(bool(page.is_script_doc))
	assert_eq(String(page.script_path), path)

	var advance := _entry(page.methods, "advance")
	assert_eq(String(advance.get("description", "")),
			"Advance by [param amount] steps and report the new total.")
	assert_eq(String(advance.get("return_type", "")), "int")

	assert_false(_entry(page.signals, "progressed").is_empty())
	assert_eq(String(_entry(page.properties, "speed").get("default_value", "")), "7")

func test_a_script_page_inherits_the_doc_name_of_a_script_base():
	var base_path := FIXTURE_DIR + "base.sgd"
	var base: SafeGDScript = load(base_path)
	var script := _compiled("extends \"" + base_path + "\"\nfunc go():\n\tpass\n")
	var pages: Array = script.editor_documentation()
	assert_eq(String(pages[0].inherits), String(base.get_global_name()))

func test_enum_members_document_as_constants_pointing_back_at_their_enum():
	var script: SafeGDScript = load(FIXTURE_DIR + "enum.sgd")
	var page: Dictionary = script.editor_documentation()[0]
	assert_true(page.enums.has("Mode"), str(page.enums))
	var run := _entry(page.constants, "RUN")
	assert_eq(String(run.get("value", "")), "5")
	assert_eq(String(run.get("enumeration", "")), "Mode")
	assert_eq(String(run.get("type", "")), "int")

func test_a_property_page_carries_its_accessors_default_and_enum():
	var script := _compiled("extends Node\nenum Mode { IDLE, RUN }\nvar mode: Mode = Mode.IDLE\n"
			+ "var total := 0:\n\tset(v):\n\t\ttotal = v\n\tget:\n\t\treturn total\nconst LIMIT := 9\n")
	var page: Dictionary = script.editor_documentation()[0]
	var mode := _entry(page.properties, "mode")
	assert_eq(String(mode.get("type", "")), "int")
	assert_eq(String(mode.get("enumeration", "")), "Mode")
	var total := _entry(page.properties, "total")
	assert_eq(String(total.get("type", "")), "int")
	assert_false(String(total.get("setter", "")).is_empty())
	assert_false(String(total.get("getter", "")).is_empty())
	assert_eq(String(total.get("default_value", "")), "0")
	assert_eq(String(_entry(page.constants, "LIMIT").get("value", "")), "9")

func test_a_nested_class_and_a_struct_each_get_their_own_page():
	var nested: SafeGDScript = load(FIXTURE_DIR + "nested_class.sgd")
	var marker := _page(nested.editor_documentation(),
			String(nested.get_global_name()) + ".Marker")
	assert_false(marker.is_empty(), "the nested class has no page")
	assert_eq(String(marker.inherits), "Node2D")
	assert_true(bool(marker.is_script_doc))
	assert_eq(String(marker.script_path), FIXTURE_DIR + "nested_class.sgd")
	assert_false(_entry(marker.methods, "heavier").is_empty())
	assert_eq(String(_entry(marker.properties, "weight").get("type", "")), "int")

	var struct_script: SafeGDScript = load(FIXTURE_DIR + "struct.sgd")
	var point := _page(struct_script.editor_documentation(),
			String(struct_script.get_global_name()) + ".Point")
	assert_false(point.is_empty(), "the struct has no page")
	assert_eq(String(_entry(point.properties, "x").get("type", "")), "Variant")

func test_a_file_doc_comment_becomes_the_brief_description():
	var script := _compiled("## A fixture script.\n##\n## The rest of the page.\nextends Node\n")
	var page: Dictionary = script.editor_documentation()[0]
	assert_eq(String(page.brief_description), "A fixture script.")
	assert_true(String(page.description).begins_with("A fixture script."))

func test_a_script_without_a_global_name_still_names_its_page():
	# DocTools::add_doc() refuses a page with an empty name, and the editor adds
	# a page for every script it scans, built-in ones included.
	var script := _compiled("extends Node\nfunc go():\n\tpass\n")
	var page: Dictionary = script.editor_documentation()[0]
	assert_eq(String(script.get_global_name()), "")
	assert_false(String(page.name).is_empty())
