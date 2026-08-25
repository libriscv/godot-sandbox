class_name ModLoader
extends Node

## Loads .sgd mods into fully restricted sandboxes.
##
## A mod folder has a mod.cfg manifest and a .sgd entry file. The entry is
## compiled to RISC-V at load time and runs with all host access denied.
## The mod can only reach what the host puts in the Dictionary passed to
## mod_init(). The Dictionary is frozen before it crosses.

signal mod_loaded(id: String)
signal mod_failed(id: String, reason: String)

const MOD_DIRS := ["res://mods", "user://mods"]

# Ceilings. A manifest can lower these, never raise them.
const MAX_EXECUTION_TIMEOUT := 200  # millions of instructions per call
const MAX_MEMORY := 32              # MB
const MAX_ALLOCATIONS := 8000
const MAX_REFERENCES := 100
const MAX_SOURCE_BYTES := 256 * 1024

# Called as api_provider.call(id, manifest) -> Dictionary.
# The returned Dictionary is frozen; build a fresh one per mod.
var api_provider := Callable()

var _mods := {}

func scan() -> int:
	var count := 0
	for root in MOD_DIRS:
		if not DirAccess.dir_exists_absolute(root):
			continue
		for dir_name in DirAccess.get_directories_at(root):
			if load_mod(root.path_join(dir_name)) != null:
				count += 1
	return count

func get_mod(id: String) -> Node:
	return _mods.get(id)

func get_mod_ids() -> Array:
	return _mods.keys()

func unload(id: String) -> void:
	var node: Node = _mods.get(id)
	if node == null:
		return
	_mods.erase(id)
	node.queue_free()

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

	var entry: String = str(cfg.get_value("mod", "entry", ""))
	if entry.is_empty() or entry.get_extension() != "sgd" or not entry.get_base_dir().is_empty():
		mod_failed.emit(id, "mod.cfg needs an 'entry' naming a .sgd file directly in the mod folder")
		return null

	var entry_path := dir.path_join(entry)
	if not FileAccess.file_exists(entry_path):
		mod_failed.emit(id, "entry script not found: " + entry_path)
		return null

	var source := FileAccess.get_file_as_string(entry_path)
	if source.length() > MAX_SOURCE_BYTES:
		mod_failed.emit(id, "entry script is larger than %d bytes" % MAX_SOURCE_BYTES)
		return null
	if source.strip_edges().is_empty():
		mod_failed.emit(id, "entry script is empty: " + entry_path)
		return null

	var script := SafeGDScript.new()
	# Built here, not loaded, so the script has no resource path of its own.
	# Name the file it came from: the editor's breakpoint gutter is keyed by
	# path, and a stop needs a file for the editor to open. Before the source,
	# because setting the source is what compiles, and the breakpoints have to
	# be in that first build -- adding them later reloads the program, and a mod
	# comes back with its mod_init() undone.
	script.take_over_path(entry_path)
	script.set_source_code(source)

	var compile_error := script.get_compile_error()
	if not compile_error.is_empty():
		mod_failed.emit(id, "did not compile: " + compile_error)
		return null

	var node := Node.new()
	node.name = id
	# Attach before add_child: Godot queries callbacks at tree entry.
	node.set_script(script)
	_restrict(node, cfg)

	if not _declares_mod_init(node):
		node.free()
		mod_failed.emit(id, "no mod_init(api) function taking exactly one argument")
		return null

	var manifest := {
		"id": id,
		"name": str(cfg.get_value("mod", "name", id)),
		"version": str(cfg.get_value("mod", "version", "0")),
		"author": str(cfg.get_value("mod", "author", "unknown")),
		"description": str(cfg.get_value("mod", "description", "")),
		"path": dir,
	}
	var api := {}
	if api_provider.is_valid():
		api = api_provider.call(id, manifest)
	freeze(api)

	# Before add_child: Godot runs _enter_tree/_ready during it.
	var exceptions := int(node.get("monitor_exceptions"))
	node.call("mod_init", api)
	if int(node.get("monitor_exceptions")) > exceptions:
		node.free()
		mod_failed.emit(id, "mod_init() raised inside the sandbox")
		return null

	add_child(node)

	_mods[id] = node
	mod_loaded.emit(id)
	return node

func _restrict(node: Node, cfg: ConfigFile) -> void:
	node.set("restrictions", true)
	node.set("execution_timeout", _limit(cfg, "execution_timeout", MAX_EXECUTION_TIMEOUT))
	node.set("memory_max", _limit(cfg, "memory_max", MAX_MEMORY))
	node.set("allocations_max", _limit(cfg, "allocations_max", MAX_ALLOCATIONS))
	node.set("references_max", _limit(cfg, "references_max", MAX_REFERENCES))

func _limit(cfg: ConfigFile, key: String, ceiling: int) -> int:
	return clampi(int(cfg.get_value("limits", key, ceiling)), 1, ceiling)

func _declares_mod_init(node: Node) -> bool:
	for method in node.get_method_list():
		if method["name"] == "mod_init":
			return method["args"].size() == 1
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
