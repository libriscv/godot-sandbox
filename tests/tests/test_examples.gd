extends GutTest

var Sandbox_TestsTests = load("res://tests/tests.elf")

func _compile_and_load(gdscript_code: String, instructions_max: int = 4000) -> Sandbox:
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	ts.queue_free()
	assert_eq(compiled_elf.is_empty(), false, "Compilation should succeed")
	if compiled_elf.is_empty():
		return null
	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(instructions_max)
	return s


# README.md — SafeGDScript inventory example with struct, match and dict patterns
func test_readme_inventory_example():
	var gdscript_code = """
struct Item:
	var name : String = ""
	var value : int = 0
	var stackable : bool = true

var inventory : Array = []

func add_item(item_name : String, item_value : int) -> bool:
	for item in inventory:
		match item:
			{"name": var n, "stackable": true, ..} when n == item_name:
				item.value += item_value
				return true
	inventory.append(Item.new(item_name, item_value))
	return false

func get_total_value() -> int:
	var total = 0
	for item in inventory:
		total += item.value
	return total
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("add_item", "coin", 1), false, "first coin is new")
	assert_eq(s.vmcallv("add_item", "gem", 5), false, "first gem is new")
	assert_eq(s.vmcallv("add_item", "coin", 1), true, "second coin stacks")
	assert_eq(s.vmcallv("get_total_value"), 7, "total value should be 2 + 5")

	s.queue_free()


# MODDING.md — mod contract: mod_init receives a Dictionary, callables work
func test_modding_contract_example():
	var gdscript_code = """
var api : Dictionary = {}

func mod_init(granted : Dictionary) -> void:
	api = granted
	api["log"].call("hello")

func tick(delta) -> void:
	api["report"].call("ticks", 1)

func get_api_id() -> String:
	return api["id"]
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	var log_messages := []
	var reports := {}
	var api := {
		"id": "test_mod",
		"log": func(msg): log_messages.append(msg),
		"report": func(key, value): reports[key] = value,
	}

	s.vmcallv("mod_init", api)
	assert_eq(log_messages, ["hello"], "mod_init should call log")
	assert_eq(s.vmcallv("get_api_id"), "test_mod", "mod should read the granted id")

	s.vmcallv("tick", 0.016)
	assert_eq(reports.get("ticks"), 1, "tick should report via the granted callable")

	s.queue_free()


# MODDING.md — hostile mod: every try_* must be denied under restrictions.
# Source is inlined from examples/modding/mods/breakout/breakout.sgd.

const HOSTILE_MOD_SOURCE = """
var api : Dictionary = {}

func mod_init(a : Dictionary) -> void:
	api = a

func try_unwrap_callable() -> String:
	var host = api["log"].get_object()
	host.call("get_class")
	return "reached the object behind a granted Callable"

func try_rename_own_node() -> String:
	set_name("pwned")
	return "renamed its own node"

func try_reach_parent() -> String:
	var p = get_parent()
	p.call("get_class")
	return "called a method on its parent"

func try_reach_scene_root() -> String:
	var root = get_node("/root")
	root.call("get_class")
	return "called a method on the scene root"

func try_read_root_property() -> String:
	var root = get_node("/root")
	return "read a property off the scene root: " + str(root.name)

func try_write_root_property() -> String:
	var root = get_node("/root")
	root.name = "pwned"
	return "wrote a property on the scene root"

func try_load_resource() -> String:
	var res = load("res://project.godot")
	return "loaded a project resource: " + str(res)
"""

var _hostile_sandbox : Sandbox = null

func _setup_hostile_mod() -> Sandbox:
	if _hostile_sandbox != null:
		return _hostile_sandbox
	var s = _compile_and_load(HOSTILE_MOD_SOURCE, 400000)
	if s == null:
		return null
	s.restrictions = true
	add_child(s)
	var api := {
		"log": func(msg): pass,
	}
	s.vmcallv("mod_init", api)
	_hostile_sandbox = s
	return s

func after_all():
	if _hostile_sandbox != null:
		remove_child(_hostile_sandbox)
		_hostile_sandbox.queue_free()
		_hostile_sandbox = null

func test_hostile_mod_compiles():
	var s = _setup_hostile_mod()
	assert_not_null(s, "hostile mod should compile")

func test_hostile_try_unwrap_callable():
	var s = _setup_hostile_mod()
	if s == null:
		return
	var before := s.get_exceptions()
	s.vmcallv("try_unwrap_callable")
	assert_engine_error("GuestVariant::create(): Object is not allowed")
	assert_gt(s.get_exceptions(), before, "try_unwrap_callable should be denied")

func test_hostile_try_rename_own_node():
	var s = _setup_hostile_mod()
	if s == null:
		return
	var before := s.get_exceptions()
	s.vmcallv("try_rename_own_node")
	assert_engine_error("Method not allowed: set_name")
	assert_engine_error("Exception: Variant::call(): Method not allowed: set_name")
	assert_gt(s.get_exceptions(), before, "try_rename_own_node should be denied")

func test_hostile_try_reach_parent():
	var s = _setup_hostile_mod()
	if s == null:
		return
	var before := s.get_exceptions()
	s.vmcallv("try_reach_parent")
	assert_engine_error("Method not allowed: get_parent")
	assert_engine_error("Exception: Variant::call(): Method not allowed: get_parent")
	assert_gt(s.get_exceptions(), before, "try_reach_parent should be denied")

func test_hostile_try_reach_scene_root():
	var s = _setup_hostile_mod()
	if s == null:
		return
	var before := s.get_exceptions()
	s.vmcallv("try_reach_scene_root")
	assert_engine_error("Method not allowed: call")
	assert_engine_error("Exception: Variant::call(): Method not allowed: call")
	assert_gt(s.get_exceptions(), before, "try_reach_scene_root should be denied")

func test_hostile_try_read_root_property():
	var s = _setup_hostile_mod()
	if s == null:
		return
	var before := s.get_exceptions()
	s.vmcallv("try_read_root_property")
	assert_engine_error("Banned property accessed: name")
	assert_engine_error("Exception: Banned property accessed: name")
	assert_gt(s.get_exceptions(), before, "try_read_root_property should be denied")

func test_hostile_try_write_root_property():
	var s = _setup_hostile_mod()
	if s == null:
		return
	var before := s.get_exceptions()
	s.vmcallv("try_write_root_property")
	assert_engine_error("Banned property set: name")
	assert_engine_error("Exception: Banned property set: name")
	assert_gt(s.get_exceptions(), before, "try_write_root_property should be denied")

func test_hostile_try_load_resource():
	var s = _setup_hostile_mod()
	if s == null:
		return
	var before := s.get_exceptions()
	s.vmcallv("try_load_resource")
	assert_engine_error("Resource path is not allowed: res://project.godot")
	assert_engine_error("Exception: Resource path is not allowed: res://project.godot")
	assert_gt(s.get_exceptions(), before, "try_load_resource should be denied")
