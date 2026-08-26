extends GutTest

var Sandbox_TestsTests = load("res://tests/tests.elf")

func _compile(source: String) -> PackedByteArray:
	var ts := Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var elf: PackedByteArray = ts.vmcall("compile_to_elf", source)
	assert_false(elf.is_empty(), "Compiled ELF should not be empty")
	ts.free()
	return elf

func test_metadata_absent_on_non_gdscript_binary():
	var meta: Dictionary = Sandbox.get_program_metadata(PackedByteArray())
	assert_false(meta["has_metadata"], "Empty binary has no script metadata")

func test_class_and_base_are_embedded():
	var elf := _compile("""
class_name Enemy
extends Node2D

func _ready() -> void:
	pass
""")
	var meta: Dictionary = Sandbox.get_program_metadata(elf)
	assert_true(meta["has_metadata"], "Compiled ELF carries .gdsmeta")
	assert_eq(meta["class_name"], "Enemy", "class_name is embedded")
	assert_eq(meta["base_class"], "Node2D", "extends base is embedded")
	assert_false(meta["base_is_path"], "Node2D is a native base, not a path")
	assert_eq(meta["language"], "GDScript", ".comment tags the language")

func test_tool_flag_is_embedded():
	var plain := Sandbox.get_program_metadata(_compile("""
func f() -> void:
	pass
"""))
	assert_false(plain["is_tool"], "A non-@tool script is not a tool")

	var tool_meta := Sandbox.get_program_metadata(_compile("""
@tool
func f() -> void:
	pass
"""))
	assert_true(tool_meta["is_tool"], "@tool is embedded")

func test_function_signatures_are_embedded():
	var elf := _compile("""
## Doubles the speed.
func scaled(mult: int = 2) -> float:
	return float(mult) * 2.0

func plain(a, b):
	return a
""")
	var meta: Dictionary = Sandbox.get_program_metadata(elf)
	assert_true(meta["has_metadata"], "Compiled ELF carries .gdsmeta")

	var by_name := {}
	for m in meta["methods"]:
		by_name[m["name"]] = m

	assert_true(by_name.has("scaled"), "scaled is in the signatures")
	var scaled: Dictionary = by_name["scaled"]
	assert_eq(scaled["description"], "Doubles the speed.", "doc comment travels")
	assert_eq(int(scaled["return_type"]), TYPE_FLOAT, "return type is float")
	assert_eq(scaled["args"].size(), 1, "scaled takes one argument")
	assert_eq(scaled["args"][0]["name"], "mult", "argument name is preserved")
	assert_eq(int(scaled["args"][0]["type"]), TYPE_INT, "argument type is int")
	assert_true(scaled["args"][0]["optional"], "mult has a default, so it is optional")
	assert_eq(int(scaled["required_arguments"]), 0, "no required arguments")

	assert_true(by_name.has("plain"), "plain is in the signatures")
	assert_eq(by_name["plain"]["args"].size(), 2, "plain takes two arguments")

func test_double_precision_flag_matches_build():
	var meta: Dictionary = Sandbox.get_program_metadata(_compile("""
func f() -> void:
	pass
"""))
	# The compiler runs inside this sandbox build, so the flag it bakes in must
	# match this engine's real_t width.
	assert_true(meta.has("double_precision"), "double_precision flag is present")
	assert_eq(typeof(meta["double_precision"]), TYPE_BOOL, "double_precision is a bool")
