class_name ModLoader
extends Node

## Loads .sgd mods with Sandbox restrictions enabled.
##
## Each mod has a mod.cfg manifest and a .sgd entry file. The loader compiles
## the entry to RISC-V and passes a read-only API Dictionary to mod_init().

signal mod_loaded(id: String)
signal mod_failed(id: String, reason: String)

signal mod_errored(id: String, exceptions: int, timeouts: int)
signal mod_quarantined(id: String)

const MOD_DIRS := ["res://mods", "user://mods"]

const API_VERSION := 1

# Ceilings. A manifest can lower these, never raise them.
const MAX_EXECUTION_TIMEOUT := 200  # millions of instructions per call
const MAX_MEMORY := 32              # MB
const MAX_ALLOCATIONS := 8000
const MAX_REFERENCES := 100
const MAX_COROUTINES := 32
const MAX_SOURCE_BYTES := 256 * 1024

const MAX_FAULTS := 60
const CLEAN_FRAMES_TO_FORGIVE := 120

# Called as api_provider.call(id, manifest) -> Dictionary.
# The returned Dictionary is frozen; build a fresh one per mod.
var api_provider := Callable()

var property_policy := Callable()
var method_policy := Callable()

var _mods := {}
var _health := {}

func scan() -> int:
	var found := []
	for root in MOD_DIRS:
		if not DirAccess.dir_exists_absolute(root):
			continue
		for dir_name in DirAccess.get_directories_at(root):
			var dir: String = root.path_join(dir_name)
			var cfg := ConfigFile.new()
			if cfg.load(dir.path_join("mod.cfg")) != OK:
				mod_failed.emit(dir_name, "no readable mod.cfg in " + dir)
				continue
			found.append({
				"dir": dir,
				"order": int(cfg.get_value("mod", "order", 0)),
				"id": str(cfg.get_value("mod", "id", dir_name)),
			})
	found.sort_custom(func(a, b):
		if a["order"] != b["order"]:
			return a["order"] < b["order"]
		return a["id"] < b["id"])

	var count := 0
	for entry in found:
		if load_mod(entry["dir"]) != null:
			count += 1
	return count

func get_mod(id: String) -> Node:
	return _mods.get(id)

func get_mod_ids() -> Array:
	return _mods.keys()

func is_quarantined(id: String) -> bool:
	return _health.has(id) and bool(_health[id]["quarantined"])

func resume(id: String) -> void:
	var node: Node = _mods.get(id)
	if node == null or not is_quarantined(id):
		return
	_reset_health(id, node)
	_set_callbacks_enabled(node, true)

func unload(id: String) -> void:
	var node: Node = _mods.get(id)
	if node == null:
		return
	if _declares(node, "mod_deinit", 0):
		var exceptions := int(node.get("monitor_exceptions"))
		node.call("mod_deinit")
		if int(node.get("monitor_exceptions")) > exceptions:
			push_warning("[modloader] mod '%s' raised inside mod_deinit()" % id)
	_mods.erase(id)
	_health.erase(id)
	if node.get_parent() != null:
		remove_child(node)
	node.queue_free()

func unload_all() -> void:
	for id in _mods.keys():
		unload(id)

func _exit_tree() -> void:
	unload_all()

