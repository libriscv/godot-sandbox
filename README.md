<p align="center">
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
        <a href="https://libriscv.no">Website</a> | <a href="https://github.com/libriscv/godot-sandbox/blob/main/CHANGELOG.md">Changelog</a> | <a href="https://discord.gg/n4GcXr66X5">Discord</a> | <a href="https://gonzerelli.itch.io/demo">Web Demo</a>
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

## Usage

- Create a new `Sandbox` and [assign an ELF script resource to it](https://libriscv.no/docs/godot/sandbox/#create-a-sandbox)
	- Access to the underlying Sandbox
	- Control of lifetime
	- Auto-completion from other GDScripts
- [Directly assign an ELF script resource to a node](https://libriscv.no/docs/godot/sandbox/#using-programs-directly-as-scripts)
	- Shared sandbox among all instances with that script
	- Maximum scalability
	- Call functions and attach signals like GDScript

```C++
static void add_coin(const Node& player) {
	static int coins = 0;
	coins ++;
	auto coinlabel = player.get_parent().get_node("Texts/CoinLabel");
	coinlabel.set("text", "You have collected "
		+ std::to_string(coins) + ((coins == 1) ? " coin" : " coins"));
}

extern "C" Variant _on_body_entered(Variant arg) {
	Node player_node = arg.as_node();
	if (player_node.get_name() != "Player")
		return {};

	Node(".").queue_free(); // Remove the current coin!
	add_coin(player_node);
	return {};
}
```

Script of a simple Coin pickup, with a counter that updates a label in the tree of the player. This script can be attached to the Coin just like GDScript.

You may also have a look at our [demo repository](https://github.com/libriscv/godot-sandbox-demo) for the Godot Sandbox. It's a tiny platformer that uses C++ and Rust.

### What can I do?

- You can implement a modding API for your game, to be used inside the sandbox. This API can then be used by other players to extend your game, in a safe manner. That is, they can send their mod to other people, including you, and they (and you) can assume that it is safe to try out the mod. The mod is *not supposed* to be able to do any harm. That is the whole point of this extension.
- You can implement support for your favorite language inside the sandbox. The sandbox receives Variants from Godot or GDScript, and can respond back with Variants. This means the communication is fully dynamic, and supports normal Godot usage.
- You can distribute programs from a server to clients on login. The programs will behave the same way on all platforms, including 32-bit platforms. You can use this to live-distribute changes, like bugfixes, to the game without having to redeploy the game itself.

Languages that are known to work inside the sandbox:
1. QuickJS
2. C
3. C++
4. Rust
5. Zig
6. Nim
7. Nelua
8. v8 JavaScript w/JIT
9. Go (not recommended)
10. Lua, Luau
11. Any language that transpiles to C or emits RISC-V programs

More languages will be supported out-of-the-box over time. *Currently C++ and Rust are supported.*

## Tips

- Not all Variant types are implemented inside the Sandbox.
- Complex variants may only be passed out of the sandbox if they have been seen (bit-exact) on the way in. This means, the only way to pass a Dictionary or Callable out of the sandbox, is to first pass it in. Passing a complex Variant into the sandbox is implicitly seen as allow-listing that specific Variant. Once the VM function call ends, all temporary allowances are cleared (for security reasons) and forgotten.
- More Variant types will be supported inside the sandbox over time. For security reasons, each type must receive proper scrutiny and will receive at most a conservative implementation.
- The performance of complex Variant calls and many common operations in the sandbox will have native performance (on a case-by-case basis). Do not be afraid of copying data or using temporary strings, as memory-, heap- and string- operations have native performance on all supported languages.


## Performance

The sandbox is implemented using _libriscv_ which primarily focuses on being low-latency. This means that calling small functions in the sandbox is extremely fast, unlike other sandboxing solutions. _libriscv_ is a specialty emulator designed for low-latency sandboxing.

There are high-performance modes for _libriscv_ available for both development and final builds. When developing on Linux, libtcc-jit is available (which is in theory portable to both Windows and MacOS). And for final builds one can produce a high-performance binary translation, with up to 92% native performance, that can be embedded in the project (either Godot itself, or the GDExtension). This high-performance binary translation works on all platforms, such as Nintendo Switch, Mobile Phones, and other locked-down systems. It is especially made for such systems, but is inconvenient to produce.

Please see the [documentation for the emulator](https://github.com/libriscv/libriscv) for more information.

As a final note, the default interpreter mode in _libriscv_ is no slouch, being among the fastest interpreters. And will for most games, and in most scenarios be both the slimmest in terms of memory and the fastest in terms of iteration. Certain common operations will call out to Godot in order to get native performance.


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
