<p align="center">
<img src="https://github.com/libriscv/godot-sandbox/blob/main/banner.png?raw=true" width="312px"/>
</p>
<p align="center">

<p align="center">
        <img src="https://github.com/libriscv/godot-sandbox/actions/workflows/runner.yml/badge.svg?branch=main"
            alt="Godot Sandbox Build"></a>
        <img src="https://img.shields.io/badge/Godot-4.3-%23478cbf?logo=godot-engine&logoColor=white" />
</p>

<p align = "center">
    <strong>
        <a href="https://libriscv.no">Website</a> | <a href="https://github.com/libriscv/godot-sandbox-programs">Code Examples</a> | <a href="https://discord.gg/n4GcXr66X5">Discord</a> | <a href="https://gonzerelli.itch.io/demo">Web Demo</a>
    </strong>
</p>


-----

<p align = "center">
<b>Safe, low-latency and fast sandbox</b>
<i>for the Godot game engine.</i>
</p>

<p align = "center">
	<strong>
  		<a href="https://github.com/user-attachments/files/17729740/Introducing.the.New.Godot.Sandbox.pdf">GodotCon 2024 Presentation</a>
	</strong>
</p>

-----

Godot Sandbox lets players run untrusted code safely. Write gameplay in SafeGDScript, a safe GDScript-dialect executed inside a memory-safe sandbox with limits and execution timeouts. Restricted programs cannot access the host beyond what you explicitly allow, making it safe to load mods, user-generated content and scripts from other players. All Godot platforms are supported.

## Installation