func load_mod(dir: String) -> Node:
	var cfg := ConfigFile.new()
	if cfg.load(dir.path_join("mod.cfg")) != OK:
		mod_failed.emit(dir.get_file(), "no readable mod.cfg in " + dir)
		return null

	# Godot silently rewrites node names with special characters.
	var id: String = str(cfg.get_value("mod", "id", dir.get_file()))
	if not id.is_valid_ascii_identifier():
		mod_failed.emit(dir.get_file(), "mod.cfg 'id' must be a plain name: letters, digits and "
			+ "underscore, not starting with a digit: " + id)
		return null
	if _mods.has(id):
		mod_failed.emit(id, "a mod with this id is already loaded")
		return null

	var requires := int(cfg.get_value("mod", "requires_api", API_VERSION))
	if requires > API_VERSION:
		mod_failed.emit(id, "needs API version %d, this game provides %d" % [requires, API_VERSION])
		return null

	var entry: String = str(cfg.get_value("mod", "entry", ""))
	if entry.is_empty() or entry.get_extension() != "sgd" or not entry.get_base_dir().is_empty():
		mod_failed.emit(id, "mod.cfg needs an 'entry' naming a .sgd file directly in the mod folder")
		return null

	var entry_path := dir.path_join(entry)
	if not FileAccess.file_exists(entry_path):
		mod_failed.emit(id, "entry script not found: " + entry_path)
		return null

	var source_bytes := FileAccess.get_file_as_bytes(entry_path)
	if source_bytes.size() > MAX_SOURCE_BYTES:
		mod_failed.emit(id, "entry script is larger than %d bytes" % MAX_SOURCE_BYTES)
		return null
	var source := source_bytes.get_string_from_utf8()
	if source.strip_edges().is_empty():
		mod_failed.emit(id, "entry script is empty: " + entry_path)
		return null

	var script := SafeGDScript.new()
	# Attach a harmless program first. This creates the Sandbox so restrictions
	# and limits can be set before the mod's member initializers run.
	script.set_source_code("func __mod_bootstrap():\n\tpass\n")
	var node := Node.new()
	node.name = id
	node.set_meta("mod_path", dir)
	node.set_script(script)
	_restrict(node, cfg, dir)
	var exceptions_before_init := int(node.get("monitor_exceptions"))
	var timeouts_before_init := int(node.get("monitor_execution_timeouts"))

	# Breakpoints use the source path. Set it before compiling the mod.
	script.take_over_path(entry_path)
	script.set_source_code(source)

	var compile_error := script.get_compile_error()
	if not compile_error.is_empty():
		node.free()
		mod_failed.emit(id, "did not compile: " + compile_error)
		return null

	if not _declares(node, "mod_init", 1):
		node.free()
		mod_failed.emit(id, "no mod_init(api) function taking exactly one argument")
		return null

	# Reading a sandbox property creates the real instance record and runs member
	# initializers. Restrictions and limits are already active.
	var exceptions_after_init := int(node.get("monitor_exceptions"))
	var timeouts_after_init := int(node.get("monitor_execution_timeouts"))
	if exceptions_after_init > exceptions_before_init or timeouts_after_init > timeouts_before_init:
		node.free()
		mod_failed.emit(id, "a member initializer raised inside the sandbox")
		return null

	_set_callbacks_enabled(node, true)

	var manifest := {
		"id": id,
		"name": str(cfg.get_value("mod", "name", id)),
		"version": str(cfg.get_value("mod", "version", "0")),
		"author": str(cfg.get_value("mod", "author", "unknown")),
		"description": str(cfg.get_value("mod", "description", "")),
		"path": dir,
	}
	_mods[id] = node

	var api := {}
	if api_provider.is_valid():
		api = api_provider.call(id, manifest)
	freeze(api)

	# Before add_child: Godot runs _enter_tree/_ready during it.
	var exceptions := int(node.get("monitor_exceptions"))
	node.call("mod_init", api)
	if int(node.get("monitor_exceptions")) > exceptions:
		_mods.erase(id)
		node.free()
		mod_failed.emit(id, "mod_init() raised inside the sandbox")
		return null

	_reset_health(id, node)
	add_child(node)

	mod_loaded.emit(id)
	return node

