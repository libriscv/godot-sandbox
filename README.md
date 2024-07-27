# Sandboxing for Godot

This GDExtension implements safe, low-latency sandboxing for Godot. It is not a convenience layer for other languages, unlike language ports/bindings. This extension exists to allow Godot creators to implement safe modding support, such that they can pass around programs built by other players, knowing that these programs cannot harm other players.

```gdscript
	var f = FileAccess.open("res://riscv/test", FileAccess.READ);

	# Create a sandbox by loading a static RISC-V program
	var v = Sandbox.new();
	v.load(f.get_buffer(f.get_length()), []);

	# Make a function call into the sandbox
	v.vmcall("my_function", Vector4(1, 2, 3, 4));

	# Create a callable Variant, and then call it later
	var callable = v.vmcallable("function3");
	callable.call(123, 456.0, "Test");

	# Pass a function to the sandbox as a callable Variant, and let it call it
	var ff : Callable = v.vmcallable("final_function");
	v.vmcall("trampoline_function", ff);
```

The API towards the sandbox uses Variants, and the API inside the sandbox uses (translated) Variants.

```C++
extern "C"
void function3(std::span<Variant> args)
{
	UtilityFunctions::print("x = ", args[0], " y = ", args[1], " text = ", args[2]);
}
```

Above: An example of a sandboxed C++ function.

## What can I do?

- You can implement a modding API for your game, to be used inside the sandbox. This API can then be used by other players to extend your game, in a safe manner. That is, they can send their mod to other people, including you, and they (and you) can assume that it is safe to try out the mod. The mod is *not supposed* to be able to do any harm. That is the whole point of this extension.
- You can implement support for your favorite language inside the sandbox. The sandbox receives Variants from Godot or GDScript, and can respond back with Variants. This means the communication is fully dynamic, and supports normal Godot usage. 

Languages that are known to work inside _libriscv_:
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
11. Any other language that can produce RISC-V programs

## Performance

The sandbox is implemented using _libriscv_ which primarily focuses on being lower latency than everyone else. This means that calling small functions in the sandbox is extremely fast, unlike all other sandboxing solutions, and most if not all other emulators.

There are high-performance modes for _libriscv_ available for development and final builds. When developing on Linux, libtcc-jit is available (which is in theory portable to both Windows and MacOS). And for final builds one can produce a high-performance binary translation that can be embedded in the project (either Godot itself, or the GDExtension). This high-performance binary translation works on all platforms, such as Nintendo Switch, Mobile Phones, and other locked-down systems. It is especially made for such systems, but is inconvenient to produce.

Please see the [documentation for libriscv](https://github.com/fwsGonzo/libriscv) for more information.

As a final note, the default interpreter mode in _libriscv_ is no slouch. And will for most games, and in most scenarios be both the slimmest in terms of memory and the fastest in terms of iteration. Certain variant operations will call out to Godot in order to get native performance.

# Contributing

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

# How to compile cpp code locally

This project provides a dockerfile for compiling cpp code locally. For usage example, go to:

```
cd program
docker run -d ghcr.io/fwsgonzo/compiler -v .:/usr/src riscv64-linux-gnu build.sh
```