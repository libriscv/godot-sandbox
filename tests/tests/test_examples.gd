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


# README.md — typed SafeGDScript inventory example
func test_readme_inventory_example():
	var gdscript_code = """
struct Item:
	var name: String
	var value: int
	var dropped_at: Vector2?

	func try_stack(other: Item) -> bool:
		if self.name != other.name:
			return false
		self.value += other.value
		return true

	func drop(at: int | Vector2) -> void:
		if at is int:
			self.dropped_at = Vector2(at, 0)
		else:
			self.dropped_at = at

var inventory: Array[Item] = []

func add_item(item_name: String, item_value: int) -> bool:
	var item := Item(item_name, item_value)
	for stored in inventory:
		if stored.try_stack(item):
			return true
	inventory.append(item)
	return false

func drop_first(at: int | Vector2) -> Vector2?:
	inventory[0].drop(at)
	return inventory[0].dropped_at
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("add_item", "coin", 1), false, "first coin is new")
	assert_eq(s.vmcallv("add_item", "gem", 5), false, "first gem is new")
	assert_eq(s.vmcallv("add_item", "coin", 1), true, "second coin stacks")
	assert_eq(s.vmcallv("drop_first", 3), Vector2(3, 0), "an int selects a drop column")
	assert_eq(s.vmcallv("drop_first", Vector2(4, 5)), Vector2(4, 5),
		"a Vector2 is used directly")

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

func try_smuggle_into_api() -> String:
	api["smuggled"] = "payload"
	return "added a key to the API the host is holding"

func try_replace_api_callable() -> String:
	api["log"] = func(x): return x
	return "replaced a granted Callable with its own"

func try_erase_api_key() -> String:
	api.erase("log")
	return "erased a key from the API"

func try_clear_api() -> String:
	api.clear()
	return "emptied the API Dictionary"
"""

var _hostile_sandbox : Sandbox = null
var _hostile_api : Dictionary = {}

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
	api.make_read_only()
	s.vmcallv("mod_init", api)
	_hostile_api = api
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

func test_hostile_try_smuggle_into_api():
	var s = _setup_hostile_mod()
	if s == null:
		return
	var before := s.get_exceptions()
	s.vmcallv("try_smuggle_into_api")
	assert_engine_error("Exception: Dictionary::operation: the container is read-only")
	assert_gt(s.get_exceptions(), before, "try_smuggle_into_api should be denied")
	assert_eq(_hostile_api.has("smuggled"), false, "no key was added to the API")

func test_hostile_try_replace_api_callable():
	var s = _setup_hostile_mod()
	if s == null:
		return
	var before := s.get_exceptions()
	s.vmcallv("try_replace_api_callable")
	assert_engine_error("Exception: Dictionary::operation: the container is read-only")
	assert_gt(s.get_exceptions(), before, "try_replace_api_callable should be denied")

func test_hostile_try_erase_api_key():
	var s = _setup_hostile_mod()
	if s == null:
		return
	var before := s.get_exceptions()
	s.vmcallv("try_erase_api_key")
	assert_engine_error("Exception: Variant::call: the container is read-only")
	assert_gt(s.get_exceptions(), before, "try_erase_api_key should be denied")
	assert_eq(_hostile_api.has("log"), true, "the granted Callable is still there")

func test_hostile_try_clear_api():
	var s = _setup_hostile_mod()
	if s == null:
		return
	var before := s.get_exceptions()
	s.vmcallv("try_clear_api")
	assert_engine_error("Exception: Dictionary::operation: the container is read-only")
	assert_gt(s.get_exceptions(), before, "try_clear_api should be denied")
	assert_eq(_hostile_api.size(), 1, "the API Dictionary was not emptied")


# Mirrors ModLoader.freeze().
static func _freeze(value: Variant) -> void:
	if value is Dictionary:
		if value.is_read_only():
			return
		value.make_read_only()
		for key in value:
			_freeze(value[key])
	elif value is Array:
		if value.is_read_only():
			return
		value.make_read_only()
		for element in value:
			_freeze(element)

static func _detach(value: Variant) -> Variant:
	if value is Dictionary or value is Array:
		return value.duplicate(true)
	return value

func _mod_node(source: String) -> Node:
	var script := SafeGDScript.new()
	script.set_source_code(source)
	var n := Node.new()
	n.set_script(script)
	n.set("restrictions", true)
	n.set("instructions_max", 400000)
	return n

func test_modding_mod_init_runs_before_the_node_enters_the_tree():
	var source := """
var api : Dictionary = {}
var order : Array = []

func _enter_tree():
	order.append("_enter_tree:" + str(api.size()))

func _ready():
	order.append("_ready:" + str(api.size()))

func mod_init(granted : Dictionary) -> void:
	api = granted
	order.append("mod_init:" + str(api.size()))

func get_order() -> Array:
	return order
"""
	var api := {"log": 1, "report": 2}
	_freeze(api)

	var good := _mod_node(source)
	good.call("mod_init", api)
	add_child(good)
	assert_eq(good.call("get_order"), ["mod_init:2", "_enter_tree:2", "_ready:2"],
		"_enter_tree and _ready must already have the API")
	good.queue_free()

	var bad := _mod_node(source)
	add_child(bad)
	bad.call("mod_init", api)
	assert_eq(bad.call("get_order"), ["_enter_tree:0", "_ready:0", "mod_init:2"],
		"adding first runs the mod's callbacks against an empty API")
	bad.queue_free()

