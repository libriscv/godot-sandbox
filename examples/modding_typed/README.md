# Traits-based modding system

SafeGDScript supports traits, and with that Godot Sandbox has special support
for traits-based modding APIs. The traits-based mod loader supplies mods with
two separate traits which say something like: “Here is what your mod must do,
and here is what it is allowed to ask the game to do.”

A trait contains typed functions, the args they accept, and the results they
return. The compiler checks those agreements while the mod is being built, and
then again before running `mod_init`. In other words, a mod that doesn't
implement required methods will fail compilation. An independently compiled
ELF with incompatible declarations is refused before its initializers run.

The previous Dictionary-based modding example could not use/expose fully typed
functions by itself, nor would it fail compilation if any requirement was
missing. And finally, it could not tell the game that it needs a function to
be called in order to work properly. Of course, it's a simpler approach and
still works, but it's not a self-documenting API.

Only `@abstract` functions in the trait are required to be implemented by mod
authors:

```gdscript
trait_name ModBrain

@abstract func mod_init(api: ArenaAPI) -> void
@abstract func think(delta: float) -> Vector2

func on_round_finished(won: bool) -> void:
    pass
```

Here, both `mod_init` and `think` have to be implemented by the mod author or
it will fail compilation. The API also has an `on_round_finished` that does
nothing, and if a mod implements it, the mods function is the one that gets
called (for each mod that implements it).

On the game side it would be something like this:
```gdscript
if mod.has_method("on_round_finished"):
    mod.call("on_round_finished", won)
```

The benefit is better tooling, clear contracts, and automatically derived
API permissions.

## API contract and loading

`mods_sdk/mod_brain.sgd` declares what a mod must implement.
`mods_sdk/arena_api.sgd` declares the modding API (to the game). A mod writes
`uses ModBrain`, retains the typed API passed to `mod_init`, and implements the
required methods. The compiler checks trait calls and obligations. The loader
checks signatures again from ELF metadata, including independently compiled
ELFs that never used the trait. `SafeGDScript.uses_trait(name)` reads nominal
membership. `script.conforms_to(trait_resource)` returns `{ok, errors}` for the
primary trait declared by that resource.

```gdscript
var loader = SgdModLoader.new()
add_child(loader)
for manifest in loader.scan():
    var mod = loader.load(manifest, api, self)
    if mod == null:
        push_error(loader.get_last_error())
```

Each mod gets a fresh script resource and a restricted Sandbox. Limits, object
grants and the derived method policy are installed before initializers run.
Only methods declared by the API trait on the exact granted API object are
allowed. Type erasure does not bypass the runtime policy. Class, resource and
property access remain denied. API methods must not return additional engine
objects unless the game deliberately wants to expand its capabilities.

`mod_faulted(id, reason, location)` reports a quarantined mod, including an
instruction timeout. The loader disables callbacks and detaches it, retaining
it until `unload(id)`. It never restarts it. Faults are polled each frame. A game
can call `poll_faults()` immediately after invoking its own gameplay method.
`get_refusals(id)` counts method-policy denials. `mod_refused(id, method)` is
emitted deferred. `mod_failed(id, reason)` reports load errors. Compiler line
and column are available through `get_error_line()` / `get_error_column()` in
the module. `unload(id)` calls optional `mod_deinit()` unless quarantined.
Unload explicitly when replacing a mod. The loader also owns final cleanup.

## Older mods, newer APIs

Adding new methods to `ArenaAPI` will not stop older mods from working.
Adding new required `@abstract` callbacks to `ModBrain` will refuses mods,
new or old, that are missing those callbacks. The game should be checking
if a mod has a given method before calling for any concrete (non-abstract)
function, which means it will simply be skipped for both older and newer
mods that does not implement it.

The loader compares the API trait recorded in a mod's ELF with the current
game's offered trait. Every recorded method must still exist with a compatible
signature. The host may accept broader argument types and return narrower
types; new methods, declaration order and parameter names do not cause a
refusal. Typed Array and Dictionary element types must match. Native class
compatibility follows Godot's inheritance hierarchy. This checks the whole
recorded trait, including methods the mod never calls, without a version number
or byte-for-byte SDK comparison. Untyped ELFs without a recorded API trait
still get callback validation and runtime restrictions.