- Automatic (Recommended): Download the plugin from the official [Godot Asset Store](https://godotengine.org/asset-library/asset/3192) using the **AssetLib** tab in Godot by searching for **Godot Sandbox**.

- Manual: Download the [latest github release](https://github.com/libriscv/godot-sandbox/releases/latest) and move only the **addons** folder into your project **addons** folder.

## SafeGDScript

SafeGDScript (`.sgd`) is the default language for sandboxed code. It is a [safety-oriented GDScript-dialect](https://libriscv.no/docs/host_langs/godot_integration/godot_intro/safegdscript) with most of the same syntax, and some additions (like structs). Attach a `.sgd` file to any node the same way you would attach a `.gd` script:

VS Code users can install the [SafeGDScript extension](https://marketplace.visualstudio.com/items?itemName=AlfAndrWalla.vscode-safegdscript) for syntax highlighting and basic editor support.

SafeGDScript supports reusable traits. A trait can contribute state, constants,
enums, signals and concrete/static methods, while abstract methods state what
the using class must provide:

```gdscript
uses Damageable

trait Damageable:
	var health: int = 100
	func take_damage(amount: int) -> void:
		health -= amount
	@abstract func on_death() -> void

func on_death() -> void:
	queue_free()
```

Traits are nominal for SafeGDScript classes (`value is Damageable`) and may be
used as type hints. Foreign Godot objects can satisfy a trait structurally by
providing every instance method declared by it. Disable that compatibility path
with `safe_gdscript/traits/structural_fallback` for strictly nominal matching.

```gdscript
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

func _ready():
	add_item("coin", 1)
	add_item("gem", 5)
	add_item("coin", 1)
	inventory[0].drop(Vector2(64, 32))
```

SafeGDScript also supports `await`:

```gdscript
func cutscene(player_knocked : Signal) -> String:
	$Gate.text = "The gate is sealed."
	await get_tree().create_timer(0.8).timeout
	await player_knocked
	return "opened"
```

The host receives a Signal and awaits it like any other coroutine: `var result = await $Director.cutscene(knocked)`. See [examples/async](examples/async) for a complete example.

### Modding and user-generated content

A restricted sandbox denies all host access by default: methods, properties, classes and resource loading. Gamedevs decide what a mod can reach by passing an explicit API:

```gdscript
var api : Dictionary = {}

func mod_init(granted : Dictionary) -> void:
	api = granted
	api["log"].call("hello from the mod")

func _physics_process(delta):
	api["report"].call("ticks", 1)
```

See [examples/modding](examples/modding) for a complete mod loader with a hostile-mod audit.

## C++ and Rust

For maximum performance, you can also write sandboxed programs in C++ or Rust. They use the same sandbox and the same restrictions. See the [code examples repository](https://github.com/libriscv/godot-sandbox-programs) and the [demo repository](https://github.com/libriscv/godot-sandbox-demo).

## Performance

- Sandboxed C++ is [2.5-10x faster than GDScript](https://libriscv.no/docs/benchmarks/performance) by default, 5-50x with binary translation
- Enable [full binary translation](https://libriscv.no/docs/host_langs/godot_integration/godot_docs/bintr) for maximum performance on all platforms (including locked down iOS, Web, Switch etc.)
- JIT builds are available in the Releases section for Windows, macOS, Android and Linux

Using typed variables in SafeGDScript will help the compiler optimize:

```
--- logic CPU dispatch ---
case                               ns/emulated instr     vs ref    vs base
SafeGDScript (sandbox)                         115.4      5.36x      -4.1%
GDScript (engine)                              618.0      1.00x      -7.2%
```

In the modding example that implements a virtual CPU, we gained 5x over GDScript by using typed variables.

## Usage

- [Assign an ELF script resource directly to a node](https://libriscv.no/docs/host_langs/godot_integration/godot_intro/sandbox#using-programs-directly-as-scripts). Constructs a shared sandbox among all instances with that script, maximum scalability, call functions and attach signals like GDScript

- Or, [create a Sandbox node and assign the ELF resource to it](https://libriscv.no/docs/host_langs/godot_integration/godot_intro/sandbox#creating-a-sandbox). One sandbox per node, with auto-completion from other GDScripts using @export

## Module Build

In order to build as a module, add it to a godot repo:

```
git submodule add https://github.com/libriscv/godot-sandbox modules/sandbox
cd modules/sandbox
git submodule update --init --recursive
```

## Contributing

Requirements:
- [SCons](https://www.scons.org)
- [python3](https://www.python.org)

If you want to contribute to this repo, here are steps on how to build locally:

```sh
./build.sh
```

You can also use `scons` similar to how godot-cpp addons are built.

## Contributors

Thanks goes to these wonderful people ([emoji key](https://allcontributors.org/docs/en/emoji-key)):

<!-- ALL-CONTRIBUTORS-LIST:START - Do not remove or modify this section -->
<!-- prettier-ignore-start -->
<!-- markdownlint-disable -->
<table>
  <tbody>
    <tr>
      <td align="center" valign="top" width="14.28%"><a href="https://github.com/fwsGonzo"><img src="https://avatars.githubusercontent.com/u/3758947?v=4?s=100" width="100px;" alt="Alf-André Walla"/><br /><sub><b>Alf-André Walla</b></sub></a><br /><a href="https://github.com/libriscv/godot-sandbox/commits?author=fwsGonzo" title="Code">💻</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://chibifire.com"><img src="https://avatars.githubusercontent.com/u/32321?v=4?s=100" width="100px;" alt="K. S. Ernest (iFire) Lee"/><br /><sub><b>K. S. Ernest (iFire) Lee</b></sub></a><br /><a href="https://github.com/libriscv/godot-sandbox/commits?author=fire" title="Code">💻</a> <a href="#research-fire" title="Research">🔬</a> <a href="https://github.com/libriscv/godot-sandbox/commits?author=fire" title="Tests">⚠️</a></td>
      <td align="center" valign="top" width="14.28%"><a href="https://appsinacup.com"><img src="https://avatars.githubusercontent.com/u/2369380?v=4?s=100" width="100px;" alt="Dragos Daian"/><br /><sub><b>Dragos Daian</b></sub></a><br /><a href="https://github.com/libriscv/godot-sandbox/commits?author=Ughuuu" title="Code">💻</a></td>
    </tr>
  </tbody>
</table>

<!-- markdownlint-restore -->
<!-- prettier-ignore-end -->

<!-- ALL-CONTRIBUTORS-LIST:END -->

This project follows the [all-contributors](https://github.com/all-contributors/all-contributors) specification. Contributions of any kind welcome!

## Other Projects

The [Jenova Framework](https://github.com/Jenova-Framework/) has a built-in C++ compiler, and supports writing C++ in the Godot editor with hot-reloading support.
