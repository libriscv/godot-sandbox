extends GutTest

# Not a const: String.chr() is not a constant expression.
var CARET := String.chr(0xFFFF)

const FIXTURE_DIR := "res://tests/editor/"
const FIXTURES := ["base", "global_class", "autoload", "nested_class", "struct", "trait", "enum"]

func _language() -> SafeGDScriptLanguage:
	return SafeGDScriptLanguage.new()

func _validate(source: String, path := "res://probe.sgd", functions := true, errors := true,
		warnings := true, safe_lines := true) -> Dictionary:
	var language := _language()
	var answer := language.editor_validate(source, path, functions, errors, warnings, safe_lines)
	language.free()
	return answer

func _complete_at(source_with_marker: String, path := "res://probe.sgd",
		owner: Object = null) -> Dictionary:
	var language := _language()
	var answer := language.editor_complete(source_with_marker, path, owner)
	language.free()
	return answer

func _lookup(source: String, symbol: String, path := "res://probe.sgd",
		owner: Object = null) -> Dictionary:
	var language := _language()
	var answer := language.editor_lookup(source, symbol, path, owner)
	language.free()
	return answer

func _locations(options: Array) -> Dictionary:
	var locations := {}
	for option in options:
		var display := String(option.display)
		if not locations.has(display):
			locations[display] = int(option.location)
	return locations

func _displays(options: Array) -> Array[String]:
	var displays: Array[String] = []
	for option in options:
		displays.push_back(String(option.display))
	return displays

func test_editor_validation_returns_partial_functions_and_requested_channels():
	var answer := _validate("func broken(\n\nfunc later(value):\n\treturn value\n",
			"res://partial.sgd")
	assert_false(answer.valid)
	assert_gt(answer.errors.size(), 0)
	assert_true(answer.has("warnings"))
	assert_true(answer.has("safe_lines"))
	var found_later := false
	for entry in answer.functions:
		found_later = found_later or String(entry).begins_with("later:")
	assert_true(found_later)

func test_editor_warnings_include_the_range_based_godot_47_fields():
	var answer := _validate("func example():\n\tvar unused = 1\n", "res://warning.sgd",
			false, true, true, false)
	assert_gt(answer.warnings.size(), 0)
	if answer.warnings.size() > 0:
		var warning: Dictionary = answer.warnings[0]
		assert_true(warning.has("start_line"))
		assert_true(warning.has("end_line"))
		assert_true(warning.has("string_code"))
		assert_true(warning.has("message"))
		assert_eq(warning.start_line, 2)
		assert_eq(warning.end_line, 2)

func test_editor_completion_and_lookup_share_lexical_scope():
	var source := "func calculate(amount: int):\n\tvar doubled = amount * 2\n\t" + CARET
	var displays := _displays(_complete_at(source, "res://scope.sgd").options)
	assert_true(displays.has("amount"))
	assert_true(displays.has("doubled"))
	var lookup := _lookup(source, "doubled", "res://scope.sgd")
	assert_eq(lookup.result, OK)
	assert_eq(lookup.location, 2)

func test_editor_fixture_scripts_compile():
	for name in FIXTURES:
		var path: String = FIXTURE_DIR + name + ".sgd"
		var script: SafeGDScript = load(path)
		assert_not_null(script, path)
		if script == null:
			continue
		assert_eq(script.get_compile_error(), "", path)
		assert_gt(script.get_source_code().length(), 0, path)

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

func test_editor_lookup_resolves_at_the_caret_line():
	var source := "func first():\n\tvar only_here = 1\n\treturn only_here\n\nfunc second():\n\t" + CARET + "\n"
	var elsewhere := _lookup(source, "only_here", "res://caret.sgd")
	assert_ne(elsewhere.result, OK)
	var own := "func first():\n\tvar only_here = 1\n\t" + CARET + "\n\nfunc second():\n\tpass\n"
	var found := _lookup(own, "only_here", "res://caret.sgd")
	assert_eq(found.result, OK)
	assert_eq(found.location, 2)

func test_editor_completion_ranks_locals_then_own_members_then_engine_members():
	var source := "extends Node2D\nvar speed := 1.0\nfunc go():\n\tvar step := 2\n\t" + CARET
	var options: Array = _complete_at(source, "res://rank.sgd").options
	for option in options:
		for key in ["kind", "display", "insert_text", "font_color", "icon", "default_value", "location"]:
			assert_true(option.has(key), key)
	var locations := _locations(options)
	assert_true(locations.has("step"))
	assert_true(locations.has("speed"))
	assert_true(locations.has("scale"))
	assert_eq(locations["step"], ScriptLanguageExtension.LOCATION_LOCAL)
	assert_eq(locations["speed"], ScriptLanguageExtension.LOCATION_OTHER_USER_CODE)
	assert_eq(locations["scale"], ScriptLanguageExtension.LOCATION_OTHER)

