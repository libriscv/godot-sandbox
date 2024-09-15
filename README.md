<p align="center">
<!-- ALL-CONTRIBUTORS-BADGE:START - Do not remove or modify this section -->
[![All Contributors](https://img.shields.io/badge/all_contributors-3-orange.svg?style=flat-square)](#contributors-)
<!-- ALL-CONTRIBUTORS-BADGE:END -->
<img src="https://github.com/libriscv/godot-sandbox/blob/main/banner.png?raw=true" width="312px"/>
</p>
<p align="center">

<p align="center">
        <img src="https://github.com/libriscv/godot-sandbox/actions/workflows/runner.yml/badge.svg?branch=main"
            alt="Godot Sandbox Build"></a>
        <img src="https://img.shields.io/badge/Godot-4.2-%23478cbf?logo=godot-engine&logoColor=white" />
</p>

<p align = "center">
    <strong>
        <a href="https://libriscv.no">Website</a> | <a href="https://libriscv.no/docs/godot/cppexamples">Code Examples</a> | <a href="https://discord.gg/n4GcXr66X5">Discord</a> | <a href="https://gonzerelli.itch.io/demo">Web Demo</a>
    </strong>
</p>


-----

<p align = "center">
<b>Safe, low-latency and fast sandbox</b>
<i>for the Godot game engine.</i>
</p>

-----

This extension exists to allow Godot creators to implement safe modding support, such that they can pass around programs built by other players, knowing that these programs cannot harm other players. All Godot platforms are supported.


## Installation

- Automatic (Recommended): Download the plugin from the official [Godot Asset Store](.) using the **AssetLib** tab in Godot by searching for **Godot Sandbox**.

- Manual: Download the [latest github release](https://github.com/libriscv/godot-sandbox/releases/latest) and move only the **addons** folder into your project **addons** folder.

### What can I do?

- You can implement a modding API for your game, to be used inside the sandbox. This API can then be used by other players to extend your game, in a safe manner. That is, they can send their mod to other people, including you, and they (and you) can assume that it is safe to try out the mod. The mod is *not supposed* to be able to do any harm. That is the whole point of this extension.
- You can implement support for your favorite language inside the sandbox. The sandbox receives Variants from Godot or GDScript, and can respond back with Variants. This means the communication is fully dynamic, and supports normal Godot usage.
- You can distribute programs from a server to clients on login. The programs will behave the same way on all platforms, including 32-bit platforms. You can use this to live-distribute changes, like bugfixes, to the game without having to redeploy the game itself.

## Usage

- Write C++ or Rust in the Godot editor. An accompanying ELF resource is created.

- Create a new `Sandbox` and [assign the ELF resource to it](https://libriscv.no/docs/godot/sandbox/#create-a-sandbox)
	- Access to the underlying Sandbox
	- Lifetime as any other node
	- Auto-completion from other GDScripts using @export

- Or, [directly assign an ELF script resource to a node](https://libriscv.no/docs/godot/sandbox/#using-programs-directly-as-scripts)
	- Shared sandbox among all instances with that script
	- Maximum scalability
	- Call functions and attach signals like GDScript

## Examples

```C++
static void add_coin(const Node& player) {
	static int coins = 0;
	coins ++;
	Node coinlabel = player.get_parent().get_node("Texts/CoinLabel");
	coinlabel.set("text", "You have collected "
		+ std::to_string(coins) + ((coins == 1) ? " coin" : " coins"));
}

extern "C" Variant _on_body_entered(Node2D node) {
	if (node.get_name() != "Player")
		return Nil;

	get_node().queue_free(); // Remove the current coin!
	add_coin(node);
	return Nil;
}
```

Script of a simple Coin pickup, with a counter that updates a label in the tree of the player. This script can be attached to the Coin just like GDScript.

```Rust
#[no_mangle]
pub fn _physics_process(delta: f64) -> Variant {
	if Engine::is_editor_hint() {
		return Variant::new_nil();
	}

	let slime = get_node();
	let sprite = get_node_from_path("AnimatedSprite2D");

	let flip_h = sprite.call("is_flipped_h", &[]);
	let direction: f32 = (flip_h.to_bool() as i32 as f32 - 0.5) * -2.0;

	let mut pos = slime.call("get_position", &[]).to_vec2();
	pos.x += direction * SPEED * delta as f32;

	slime.call("set_position", &[Variant::new_vec2(pos)]);

	let ray_right = get_node_from_path("raycast_right");
	let ray_left  = get_node_from_path("raycast_left");
	if direction > 0.0 && ray_right.call("is_colliding", &[]).to_bool() {
		sprite.call("set_flip_h", &[Variant::new_bool(true)]);
	}
	else if direction < 0.0 && ray_left.call("is_colliding", &[]).to_bool() {
		sprite.call("set_flip_h", &[Variant::new_bool(false)]);
	}

	Variant::new_nil()
}
```
The Rust API is still under development, but simple scripts are possible.

You may also have a look at our [demo repository](https://github.com/libriscv/godot-sandbox-demo) for the Godot Sandbox. It's a tiny platformer that uses C++ and Rust.

## Contributing

Requirements:
- [SCons](https://www.scons.org)
- [python3](https://www.python.org)

If you want to contribute to this repo, here are steps on how to build locally:

```sh
scons
```

Linting:

```sh
./scripts/clang-tidy.sh
```

## Contributors ✨

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