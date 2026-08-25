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

```gdscript
struct Item:
	var name : String = ""
	var value : int = 0
	var stackable : bool = true

var inventory : Array = []

func add_item(item_name : String, item_value : int) -> bool:
	for item in inventory:
		match item:
			{"name": var n, "stackable": true, ..} when n == item_name:
				item.value += item_value
				return true
	inventory.append(Item.new(item_name, item_value))
	return false

func get_total_value() -> int:
	var total = 0
	for item in inventory:
		total += item.value
	return total

func _ready():
	add_item("coin", 1)
	add_item("gem", 5)
	add_item("coin", 1)
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

```
Using typed variables in SafeGDScript will help the compiler optimize:

--- int loop ---
case                               ns/iteration     vs ref    vs base
SafeGDScript (sandbox)                      3.5     10.21x      -0.3%
GDScript (engine)                          35.7      1.00x      +7.8%
```

In this specific case we gained 10x over GDScript by making our loop variable an integer.

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