func test_editor_public_tables_cover_the_compiler_globals():
	var language := _language()
	var constants: Dictionary = language.editor_public_constants()
	var functions: Array = language.editor_public_functions()
	var annotations: Array = language.editor_public_annotations()
	language.free()

	var constant_names := []
	var pi := 0.0
	for key in constants:
		constant_names.push_back(String(constants[key].name))
		if String(constants[key].name) == "PI":
			pi = float(constants[key].value)
	assert_gt(constants.size(), 4)
	assert_true(constant_names.has("PI"))
	assert_almost_eq(pi, PI, 0.0001)

	var function_names := []
	for entry in functions:
		function_names.push_back(String(entry.name))
	assert_gt(functions.size(), 20)
	assert_true(function_names.has("clamp"))
	assert_true(function_names.has("print"))

	var annotation_names := []
	for entry in annotations:
		annotation_names.push_back(String(entry.name))
	assert_eq(annotations.size(), 35)
	assert_true(annotation_names.has("@export"))
	assert_true(annotation_names.has("@onready"))
	assert_true(annotation_names.has("@test"))

func test_editor_templates_substitute_and_every_built_in_compiles():
	var language := _language()
	var made: SafeGDScript = language.editor_make_template(
			"# _CLASS_\nextends _BASE_\n\nfunc _ready():\n_TS_pass\n", "my_thing", "Node")
	assert_eq(made.get_source_code(), "# MyThing\nextends Node\n\nfunc _ready():\n\tpass\n")
	assert_eq(made.get_compile_error(), "")

	var empty: SafeGDScript = language.editor_make_template("", "Thing", "Node")
	assert_true(empty.get_source_code().begins_with("extends Node"))
	assert_eq(empty.get_compile_error(), "")

	assert_eq(language.editor_built_in_templates("Sprite2D").size(), 0)
	var node_names := []
	for entry in language.editor_built_in_templates("Node"):
		node_names.push_back(String(entry.name))
	for expected in ["Default", "GDScript Tour", "Typed Struct", "Nullable and Union Types", "If Var", "Traits"]:
		assert_true(node_names.has(expected), expected)
	for base in ["Node", "Object", "CharacterBody2D", "CharacterBody3D"]:
		var templates: Array = language.editor_built_in_templates(base)
		assert_gt(templates.size(), 0, base)
		for entry in templates:
			assert_eq(String(entry.inherit), base)
			assert_false(String(entry.name).is_empty())
			assert_false(String(entry.description).is_empty())
			assert_eq(int(entry.origin), 0)
			var script: SafeGDScript = language.editor_make_template(entry.content, "Thing", base)
			assert_eq(script.get_compile_error(), "", base + "/" + String(entry.name))
	language.free()

func test_editor_outline_validation_asks_for_functions_alone():
	var answer := _validate("func alpha():\n\tpass\n", "res://outline.sgd", true, false, false, false)
	assert_true(answer.has("functions"))
	assert_false(answer.has("errors"))
	assert_false(answer.has("warnings"))
	assert_false(answer.has("safe_lines"))

func test_editor_outline_nests_nested_class_methods_and_keeps_statics():
	var answer := _validate(
			"extends Node\n\nclass Inner extends Node2D:\n\tfunc method():\n\t\tpass\n\nstatic func helper():\n\tpass\n",
			"res://nested.sgd", true, false, false, false)
	var names := []
	for entry in answer.functions:
		names.push_back(String(entry).get_slice(":", 0))
	assert_true(names.has("Inner.method"), str(names))
	assert_true(names.has("helper"), str(names))

func test_editor_completion_types_a_builtin_receiver():
	var source := "func go():\n\tvar v := Vector2()\n\tv." + CARET
	var displays := _displays(_complete_at(source, "res://builtin.sgd").options)
	assert_true(displays.has("length("), str(displays.size()))
	assert_true(displays.has("x"))

func test_editor_completion_resolves_a_scene_path_receiver():
	var root := Node2D.new()
	var sprite := Sprite2D.new()
	sprite.name = "Sprite2D"
	root.add_child(sprite)
	var source := "extends Node2D\nfunc go():\n\t$Sprite2D." + CARET
	var displays := _displays(_complete_at(source, "res://sugar.sgd", root).options)
	root.free()
	assert_true(displays.has("texture"), str(displays.size()))

