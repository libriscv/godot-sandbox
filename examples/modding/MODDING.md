# Mods

A mod is a folder with a `mod.cfg` manifest and a `.sgd` entry file. The loader
compiles it to RISC-V at startup and runs it in a sandbox with all host access
denied. The mod can only reach what the game puts in the Dictionary passed to
`mod_init()`.

```
mods/cpu/mod.cfg          shipped with the game
mods/cpu/cpu.sgd
user://mods/<id>/...      installed by a player
```

Both paths load identically. Neither is trusted.

## Contract

```gdscript
var api : Dictionary = {}

func mod_init(granted : Dictionary) -> void:
    api = granted
    api["log"].call("hello")

func _physics_process(delta):
    api["report"].call("ticks", 1)
```

`mod_init(api)` is required. A mod without it is rejected (same as a compile
failure: no program, no methods).

Standard callbacks (`_ready`, `_process`, `_physics_process`) work normally.

**Hold the Dictionary, not the Callable.** A Dictionary global persists between
calls; a bare Callable global does not. Write `api["log"].call(x)`, not
`var log = api["log"]` at file scope.

## API surface

`_build_api()` in `node_2d.gd` defines the entire host surface:

| Key                | Type       | Purpose                       |
| ------------------ | ---------- | ----------------------------- |
| `id`, `name`       | String     | From manifest                 |
| `cycles_per_frame` | int        | Budget for the mod            |
| `log`              | Callable   | Tagged output line            |
| `report`           | Callable   | Publish a stat the game shows |

No game nodes are exposed. Callables can be called, not unwrapped.

## Denied operations

`mods/breakout` is a hostile mod shipped with the game. Each `try_*` function
attempts one forbidden operation. All six are refused:

| Attempt                            | Blocked by              |
| ---------------------------------- | ----------------------- |
| `api["log"].get_object()`          | Object never crosses    |
| `set_name()` on own node           | Method denied           |
| `get_parent().call(...)`           | Method denied           |
| `get_node("/root").call(...)`      | Method denied           |
| Read `get_node("/root").name`      | Property denied         |
| Write `get_node("/root").name`     | Property denied         |

`get_node()` returns a handle, but every method and property on it is denied.
`load("res://...")` is a compile error: no program is produced.

A refused call raises inside the guest and unwinds that call. The mod stays
loaded.

## Limits

`mod.cfg` may request limits under `[limits]`. The loader clamps each to a
ceiling defined in `mod_loader.gd`. A manifest can only ask for less.

`execution_timeout` is millions of instructions per call. Syscalls are charged
against it (one Array operation costs ~50k instructions). Turning `profiling`
on removes syscall charges; the loader leaves it off.

## Installing a mod

Drop the folder into `user://mods/`. On Linux:
`~/.local/share/godot/app_userdata/Useless/mods/`

For exports, add `mods/*` to *Export > Resources > Filters to export
non-resource files*.
