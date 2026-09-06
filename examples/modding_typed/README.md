# Typed mods

`SgdModLoader` is available in both the native engine module and the BSD
extension. This example is a SafeGDScript game with a typed API object and one
mod. Install the upstream addon (including its compiler ELF for restricted
compilation), or open the project with a module editor, and run it. The example
prints `CHECK typed_export=true` and exits after loading, calling and unloading.
The original [Dictionary/Callable example](../modding/MODDING.md) is unchanged.

## Contract and loading

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

Each mod gets a fresh script resource and restricted Sandbox. Limits, object
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

## Settings

All keys have the `sandbox/mods/` prefix.

| Key | Default / meaning |
| --- | --- |
| `obligations_trait`, `api_trait` | Paths to the two SDK traits. Required for loading |
| `directories` | `res://mods`, `user://mods`. Scan immediate subdirectories |
| `entry_form` | `auto`, `source`, or `elf`. Auto accepts ELF entries on both hosts and source where the compiler is present |
| `api_version` | `1`. reject a higher manifest `requires_api` |
| `node_type` | `Node`. `Node2D` is available for spatial mod hosts |
| `limits/execution_timeout` | 1 million instructions per call |
| `limits/memory_max` | 32 MiB |
| `limits/allocations_max` | 8000 |
| `limits/references_max` | 100 |
| `limits/coroutines_max` | 32 |
| `restricted` | Module export: `mods/**`, comma-separated project-relative globs |
| `runtime_compiler_template` | Module export: template to select for explicit source mode |
| `allow_concrete_sdk_methods` | Module export: false. Opt in only when trait method bodies may be public |

Manifest `[mod]` fields are `id`, `name`, `version`, `entry`, `order`, and
`requires_api`. `[limits]` can only lower the project ceilings (zero clamps to
one). IDs are ASCII identifiers, entries are basenames in the mod directory,
and scan sorts by order then ID. Source entries are limited to 256 KiB, ELF
entries to 16 MiB. There is no dependency resolver. The game may supply a
`source` string in the load dictionary for an in-memory editor.

## Release and SDK

The module exporter writes the text traits, a manifest template and generated
method documentation into `mods_sdk/` beside the build. Public trait sources
also remain in the PCK, with metadata-bearing ELF siblings for compiler-free
validation. Concrete SDK methods fail export unless explicitly allowed.
Restricted mod globs compile with restricted semantics and bake into their own
directory with checked memory. Normal game scripts keep the release policy.

Source mode needs a selected template built with `sgd_runtime_compiler=yes`.
Set the custom template in the export preset or set
`sandbox/mods/runtime_compiler_template`. Export refuses if the selected binary
(or Web template ZIP) does not contain a runtime compiler. Default compiler-free
templates use ELF entries. Auto export rewrites packaged source manifests to
ELF entries when selecting such a template. A player-supplied ELF need not have
a native translation: the runtime safely interprets it if no compatible checked
translation exists. The module SDK/export integration is separate from the BSD
extension's existing export workflow.