func test_editor_completion_walks_a_chained_receiver():
	var root := Node2D.new()
	var child := Node2D.new()
	child.name = "A"
	root.add_child(child)
	var source := "extends Node2D\nfunc go():\n\tget_node(\"A\").position." + CARET
	var displays := _displays(_complete_at(source, "res://chain.sgd", root).options)
	root.free()
	assert_true(displays.has("x"), str(displays.size()))

func test_editor_completion_offers_an_autoloads_members():
	var setting := "autoload/Fixture"
	ProjectSettings.set_setting(setting, "*" + FIXTURE_DIR + "autoload.sgd")
	var source := "func go():\n\tFixture." + CARET
	var displays := _displays(_complete_at(source, "res://autoload_use.sgd").options)
	ProjectSettings.set_setting(setting, null)
	assert_true(displays.has("bump("), str(displays.size()))

func test_editor_completion_offers_the_base_class_after_super():
	var source := "extends Node2D\nfunc go():\n\tsuper." + CARET
	var options: Array = _complete_at(source, "res://super.sgd").options
	var displays := _displays(options)
	assert_true(displays.has("get_position("), str(displays.size()))
	assert_eq(_locations(options)["get_position("],
			ScriptLanguageExtension.LOCATION_PARENT_MASK + 1)

func test_editor_completion_offers_virtuals_and_own_signals_after_self():
	var source := "extends Node\nsignal pinged\nfunc go():\n\tself." + CARET
	var displays := _displays(_complete_at(source, "res://self.sgd").options)
	assert_true(displays.has("_ready("), str(displays.size()))
	assert_true(displays.has("pinged"))

func test_editor_completion_offers_enum_members_on_the_enum():
	var source := "extends Node\nenum Mode { IDLE, RUN = 5, STOP }\nfunc go():\n\tMode." + CARET
	var displays := _displays(_complete_at(source, "res://enum_use.sgd").options)
	assert_true(displays.has("RUN"), str(displays.size()))
	assert_true(displays.has("STOP"))

func test_editor_completion_offers_engine_class_constants_and_enums():
	var source := "func go():\n\tNode." + CARET
	var displays := _displays(_complete_at(source, "res://classdb.sgd").options)
	assert_true(displays.has("NOTIFICATION_READY"), str(displays.size()))
	assert_true(displays.has("ProcessMode"))

func test_editor_completion_offers_a_struct_and_its_fields():
	var source := "struct Point:\n\tvar x = 0\n\tvar y = 0\n\nfunc go():\n\tvar p := Point.new(1, 2)\n\tp." + CARET
	var displays := _displays(_complete_at(source, "res://struct_use.sgd").options)
	assert_true(displays.has("x"), str(displays.size()))
	assert_true(displays.has("y"))

func test_editor_call_hint_uses_the_declared_signature():
	var source := "func f(a: int, b := 2):\n\tpass\n\nfunc go():\n\tf(" + CARET
	var answer := _complete_at(source, "res://hint.sgd")
	assert_eq(String(answer.call_hint), "void f(" + CARET + "a: int" + CARET + ", b: int = 2)")

func test_editor_call_hint_reads_an_engine_method_and_a_builtin_constructor():
	var engine_hint := _complete_at("extends Node2D\nfunc go():\n\tset_process(" + CARET,
			"res://hint2.sgd")
	assert_true(String(engine_hint.call_hint).begins_with("void set_process("),
			String(engine_hint.call_hint))
	var builtin := _complete_at("func go():\n\tvar v = Vector2(" + CARET, "res://hint3.sgd")
	assert_true(String(builtin.call_hint).contains("Vector2 Vector2("),
			String(builtin.call_hint))
	assert_true(String(builtin.call_hint).contains("x: float"), String(builtin.call_hint))

func test_editor_string_completion_leaves_filtering_to_the_editor():
	var source := "func go():\n\tvar s = preload(\"zz" + CARET
	var displays := _displays(_complete_at(source, "res://strings.sgd").options)
	assert_gt(displays.size(), 0)
	var unfiltered := false
	for display in displays:
		unfiltered = unfiltered or not display.begins_with("zz")
	assert_true(unfiltered, "the editor fuzzy-filters; the language must not")

func test_editor_completion_offers_a_nested_class_constructor_and_its_methods():
	var source := "extends Node\nclass Marker extends Node2D:\n\tvar weight := 2\n\tfunc heavier(by: int):\n\t\tpass\n\nfunc go():\n\tMarker." + CARET
	var displays := _displays(_complete_at(source, "res://nested_use.sgd").options)
	assert_true(displays.has("new("), str(displays.size()))