func test_modding_a_container_from_a_mod_must_be_detached():
	var source := """
var mine : Dictionary = {}
var api : Dictionary = {}

func mod_init(granted : Dictionary) -> void:
	api = granted
	mine["score"] = 10

func report():
	api["report"].call("stats", mine)

func tamper():
	mine["score"] = 999999
"""
	var kept := {}
	var detached := {}
	var api := {"report": func(k, v):
		kept[k] = v
		detached[k] = _detach(v)}
	_freeze(api)

	var n := _mod_node(source)
	add_child(n)
	n.call("mod_init", api)
	n.call("report")
	assert_eq(kept["stats"], {"score": 10})
	assert_eq(detached["stats"], {"score": 10})

	n.call("tamper")
	assert_eq(kept["stats"]["score"], 999999,
		"the handle the mod handed over is still the mod's to rewrite")
	assert_eq(detached["stats"]["score"], 10,
		"detach() is what the game keeps")
	n.queue_free()

func test_modding_compile_error_is_reported():
	var broken := SafeGDScript.new()
	broken.set_source_code("func mod_init(a):\n\treturn undefined_name_here\n")
	assert_engine_error("SafeGDScript: : [Code Generation Error] Undefined variable: undefined_name_here (line 2, column 28)")
	var message : String = broken.get_compile_error()
	assert_true(message.contains("Undefined variable: undefined_name_here"),
		"the compiler's diagnostic reaches the loader, got: " + message)

	var fine := SafeGDScript.new()
	fine.set_source_code("func mod_init(a):\n\tpass\n")
	assert_eq(fine.get_compile_error(), "",
		"a script that compiled reports no error, even though the shared compiler kept the last one")

	var not_a_mod := SafeGDScript.new()
	not_a_mod.set_source_code("func something():\n\treturn 1\n")
	assert_eq(not_a_mod.get_compile_error(), "",
		"a script with no mod_init still compiled")

func test_modding_freeze_handles_a_cyclic_api():
	var api := {"name": "world"}
	api["self"] = api
	var a := []
	a.append(a)
	api["ring"] = a

	_freeze(api)

	assert_true(api.is_read_only(), "the api is frozen")
	assert_true(a.is_read_only(), "the self-referential Array is frozen")
	assert_true((api["self"] as Dictionary).is_read_only(), "the cycle is frozen once")

func test_modding_compile_error_belongs_to_its_own_script():
	var broken := SafeGDScript.new()
	broken.set_source_code("func mod_init(a):\n\treturn undefined_name_here\n")
	assert_engine_error("SafeGDScript: : [Code Generation Error] Undefined variable: undefined_name_here (line 2, column 28)")
	assert_ne(broken.get_compile_error(), "", "the broken script reports its own error")

	var untouched := SafeGDScript.new()
	assert_eq(untouched.get_compile_error(), "",
		"a script that was never compiled has no error, not the last one someone else hit")

func test_modding_failed_rebuild_is_not_reported_as_success():
	var script := SafeGDScript.new()
	script.set_source_code("func mod_init(a):\n\treturn 1\n")
	assert_eq(script.get_compile_error(), "", "the first build succeeded")

	script.set_source_code("func mod_init(a):\n\treturn undefined_name_here\n")
	assert_engine_error("SafeGDScript: : [Code Generation Error] Undefined variable: undefined_name_here (line 2, column 28)")
	assert_ne(script.get_compile_error(), "",
		"the failed rebuild is reported, even though the previous ELF is still there")

	script.set_source_code("func mod_init(a):\n\treturn 2\n")
	assert_eq(script.get_compile_error(), "", "a build that works clears the error again")

# README.md — safe navigation (?.) and null coalescing (??)
func test_readme_safe_navigation_example():
	var script := SafeGDScript.new()
	script.set_source_code("""
extends Node2D

var health: int = 42

func update_hud() -> String:
	var hp_label = get_node_or_null("HUD/HP")
	hp_label?.set_text("HP: " + str(health))
	return hp_label?.get_text() ?? "(no label)"

func aim_at(target) -> Vector2:
	return target?.global_position ?? global_position

@test
## Each test runs on a fresh Node2D owner, so there is no HUD and no target.
func the_hud_is_optional():
	assert(update_hud() == "(no label)", "'?.' skips the call when the node is missing")

@test
func aim_falls_back_to_our_own_position():
	assert(aim_at(null) == global_position)
""")
	assert_eq(script.get_compile_error(), "", "the example should compile")

	var owner := Node2D.new()
	owner.position = Vector2(1, 2)
	owner.set_script(script)

	assert_eq(owner.call("update_hud"), "(no label)",
		"'?.' answers null for a missing node, and '??' supplies the fallback")

	var hud := Node.new()
	hud.name = "HUD"
	var label := Label.new()
	label.name = "HP"
	hud.add_child(label)
	owner.add_child(hud)

	assert_eq(owner.call("update_hud"), "HP: 42",
		"with the label present every link in the chain runs")
	assert_eq(label.text, "HP: 42", "and the call through '?.' actually happened")

	var target := Node2D.new()
	target.position = Vector2(7, 8)
	assert_eq(owner.call("aim_at", target), Vector2(7, 8),
		"a live target is read through '?.'")
	assert_eq(owner.call("aim_at", null), Vector2(1, 2),
		"'??' only replaces null, so a null target falls back to our own position")

	target.free()
	owner.free()

	assert_eq(script.get_test_functions(),
		PackedStringArray(["the_hud_is_optional", "aim_falls_back_to_our_own_position"]),
		"both @test functions are published, in declaration order")
	var report := script.run_tests(PackedStringArray(), true)
	assert_eq(report["passed"], 2, "@test runs each case on its own fresh instance")
	assert_eq(report["failed"], 0)
	assert_eq(report["errors"], 0)
