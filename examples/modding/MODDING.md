# Modding

This example loads GDScript mods in a restricted sandbox. A mod can only use
the values and functions that the game gives it.

## Mod folder

Each mod needs two files:

```text
my_mod/
├── mod.cfg
└── my_mod.sgd
```

The loader checks these locations:

- `res://mods/` for mods included with the game
- `user://mods/` for mods installed by a player

Both locations use the same restrictions.

## Manifest

Create `mod.cfg` in the mod folder:

```ini
[mod]
id="cpu"
name="Register CPU"
version="1.0.0"
author="Godot Sandbox"
description="A small register machine."
entry="cpu.sgd"

[limits]
execution_timeout=200
memory_max=20
```

Rules:

- `id` may contain letters, digits, and underscores.
- `id` must not start with a digit.
- Each loaded mod must have a unique `id`.
- `entry` must be a `.sgd` filename in the same folder as `mod.cfg`.
- `entry` must not contain a directory path.
- `[limits]` is optional.

## Entry script

The entry script must define `mod_init()` with one argument:

```gdscript
var api: Dictionary = {}

func mod_init(granted: Dictionary) -> void:
    api = granted
    api["log"].call("hello")

func _physics_process(_delta: float) -> void:
    api["report"].call("ticks", 1)
```

Keep the whole API dictionary in a global variable. Do not save one of its
`Callable` values in a global variable.

Use:

```gdscript
api["log"].call("hello")
```

Do not use:

```gdscript
var log = api["log"]
```

The loader calls `mod_init()` before the mod enters the scene tree. The API is
therefore available in these supported callbacks:

- `_enter_tree()`
- `_ready()`
- `_process()`
- `_physics_process()`
- `_exit_tree()`

`_notification()` is not supported for `.sgd` mods.

## Example API

The game defines the API in `_build_api()` in `node_2d.gd`.

| Key | Type | Description |
| --- | --- | --- |
| `id` | `String` | Mod ID from `mod.cfg` |
| `name` | `String` | Mod name from `mod.cfg` |
| `cycles_per_frame` | `int` | Work limit chosen by the game |
| `log` | `Callable` | Writes a tagged log message |
| `report` | `Callable` | Sends a value to the game UI |

The mod cannot access game nodes unless the game explicitly provides access.

## Errors

The loader rejects a mod when:

- `mod.cfg` cannot be read
- `id` or `entry` is invalid
- the entry file is missing, empty, or too large
- the script does not compile
- `mod_init()` is missing or has the wrong number of arguments
- `mod_init()` causes a sandbox error

The reason is sent through the `mod_failed` signal.

An error after loading stops the current mod call. The mod stays loaded. Common
causes are:

- calling a method that does not exist
- passing the wrong number or type of arguments
- using a missing API key, such as `api["missing"].call()`
- trying to use a blocked method, property, or resource
- exceeding a sandbox limit

## Sandbox limits

The optional `[limits]` section supports these keys:

| Key | Maximum | Meaning |
| --- | ---: | --- |
| `execution_timeout` | `200` | Millions of instructions allowed per call |
| `memory_max` | `32` | Memory in MB |
| `allocations_max` | `8000` | Allocations |
| `references_max` | `100` | Object references |
| `coroutines_max` | `32`  | Max coroutines |

Values are clamped from `1` to the maximum. A mod may request a lower value but
cannot raise the maximum. When the execution timeout is set to `0`, it is no
longer in effect, but scripts will generally execute ~15% faster.

## Resource loading

Resource paths are blocked by default. The game can allow selected paths:

```gdscript
mod.call("set_resource_allowed_callback", func(_sandbox, path):
    return path.begins_with("res://mods/shared/")
)
```

Only allow paths that mods need.

## Add the loader to a game

Create a loader, provide the API builder, connect its signals, and scan for
mods:

```gdscript
var loader := ModLoader.new()
loader.api_provider = _build_api
loader.mod_loaded.connect(_on_mod_loaded)
loader.mod_failed.connect(_on_mod_failed)
add_child(loader)
loader.scan()
```

The API builder receives the mod ID and its manifest:

```gdscript
func _build_api(id: String, manifest: Dictionary) -> Dictionary:
    return {
        "id": id,
        "name": manifest["name"],
        "log": _mod_log.bind(id),
    }
```

Loader rules:

- Return a new API dictionary for every mod.
- The loader makes the API dictionary and all nested arrays and dictionaries
  read-only.
- The game must not change an API dictionary after returning it.
- Provide a `Callable` when a mod needs changing data.
- Keep API functions small and limit how much data or work each call accepts.

## Copy data received from mods

Arrays and dictionaries passed by a mod are shared objects. The mod can change
them after the game receives them.

Copy them before storing them:

```gdscript
func _mod_report(key: Variant, value: Variant, id: String) -> void:
    stats[id][key] = ModLoader.detach(value)
```

`ModLoader.detach()` makes a deep copy of an `Array` or `Dictionary`. Other
values are returned unchanged.

## Blocked operations

With restrictions enabled, a mod cannot:

- get the object stored inside a provided `Callable`
- call blocked methods on its node
- access parent or root nodes
- read or write blocked object properties
- load a resource unless its path is allowed
- add, replace, or remove values in the API dictionary

The `mods/breakout` example tries these operations. The sandbox blocks them and
keeps the mod loaded.

## Install a mod

Copy the whole mod folder into `user://mods/`.

For this example on Linux, that folder is:

```text
~/.local/share/godot/app_userdata/Useless/mods/
```

To include built-in mods in an exported game, add this export filter:

```text
mods/*
```

Set it in **Export > Resources > Filters to export non-resource files**.