func _process(_delta: float) -> void:
	for id in _mods:
		var node: Node = _mods[id]
		if is_quarantined(id):
			continue
		var health: Dictionary = _health[id]
		var exceptions := int(node.get("monitor_exceptions"))
		var timeouts := int(node.get("monitor_execution_timeouts"))
		var de := exceptions - int(health["exceptions"])
		var dt := timeouts - int(health["timeouts"])
		if de == 0 and dt == 0:
			health["clean"] = int(health["clean"]) + 1
			if int(health["clean"]) >= CLEAN_FRAMES_TO_FORGIVE:
				health["faults"] = 0
				health["clean"] = 0
			continue
		health["exceptions"] = exceptions
		health["timeouts"] = timeouts
		health["clean"] = 0
		# Every timeout also increments monitor_exceptions.
		health["faults"] = int(health["faults"]) + de
		mod_errored.emit(id, de, dt)
		if int(health["faults"]) >= MAX_FAULTS:
			health["quarantined"] = true
			_set_callbacks_enabled(node, false)
			mod_quarantined.emit(id)

func forgive(id: String) -> void:
	var node: Node = _mods.get(id)
	if node != null:
		_reset_health(id, node)

func _reset_health(id: String, node: Node) -> void:
	_health[id] = {
		"exceptions": int(node.get("monitor_exceptions")),
		"timeouts": int(node.get("monitor_execution_timeouts")),
		"faults": 0,
		"clean": 0,
		"quarantined": false,
	}

func _set_callbacks_enabled(node: Node, enabled: bool) -> void:
	node.set_process(enabled and _declares(node, "_process", 1))
	node.set_physics_process(enabled and _declares(node, "_physics_process", 1))
	node.set_process_input(enabled and _declares(node, "_input", 1))
	node.set_process_shortcut_input(enabled and _declares(node, "_shortcut_input", 1))
	node.set_process_unhandled_input(enabled and _declares(node, "_unhandled_input", 1))
	node.set_process_unhandled_key_input(enabled and _declares(node, "_unhandled_key_input", 1))

func _restrict(node: Node, cfg: ConfigFile, dir: String) -> void:
	node.set("restrictions", true)
	node.call("set_resource_allowed_callback", _resource_allowed.bind(dir))
	if property_policy.is_valid():
		node.call("set_property_allowed_callback", _property_allowed.bind(node.name))
	if method_policy.is_valid():
		node.call("set_method_allowed_callback", _method_allowed.bind(node.name))
	node.set("execution_timeout", _limit(cfg, "execution_timeout", MAX_EXECUTION_TIMEOUT, 0))
	node.set("memory_max", _limit(cfg, "memory_max", MAX_MEMORY))
	node.set("allocations_max", _limit(cfg, "allocations_max", MAX_ALLOCATIONS))
	node.set("references_max", _limit(cfg, "references_max", MAX_REFERENCES))
	node.set("coroutines_max", _limit(cfg, "coroutines_max", MAX_COROUTINES))

func _resource_allowed(_sandbox: Object, path: String, dir: String) -> bool:
	return path.simplify_path().begins_with(dir.simplify_path() + "/")

func _property_allowed(_sandbox: Object, object: Object, property: String, is_set: bool, id: String) -> bool:
	return property_policy.call(id, object, property, is_set)

func _method_allowed(_sandbox: Object, object: Object, method: String, id: String) -> bool:
	return method_policy.call(id, object, method)

func _limit(cfg: ConfigFile, key: String, ceiling: int, minimum: int = 1) -> int:
	return clampi(int(cfg.get_value("limits", key, ceiling)), minimum, ceiling)

func _declares(node: Node, function: String, arity: int) -> bool:
	for method in node.get_method_list():
		if method["name"] == function:
			return method["args"].size() == arity
	return false

## Deep make_read_only(). Marks before descending, so the read-only test is also
## the cycle guard.
static func freeze(value: Variant) -> void:
	if value is Dictionary:
		if value.is_read_only():
			return
		value.make_read_only()
		for key in value:
			freeze(value[key])
	elif value is Array:
		if value.is_read_only():
			return
		value.make_read_only()
		for element in value:
			freeze(element)

## Deep-copies a container so the game holds an independent value.
static func detach(value: Variant) -> Variant:
	if value is Dictionary or value is Array:
		return value.duplicate(true)
	return value