If the modding API needs to change in drastic ways, there are many ways to
go about it, but a clean separation between older and newer can be handled at
compile-time by adding new/different required abstract functions. Every game
goes through this, and everyone does their best not to break old working
things, but sometimes the game itself changes fundamentally underneath to
support new big features for updates, and mods need to update as well.

## Newer mods, older APIs

If a mod tries to call an API function the game does not have, it will fail
at compile time when built against that game's SDK. If it was compiled against
a different SDK, the loader checks its recorded API requirements before any
guest code runs.

Separately, a mod can ask that the game should call a given function:

```gdscript
uses ModBrain

@requires_host_hook
func on_round_finished(won: bool) -> int:
    return 1 if won else 0
```

That function would exist at the time the mod was made, and for the game
version the mod was made for. If the function is never going to get called
by the game, the mod loader will refuse to load the mod. There can be two
reasons: The game is too old and never had it, or too new and deprecated
and then removed it. The mod loader will print an error with the function
it never intended to call, and fail/quarantine the mod.

The game declares callback support by including it in its obligations trait.
The game should dispatch it under the documented conditions, such as when a
round finishes. The loader does not generate dispatch code or require an event
to occur. The requirement is preserved in precompiled ELFs.

## Settings

All keys have the `sandbox/mods/` prefix.

| Key | Default / meaning |
| --- | --- |
| `obligations_trait`, `api_trait` | Paths to the two SDK traits. Required for loading |
| `directories` | `res://mods`, `user://mods`. Scan immediate subdirectories |
| `entry_form` | `auto`, `source`, or `elf`. Auto accepts ELF entries on both hosts and source where the compiler is present |
| `node_type` | `Node`. `Node2D` is available for spatial mod hosts |
| `limits/execution_timeout` | 1 million instructions per call |
| `limits/memory_max` | 32 MiB |
| `limits/allocations_max` | 8000 |
| `limits/references_max` | 100 |
| `limits/coroutines_max` | 32 |
| `restricted` | Module export: `mods/**`, comma-separated project-relative globs |
| `runtime_compiler_template` | Module export: template to select for explicit source mode |

Manifest `[mod]` fields are `id`, `name`, `version`, `entry`, and `order`. `[limits]` can only lower the project ceilings (zero clamps to
one). IDs are ASCII identifiers, entries are basenames in the mod directory,
and scan sorts by order then ID. Source entries are limited to 256 KiB, ELF
entries to 16 MiB. There is no dependency resolver. The game may supply a
`source` string in the load dictionary for an in-memory editor.

## Release and SDK

`SgdModLoader` is available in both the native engine module (GS+) and the BSD
extension. This example is a SafeGDScript game with a typed API object and one
mod. Install the upstream addon (including its compiler ELF for restricted
compilation), or open the project with a module editor, and run it. The example
prints `CHECK typed_export=true` and exits after loading, calling and unloading.

The module exporter writes the text traits, a manifest template and generated
method documentation into `mods_sdk/` beside the build. Public trait sources
also remain in the PCK, with metadata-bearing ELF siblings for compiler-free
validation. SDK traits are public, including concrete method bodies. Abstract
methods require implementations, while concrete methods provide defaults that
mods may override.

Source mode needs a selected template built with `sgd_runtime_compiler=yes`.
Set the custom template in the export preset or set
`sandbox/mods/runtime_compiler_template`. Export refuses if the selected binary
(or Web template ZIP) does not contain a runtime compiler. Default compiler-free
templates use ELF entries. Auto export rewrites packaged source manifests to
ELF entries when selecting such a template. A player-supplied ELF need not have
a native translation: the runtime safely interprets it if no compatible checked
translation exists. The module SDK/export integration is separate from the BSD
extension's existing export workflow.
