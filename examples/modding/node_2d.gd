extends Node2D

## Host for the modding example. _build_api() defines all mod access.

const CYCLES_PER_FRAME := 2000

const MAX_STATS_PER_MOD := 8
const MAX_TEXT := 64
const MAX_MARKERS_PER_MOD := 4

@onready var status: Label = $Status

var loader: ModLoader
var stats := {}

var markers := {}

func _ready() -> void:
	loader = ModLoader.new()
	loader.name = "Mods"
	loader.api_provider = _build_api
	loader.property_policy = _property_allowed
	loader.mod_loaded.connect(_on_mod_loaded)
	loader.mod_failed.connect(_on_mod_failed)
	loader.mod_errored.connect(_on_mod_errored)
	loader.mod_quarantined.connect(_on_mod_quarantined)
	add_child(loader)
	print("[game] %d mod(s) loaded" % loader.scan())
	_audit("breakout")

func _exit_tree() -> void:
	for id in markers:
		for marker in markers[id]:
			if is_instance_valid(marker):
				marker.queue_free()

var reported := {}

func _on_mod_errored(id: String, exceptions: int, timeouts: int) -> void:
	if reported.has(id):
		return
	reported[id] = true
	push_warning("[game] mod '%s' faulted: %d exception(s), %d timeout(s); further faults not reported"
			% [id, exceptions, timeouts])

func _on_mod_quarantined(id: String) -> void:
	reported.erase(id)
	push_warning("[game] mod '%s' quarantined: it faulted too often" % id)

func _on_mod_failed(id: String, why: String) -> void:
	push_warning("[game] mod '%s' failed: %s" % [id, why])
	for marker in markers.get(id, []):
		if is_instance_valid(marker):
			marker.queue_free()
	markers.erase(id)

func _on_mod_loaded(id: String) -> void:
	var mod := loader.get_mod(id)
	print("[game] loaded mod '%s': restrictions=%s timeout=%dM instructions memory=%dMB" % [
			id, mod.get("restrictions"), mod.get("execution_timeout"), mod.get("memory_max")])

func _build_api(id: String, manifest: Dictionary) -> Dictionary:
	return {
		"api_version": ModLoader.API_VERSION,
		"id": id,
		"name": manifest["name"],
		"cycles_per_frame": CYCLES_PER_FRAME,
		"log": _mod_log.bind(id),
		"report": _mod_report.bind(id),
		"spawn": _mod_spawn.bind(id),
		"despawn": _mod_despawn.bind(id),
		"load": _mod_load.bind(id),
	}

# Callable.bind() appends: the bound id arrives last.
func _mod_log(message: Variant, id: String) -> void:
	print("[%s] %s" % [id, str(message).left(MAX_TEXT)])

func _mod_report(key: Variant, value: Variant, id: String) -> void:
	if not stats.has(id):
		stats[id] = {}
	var mod_stats: Dictionary = stats[id]
	var name := str(key).left(MAX_TEXT)
	if not mod_stats.has(name) and mod_stats.size() >= MAX_STATS_PER_MOD:
		return
	mod_stats[name] = ModLoader.detach(value)

func _mod_spawn(text: Variant, id: String) -> Object:
	var owned: Array = markers.get(id, [])
	if owned.size() >= MAX_MARKERS_PER_MOD:
		return null
	var marker := Label.new()
	marker.text = str(text).left(MAX_TEXT)
	marker.position = Vector2(16, 64 + 24 * owned.size())
	add_child(marker)
	loader.get_mod(id).call("add_allowed_object", marker)
	owned.append(marker)
	markers[id] = owned
	return marker

func _mod_despawn(marker: Variant, id: String) -> bool:
	var owned: Array = markers.get(id, [])
	if not (marker is Object) or not owned.has(marker):
		return false
	owned.erase(marker)
	loader.get_mod(id).call("remove_allowed_object", marker)
	marker.queue_free()
	return true

func _mod_load(file: Variant, id: String) -> Resource:
	var mod := loader.get_mod(id)
	if mod == null:
		return null
	var name := str(file)
	if name.is_empty() or not name.get_base_dir().is_empty():
		return null
	var res := ResourceLoader.load(mod.get_meta("mod_path").path_join(name))
	if res != null:
		mod.call("add_allowed_object", res)
	return res

const MARKER_PROPERTIES := ["position", "text"]

func _property_allowed(id: String, object: Object, property: String, _is_set: bool) -> bool:
	return markers.get(id, []).has(object) and MARKER_PROPERTIES.has(property)

func _process(_delta: float) -> void:
	var lines := PackedStringArray()
	for id in loader.get_mod_ids():
		var line : String = str(id)
		if loader.is_quarantined(id):
			line += "  [quarantined]"
		for key in stats.get(id, {}):
			line += "  %s=%s" % [key, str(stats[id][key]).left(MAX_TEXT)]
		lines.append(line)
	status.text = "\n".join(lines)

# Call every try_* on the hostile mod. A refused call increments the sandbox
# exception counter. The mod stays loaded.
func _audit(id: String) -> void:
	var mod := loader.get_mod(id)
	if mod == null:
		return
	print("[game] --- sandbox audit of mod '%s'" % id)
	for method in mod.get_method_list():
		var name: String = method["name"]
		if not name.begins_with("try_"):
			continue
		var before := int(mod.get("monitor_exceptions"))
		var result = mod.call(name)
		var denied := int(mod.get("monitor_exceptions")) > before
		print("[game]   %-24s %s" % [name, "DENIED" if denied else "ALLOWED -> " + str(result)])
	loader.forgive(id)
	print("[game] --- audit done, the mod is still running")