func test_editor_completion_reads_a_preloaded_script_receiver():
	var source := "func go():\n\tpreload(\"" + FIXTURE_DIR + "global_class.sgd\")." + CARET
	var displays := _displays(_complete_at(source, "res://preload_use.sgd").options)
	assert_true(displays.has("new("), str(displays.size()))

func test_editor_completion_scopes_a_for_loop_variable_to_its_body():
	var inside := "func go():\n\tfor index in 3:\n\t\tprint(index)\n\t\t" + CARET + "\n"
	assert_true(_displays(_complete_at(inside, "res://loop.sgd").options).has("index"))
	var after := "func go():\n\tfor index in 3:\n\t\tprint(index)\n\tprint(0)\n\t" + CARET + "\n"
	assert_false(_displays(_complete_at(after, "res://loop.sgd").options).has("index"))

func test_editor_completion_types_a_struct_receiver_from_the_model():
	var source := "struct Point:\n\tvar weight = 0\n\nfunc go():\n\tvar p := Point.new(1)\n\tp." + CARET
	var displays := _displays(_complete_at(source, "res://struct_model.sgd").options)
	assert_true(displays.has("weight"), str(displays.size()))

func test_editor_errors_carry_the_column_the_problem_starts_at():
	var missing := _validate("var x = \n", "res://column.sgd", false, true, false, false)
	assert_false(missing.valid)
	assert_gt(missing.errors.size(), 0)
	if missing.errors.size() > 0:
		assert_eq(int(missing.errors[0].line), 1)
		assert_eq(int(missing.errors[0].column), 9)
	var bad := _validate("func go():\n\tvar a = *\n", "res://column2.sgd", false, true, false, false)
	assert_false(bad.valid)
	assert_gt(bad.errors.size(), 0)
	if bad.errors.size() > 0:
		assert_eq(int(bad.errors[0].line), 2)
		assert_eq(int(bad.errors[0].column), 10)

func test_editor_warnings_carry_column_ranges_and_avoid_substring_false_positives():
	var answer := _validate("func go():\n\tvar x2 = 1\n\tvar text = \"a / b\"\n\tif x2:\n\t\treturn text\n\tprint(x2)\n",
			"res://precise.sgd", false, false, true, false)
	assert_eq(answer.warnings.size(), 0, str(answer.warnings))
	var flagged := _validate("func go():\n\tvar unused = 1\n", "res://column3.sgd", false, false, true, false)
	assert_eq(flagged.warnings.size(), 1)
	if flagged.warnings.size() > 0:
		assert_true(flagged.warnings[0].has("start_column"))
		assert_eq(int(flagged.warnings[0].start_column), 6)
		assert_eq(int(flagged.warnings[0].end_column), 12)

func test_editor_lookup_names_an_own_method_on_the_scripts_own_page():
	var path := FIXTURE_DIR + "base.sgd"
	var script: SafeGDScript = load(path)
	var marked := script.get_source_code().replace("func advance", "func adv" + CARET + "ance")
	var answer := _lookup(marked, "advance", path)
	assert_eq(answer.result, OK)
	assert_eq(int(answer.type), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_METHOD)
	assert_eq(String(answer.class_name), String(script.get_global_name()))
	assert_eq(String(answer.class_member), "advance")
	assert_eq(String(answer.script_path), path)
	assert_eq(int(answer.location), 8)

func test_editor_lookup_types_a_local_and_carries_its_doc_comment():
	var source := "func go():\n\t## A point in space.\n\tvar v := Vector2()\n\tprint(v" + CARET + ")\n"
	var answer := _lookup(source, "v", "res://local.sgd")
	assert_eq(answer.result, OK)
	assert_eq(int(answer.type), ScriptLanguageExtension.LOOKUP_RESULT_LOCAL_VARIABLE)
	assert_eq(String(answer.doc_type), "Vector2")
	assert_eq(String(answer.description), "A point in space.")
	assert_eq(int(answer.location), 3)

func test_editor_lookup_reports_a_local_constants_value():
	var source := "func go():\n\tconst LIMIT = 42\n\tprint(LIM" + CARET + "IT)\n"
	var answer := _lookup(source, "LIMIT", "res://const.sgd")
	assert_eq(answer.result, OK)
	assert_eq(int(answer.type), ScriptLanguageExtension.LOOKUP_RESULT_LOCAL_CONSTANT)
	assert_eq(String(answer.value), "42")
	assert_eq(String(answer.doc_type), "int")

