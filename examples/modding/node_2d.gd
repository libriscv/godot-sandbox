extends Node2D

## The game. Ships no gameplay: the mods under mods/ are compiled and run in
## sandboxes, reaching the game only through _build_api().

const CYCLES_PER_FRAME := 2000

const MAX_STATS_PER_MOD := 8
const MAX_TEXT := 64

@onready var status: Label = $Status

var loader: ModLoader
var stats := {}

func _ready() -> void:
	loader = ModLoader.new()
	loader.name = "Mods"
	loader.api_provider = _build_api
	loader.mod_loaded.connect(_on_mod_loaded)
	loader.mod_failed.connect(func(id: String, why: String): push_warning("[game] mod '%s' failed: %s" % [id, why]))
	add_child(loader)
	print("[game] %d mod(s) loaded" % loader.scan())
	_audit("breakout")

func _on_mod_loaded(id: String) -> void:
	var mod := loader.get_mod(id)
	print("[game] loaded mod '%s': restrictions=%s timeout=%dM instructions memory=%dMB" % [
			id, mod.get("restrictions"), mod.get("execution_timeout"), mod.get("memory_max")])

func _build_api(id: String, manifest: Dictionary) -> Dictionary:
	return {
		"id": id,
		"name": manifest["name"],
		"cycles_per_frame": CYCLES_PER_FRAME,
		"log": _mod_log.bind(id),
		"report": _mod_report.bind(id),
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

func _process(_delta: float) -> void:
	var lines := PackedStringArray()
	for id in loader.get_mod_ids():
		var line : String = str(id)
		for key in stats.get(id, {}):
			line += "  %s=%s" % [key, str(stats[id][key]).left(MAX_TEXT)]
		lines.append(line)
	status.text = "\n".join(lines)

# Call every try_* on the hostile mod. A refused call increments the sandbox
# exception counter; the mod stays loaded.
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
	print("[game] --- audit done, the mod is still running")
