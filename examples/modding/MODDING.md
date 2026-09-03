# Modding

This example loads untrusted SafeGDScript mods with restrictions enabled. Mods
can only access the API and objects provided by the game.

## Mod folders

Each mod has a manifest and one entry script:

```text
my_mod/
├── mod.cfg
├── my_mod.sgd
└── icon.svg        (optional)
```

The loader scans both locations:

- `res://mods/` for built-in mods
- `user://mods/` for player-installed mods

Both locations use the same restrictions.

## Manifest

Create a `mod.cfg` file in the mod folder:

```ini
[mod]
id="cpu"
name="Register CPU"
version="1.0.0"
author="Godot Sandbox"
description="A small register machine."
entry="cpu.sgd"
order=0
requires_api=1

[limits]
execution_timeout=200
memory_max=20
allocations_max=4000
references_max=100
coroutines_max=16
```

The loader applies these rules:

- `id` must be an ASCII identifier. It may contain letters, digits and
  underscores, but it cannot start with a digit.
- Loaded mods must have unique IDs.
- `entry` must name a `.sgd` file in the same folder as `mod.cfg`.
- `entry` cannot contain a directory path.
- `order`, `requires_api` and `[limits]` are optional.

Mods load by ascending `order`, then by `id`. The loader does not resolve mod
dependencies.

`requires_api` is the minimum game API version required by the mod. The loader
rejects a value newer than `ModLoader.API_VERSION`.

## Entry script

The entry script must define `mod_init()` with one argument:

```gdscript
var api : Dictionary = {}

func mod_init(granted : Dictionary) -> void:
	api = granted
	api["log"].call("hello")

func _physics_process(_delta : float) -> void:
	api["report"].call("ticks", 1)
```

Keep the API dictionary. Do not keep one of its `Callable` values separately.

```gdscript
# Correct
api["log"].call("hello")

# Wrong
var log = api["log"]
```

`_init()` runs when the loader creates the instance, but it runs before the loader
has created/passed an API. Use `mod_init()` for mod setup instead.

The loader enables each declared frame or input callback with the matching Node
method:

| Callback | Node method |
| --- | --- |
| `_process(delta)` | `set_process()` |
| `_physics_process(delta)` | `set_physics_process()` |
| `_input(event)` | `set_process_input()` |
| `_shortcut_input(event)` | `set_process_shortcut_input()` |
| `_unhandled_input(event)` | `set_process_unhandled_input()` |
| `_unhandled_key_input(event)` | `set_process_unhandled_key_input()` |

`_notification(what)` is dispatched to the mod like any other callback.

A mod may define `mod_deinit()` with no arguments. The loader calls it before
removing the mod:

```gdscript
func mod_deinit() -> void:
	if marker != null:
		api["despawn"].call(marker)
		marker = null
```

## Example API

`node_2d.gd` returns this API from `_build_api()`:

| Key | Type | Description |
| --- | --- | --- |
| `api_version` | `int` | API version provided by the game |
| `id` | `String` | Mod ID |
| `name` | `String` | Mod name |
| `cycles_per_frame` | `int` | Work limit selected by the game |
| `log` | `Callable` | Writes a tagged log message |
| `report` | `Callable` | Sends a value to the game UI |
| `spawn` | `Callable` | Creates and grants a Label |
| `despawn` | `Callable` | Revokes and frees a granted Label |
| `load` | `Callable` | Loads a resource from the mod folder |

The loader makes the API dictionary and nested arrays and dictionaries
read-only. Return a new API dictionary for each mod. Do not modify it after
returning it. Use a `Callable` for changing data.

Keep API functions small. Limit the amount of data and work accepted by each
call.

`Callable.bind()` appends bound arguments. The example binds the mod ID, so it
is the last argument received by each API function.

## Sandbox limits

The loader accepts these `[limits]` values:

| Key | Maximum | Meaning |
| --- | ---: | --- |
| `execution_timeout` | `200` | Millions of instructions per sandbox call |
| `memory_max` | `32` | Guest memory in MiB |
| `allocations_max` | `8000` | Guest heap allocations |
| `references_max` | `100` | Referenced Variants and Objects |
| `coroutines_max` | `32` | Suspended coroutines |

`execution_timeout` is clamped from 0 to 200. The other values are clamped from
1 to their maximum. A manifest cannot raise a limit above the loader's ceiling.

An `execution_timeout` of 0 disables instruction counting. This selects a
faster emulator dispatch method and generally improves execution speed by about
15%. It also allows a mod's functions to run forever. Closing the game still stops it.

## Object access

Restrictions use separate checks for objects and their APIs:

1. `add_allowed_object()` grants a persistent object handle to the mod.
2. `property_policy` decides which properties the mod may read or write.
3. `method_policy` decides which methods the mod may call.

Granting an object does not grant its properties or methods. An unset policy
denies every property or method.

The example grants a Label returned by `spawn()`:

```gdscript
func _mod_spawn(text: Variant, id: String) -> Object:
	var marker := Label.new()
	add_child(marker)
	loader.get_mod(id).call("add_allowed_object", marker)
	return marker
```

The policy only allows the Label properties used by the CPU mod:

```gdscript
const MARKER_PROPERTIES := ["position", "text"]

func _property_allowed(id: String, object: Object, property: String, _is_set: bool) -> bool:
	return markers.get(id, []).has(object) and MARKER_PROPERTIES.has(property)
```

The example sets no `method_policy`. The CPU mod calls no method on its marker,
so every method stays denied.

Call `remove_allowed_object()` when the grant ends. The allowed object list
keeps `RefCounted` objects alive until they are removed.

## Resource loading

A Resource reaches a mod either through `load()` or through an API function that
returns a resource.

A mod that calls `load()` is checked by the resource callback, by path. The
loader confines it to the mod folder:

```gdscript
func _resource_allowed(_sandbox: Object, path: String, dir: String) -> bool:
	return path.simplify_path().begins_with(dir.simplify_path() + "/")
```

An API function that returns a Resource does not use the resource callback at
all, because the game loaded the file, not the sandbox. What crosses is an
Object, so it needs the same grant as any other object:

```gdscript
func _mod_load(file: Variant, id: String) -> Resource:
	var mod := loader.get_mod(id)
	var name := str(file)
	if name.is_empty() or not name.get_base_dir().is_empty():
		return null
	var res := ResourceLoader.load(mod.get_meta("mod_path").path_join(name))
	if res != null:
		mod.call("add_allowed_object", res)
	return res
```

`ResourceLoader` can load files imported with the project under `res://`.
Files installed under `user://` are not imported. Load supported user files by
content, then grant the created resource.

## Errors and quarantine

`mod_failed(id, reason)` reports load errors, including:

- an invalid manifest
- a missing, empty or oversized entry script
- a compile error
- a missing or invalid `mod_init()`
- an error from a member initializer
- an error from `mod_init()`

Every sandbox error increments `monitor_exceptions`. A timeout also increments
`monitor_execution_timeouts`. The loader checks both counters each frame and
emits `mod_errored(id, exceptions, timeouts)` for new errors. A timeout counts
as one fault for quarantine.

After `MAX_FAULTS`, the loader disables all six frame and input callbacks listed
above and emits `mod_quarantined(id)`. The node stays in the scene tree and its
state is kept. `resume(id)` clears its health counters and re-enables the
callbacks declared by the script.

Quarantine only disables automatic frame and input callbacks. Connected signals
and direct calls can still call mod functions. Check `is_quarantined(id)` before
making such calls.

The fault count is cleared after `CLEAN_FRAMES_TO_FORGIVE` clean frames. Use
`forgive(id)` after an intentional failing call, such as the hostile-mod audit.

`mods/faulty` fails from `_physics_process()`. The loader quarantines it without
stopping the other mods.

## Add the loader to a game

```gdscript
var loader := ModLoader.new()
loader.api_provider = _build_api
loader.property_policy = _property_allowed
loader.mod_loaded.connect(_on_mod_loaded)
loader.mod_failed.connect(_on_mod_failed)
loader.mod_errored.connect(_on_mod_errored)
loader.mod_quarantined.connect(_on_mod_quarantined)
add_child(loader)
loader.scan()
```

`api_provider` receives the mod ID and manifest:

```gdscript
func _build_api(id: String, manifest: Dictionary) -> Dictionary:
	return {
		"api_version": ModLoader.API_VERSION,
		"id": id,
		"name": manifest["name"],
		"log": _mod_log.bind(id),
	}
```

The loader registers the mod before calling `api_provider` and `mod_init()`.
API functions may use `get_mod(id)` during initialization.

## Copy data received from mods

Arrays and dictionaries passed by a mod are shared with the game. A mod can
change them after the API call returns.

Use `ModLoader.detach()` before storing a container:

```gdscript
func _mod_report(key: Variant, value: Variant, id: String) -> void:
	stats[id][key] = ModLoader.detach(value)
```

`detach()` deep-copies an Array or Dictionary. It returns other values
unchanged.

## Hostile-mod audit

With restrictions enabled, the breakout mod cannot:

- access the Object stored in a provided `Callable`
- call blocked methods on its own node
- use parent or root nodes
- read or write blocked properties
- use an Object without a persistent grant
- use a property or method denied by policy
- load a resource outside its folder
- modify the API dictionary

Run the project to execute every `try_*` function in `mods/breakout`. Each call
must increment the sandbox exception counter. The audit then calls `forgive()`
so these expected failures do not count toward quarantine.

## Install a mod

Copy the whole mod folder into `user://mods/`.

For this example on Linux, the path is:

```text
~/.local/share/godot/app_userdata/Useless/mods/
```

Add `mods/*` to **Export > Resources > Filters to export non-resource files**
when exporting built-in mods.