func test_editor_lookup_reaches_an_autoload_and_its_members():
	var setting := "autoload/Fixture"
	var path := FIXTURE_DIR + "autoload.sgd"
	ProjectSettings.set_setting(setting, "*" + path)
	var member := _lookup("func go():\n\tFixture.bu" + CARET + "mp(1)\n", "bump", "res://use.sgd")
	var named := _lookup("func go():\n\tFix" + CARET + "ture.bump(1)\n", "Fixture", "res://use.sgd")
	ProjectSettings.set_setting(setting, null)
	assert_eq(member.result, OK)
	assert_eq(int(member.type), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_METHOD)
	assert_eq(String(member.script_path), path)
	assert_eq(int(member.location), 5)
	assert_eq(named.result, OK)
	assert_eq(int(named.type), ScriptLanguageExtension.LOOKUP_RESULT_CLASS)
	assert_eq(String(named.script_path), path)
	assert_eq(int(named.location), 1)

func test_editor_lookup_follows_the_base_script_for_inherited_members():
	var path := FIXTURE_DIR + "base.sgd"
	var head := "extends \"" + path + "\"\nfunc go():\n\t"
	for source in [head + "adv" + CARET + "ance(1)\n", head + "super.adv" + CARET + "ance(1)\n"]:
		var answer := _lookup(source, "advance", "res://derived.sgd")
		assert_eq(answer.result, OK, source)
		assert_eq(int(answer.type), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_METHOD)
		assert_eq(String(answer.script_path), path)
		assert_eq(int(answer.location), 8)

func test_editor_lookup_names_the_engine_type_a_typed_receiver_reaches():
	var builtin := _lookup("func go():\n\tvar v := Vector2()\n\tv.len" + CARET + "gth()\n",
			"length", "res://builtin_lookup.sgd")
	assert_eq(builtin.result, OK)
	assert_eq(int(builtin.type), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_METHOD)
	assert_eq(String(builtin.class_name), "Vector2")

	var constant := _lookup("func go():\n\tvar v = Vector2.ZE" + CARET + "RO\n", "ZERO",
			"res://builtin_lookup.sgd")
	assert_eq(int(constant.type), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_CONSTANT)
	assert_eq(String(constant.class_name), "Vector2")

	var property := _lookup("func go():\n\tvar n: Node2D = null\n\tn.posi" + CARET + "tion\n",
			"position", "res://engine_lookup.sgd")
	assert_eq(int(property.type), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_PROPERTY)
	assert_eq(String(property.class_name), "Node2D")

func test_editor_lookup_names_own_signals_enums_and_nested_types():
	var doc := "SafeGDScript_Members"
	var source := "extends Node\nsignal pinged(x: int)\nenum Mode { IDLE, RUN = 5 }\nstruct Point:\n\tvar x = 0\n\nfunc go():\n\t"
	var signal_answer := _lookup(source + "ping" + CARET + "ed.emit(1)\n", "pinged", "res://members.sgd")
	assert_eq(int(signal_answer.type), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_SIGNAL)
	assert_eq(String(signal_answer.class_name), doc)
	assert_eq(int(signal_answer.location), 2)

	var enum_answer := _lookup(source + "print(Mo" + CARET + "de)\n", "Mode", "res://members.sgd")
	assert_eq(int(enum_answer.type), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_ENUM)
	assert_eq(String(enum_answer.class_name), doc)

	var member_answer := _lookup(source + "print(Mode.RU" + CARET + "N)\n", "RUN", "res://members.sgd")
	assert_eq(int(member_answer.type), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_CONSTANT)
	assert_eq(String(member_answer.class_name), doc)
	assert_eq(String(member_answer.class_member), "RUN")

	var struct_answer := _lookup(source + "var p = Poi" + CARET + "nt.new()\n", "Point", "res://members.sgd")
	assert_eq(int(struct_answer.type), ScriptLanguageExtension.LOOKUP_RESULT_CLASS)
	assert_eq(String(struct_answer.class_name), doc + ".Point")

func test_editor_validation_reports_arity_and_unknown_members_without_a_full_compile():
	var arity := _validate("func takes(a, b):\n\treturn a\nfunc go():\n\treturn takes(1)\n",
			"res://arity.sgd", false, true, false, false)
	assert_false(arity.valid)
	var field := _validate("struct Point:\n\tvar x = 0\n\nfunc go():\n\tvar p = Point.new()\n\treturn p.z\n",
			"res://field.sgd", false, true, false, false)
	assert_false(field.valid)
	var member := _validate("enum Mode { IDLE }\nfunc go():\n\treturn Mode.WALK\n",
			"res://member.sgd", false, true, false, false)
	assert_false(member.valid)
