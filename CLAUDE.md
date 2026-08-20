This project is a sandbox addon for the Godot engine. It implements a Sandbox node which can load a RISC-V ELF binary, which can then execute code and access the host Godot instance. The access can be restricted by class names, methods, properties etc. Execution is memory safe and has an optional execution timeout using instruction counting. The underlying emulator is libriscv. The emulator runs either in JIT or interpreter mode. This can be toggled during init. Both JIT and interpreter modes are faster than GDScript. Whitespace is tabs.

The Sandbox node can be instantiated, given an ELF and then it can export functions and properties making it a sort-of script-like instance. It can also be used as a Script, by attaching it to the Script of any Node. In that case, calling functions on the node will be forwarded to the script, which again calls into the sandboxed guest program. The sandbox node is implemented in src/sandbox.cpp and src/sandbox.h as well as a few src/sandbox_*.cpp files. The cpp ScriptLanguage is in src/cpp/*. The sandbox API is implemented in src/sandbox_syscalls.cpp and sandbox_syscalls_*.cpp.

The API inside the Sandbox follows the public GDScript API closely, simply because it implements the complete Variant with all types, as well as the ability to call functions on objects.

The Sandbox API is written in C++. It accesses the host using system calls. Most system calls are dedicated to handling methods in the very common Variant types. Eg. the most important methods in PackedVector3Array will have dedicated system calls, while the rest are achieved through vcall (a call on a Variant which holds the PackedVector3Array). This also means that the Godot Sandbox API is complete, matching the GDScript capabilities.

ELFScript is a Godot-specific Resource type that is the result of loading and ELF into a Godot Project. It can be placed into a Node, which will make a Sandbox with that resource (ELF) loaded. It really has nothing to do with the ELF format. It's just a handler for resources that end with .elf (which _are_ ELF binaries), but any actual ELF parsing happens only in the libriscv emulator.

The host-side and guest-side share a common system call API for all languages supported. The system calls are defined in syscalls.h, and are fixed numbers that cannot be reassigned, as that would break existing users programs. Instead new functionality is added as new system calls, or as options to existing system calls when that is possible.

There is an ongoing GDScript-to-RISC-V compiler project under the src/gdscript/compiler folder. It's parsing GDScript into AST, then to IR and it will finally be transformed to 64-bit RISC-V and packed into an ELF container as the last step. At that point the goal is to make it executable inside Godot Sandbox. It has written like a CMake library, and it currently being used in the unit tests. One unit test compiles an ELF inside the sandbox and then runs the result in another sandbox. When running tests for the compiler, they should have a timeout as loops may run forever. Do NOT under any circumstance disable tests, FIX the problem. The unit tests can be executed with `ctest .` in the src/gdscript/compiler/build folder.

### Global functions

GDScript's `@GlobalScope` functions -- `print`, `abs`, `sin`, `clamp`, `lerp`,
`str`, `len` and the rest -- are not methods on the owner node, so a call to one
must never fall through to the self-call path: Godot accepts the resulting VCALL
and drops it in silence. Every global the compiler knows is one row in
`src/gdscript/compiler/globals.h`, saying what it is called, how many arguments
it takes, what it returns, and which lowering performs it: inline integer or
floating-point code, a call to the host through `ECALL_UTILITY`, or a run-time
choice between the two when the argument types are not known.

The type constructors `int()`, `float()`, `bool()` and `String()` are rows in
that table too. The first three lower inline when the compiler already knows
the argument is a number or a bool, and go to the host otherwise, because
`int("42")` is 42 and only Godot's parse says so.

`randi()`, `randf()`, `randi_range()`, `randf_range()` and `randfn()` are the
one family there whose answer depends on host state. Their rows are marked
impure, which is what stops a pass from deleting a draw nobody reads, and the
IR interpreter refuses to evaluate one rather than inventing a number the
machine would not have produced -- so they cannot appear in the differential or
optimizer-invariance corpus. `randomize()` and `seed()` are deliberately absent:
they set the seed of the generator the rest of the project draws from, which is
the host's state and not the guest's. Anything else that reaches engine state
belongs outside this table for the same reason.

### Structs

`struct` is compiler-only sugar for a Dictionary with a fixed set of keys:

```gdscript
struct BankAccount:
	var balance = 0
	var loan: int = 0
```

An instance is built with `BankAccount.new(...)` or `BankAccount(...)`, taking
values positionally, by name (`BankAccount.new(loan = 50)`), or both, with any
field left out keeping its declared default. It is an ordinary Dictionary
Variant: Godot sees `{"balance": 0, "loan": 0}` coming back out, and a Dictionary
passed in from Godot works as a struct parameter.

A field access is a Dictionary get or set, never `VGET`/`VSET` -- the property
syscalls reach an Object's properties and throw on a Dictionary. That applies to
plain dictionaries too, so `d.key` now means `d["key"]` as it does in GDScript.

What the declaration buys over a bare Dictionary is that a field name the struct
does not declare is a compile error, wherever the compiler knows which struct a
value is: from `.new()`, from a `: BankAccount` hint on a variable, parameter or
global, or from a `-> BankAccount` return type. Nothing about a struct survives
into the IR. `tests/test_structs.cpp` covers the lowering.

## Compiler Debugging Tools

Two debugging tools are available in the compiler build folder:

### dump_ir
Inspects the IR (Intermediate Representation) generated from GDScript.

**Usage:**
```bash
cat script.gd | ./dump_ir                    # Basic IR dump
cat script.gd | ./dump_ir --no-optimize      # IR without optimizations
cat script.gd | ./dump_ir --codegen          # IR with register allocation info
cat script.gd | ./dump_ir -v --codegen       # Verbose with detailed operands
```

**What it shows:**
- Virtual registers (r0, r1, ...)
- Physical register allocation when using `--codegen` (t0, t1, s0, a0, etc.)
- Stack slot assignments for spilled values
- Type hints and instruction operands

### gdscript_to_riscv
Compiles GDScript to RISC-V ELF and immediately shows the disassembled machine code.

**Usage:**
```bash
cat script.gd | ./gdscript_to_riscv              # Disassemble all functions
cat script.gd | ./gdscript_to_riscv -f test      # Disassemble specific function
```

**What it shows:**
- Actual RISC-V instructions generated
- Machine code bytes
- Memory addresses
- Equivalent assembly with register names

These tools are essential for tracking down bugs in the compiler pipeline by showing what's generated at each stage.

### Checking the compiler against itself

Four checks run from `src/gdscript/compiler/build` with `ctest .`, and exist so
that a compiler bug fails a build rather than a user's program at run time. See
`src/gdscript/compiler/REFACTOR.md` for why each one is there.

- `test_ir_verifier` — the IR verifier (`ir_verifier.h`), which also runs
  between every optimizer pass in debug builds. It checks operand roles against
  the metadata table, that every register read is defined on every path, that
  labels resolve, and that type hints match what the operands hold.
- `test_opt_invariance` — an optimizer pass must not change what a program
  computes. Every corpus program is interpreted unoptimized and after each
  prefix of the pipeline, and the answers have to match. On a mismatch it
  bisects and names the pass.
- `test_differential` — the same programs through the IR interpreter and
  through a real libriscv machine over the produced ELF. `--file program.gd`
  runs one program, which is how a failure gets reduced.
- `test_fuzz` — a seeded generator (`tests/gdscript_generator.h`) feeding the
  first two, with a shrinker. `tests/fuzz_nightly.sh` runs it and the
  differential harness for longer, from fresh seeds.
- `test_globals` — compile-time global functions

`GDSC_PASSES=<comma-separated pass names>` selects which optimizer passes run,
in every tool including `dump_ir`. `GDSC_PASSES=none` disables the optimizer;
the pass names are in `IROptimizer::pipeline()`.

Adding an opcode to `ir_opcodes.def` is deliberately a compile error at every
site that dispatches over `IROpcode`: those switches have no `default:` and the
library is built with `-Werror=switch`.

The GDScript-to-RISC-V unit tests are under /tests/tests. You can only visually inspect RISC-V ELFs using riscv64-linux-gnu-objdump. Executing the unit tests specific to GDScript is from the tests folder:
./run_unittests.sh -gselect compiler
Always run unit tests from the tests folder. There is a separate script for running Zig C++ unit tests:
./zig_unittests.sh -gselect compiler
Also must be run from the tests folder. The GDScript compiler is RUNNING INSIDE a sandbox instance in all unit tests. That means when there's a failure and the originating call is `compile_to_elf`, it means the compiler crashed/failed INSIDE the sandbox instance.

The Godot Sandbox ABI has a complex passing scheme in order to be fast and low-latency. However, it has a second mode where all arguments in and out are always Variants. Using Variant only is the best choice for the GDScript compiler, as it makes it fully dynamic just like real GDScript. Most people don't use type hints anyway, and we'd have to verify them regardless. Real GDScript has an optimization phase where bytecodes are replaced with faster specific bytecodes once types have been checked. We are not going to focus on that, but rather on getting everything up and working.

Register A0 is always the Variant return value pointer. A1-A7 are function arguments as pointers.
The Variant ABI is a 4-byte type, 4-byte padding and then 16-bytes of inlined data (for certain types), making it 24 bytes with regular settings. Variants are placed on the stack and the pointer is placed in registers for calls. In order to make the sandbox not unbox Variants into registers `vmcallv()` is used instead of `vmcall()`. Variants don't need be zeroed. Only the relevant portion has to have non-garbage value. For example Integer type needs only m_type and v.i to be valid, covering only 12/24 bytes.

The GDScript compiler has a register allocator that can be used to avoid clobbered registers especially when performing system calls. Any new feature needs an internal (CMake) test before an integration test in the unit tests is written.

The RISC-V codegen has to use Variants as that is the fundamental unit of GDScript. The structure of the Variant can be seen in program/cpp/docker/api/variant.hpp, including the enum types. This means we can actually add together two Vector2's, two Vector4i's, concatenate strings etc. without really knowing what they are. Any optimization needs to carefully consider whether or not we know the actual Variant types being dealt with before reconstructing the logic. With proper type hints we can perform arithmetic on two Variants without VEVAL. Sandboxed Variants aren't real Variants - they are closely guarded, so "using them wrong" is not going to hurt the host (unless there is a bug). So, for example adding two type-hinted integers or floats can be performed in physical registers.

When dealing with floats it's CRUCIAL to understand that:
1. v.f (the regular float) in the Variant structure is _always_ 64-bit.
2. real_t is CONFIGURABLE, but 32-bit float by default. Vectors use real_t.
3. Adding integer and float (whether constant or not) produces a float result

Godot's double-precision builds set real_t = double, which widens every real_t
payload stored inline in a Variant (Vector2/3/4, Rect2, Plane, Quaternion, Color)
and grows the Variant itself from 24 to 40 bytes. Integer vectors (Vector2i/3i/4i,
Rect2i) and Variant::FLOAT are unaffected. In the compiler this is described by
`src/gdscript/compiler/variant_layout.h`: every layout-dependent size and offset in
the RISC-V backend goes through `VariantLayout`, and the real_t-width loads, stores
and FP arithmetic go through the `emit_flr` / `emit_fsr` / `emit_fadd_r` family so
that both builds share one code path. The layout is selected at run time by
`CompilerOptions::double_precision`, which defaults to whatever the compiler was
built for (`-DDOUBLE_PRECISION=ON` defines `DOUBLE_PRECISION_REAL_T`). The internal
CMake test `test_double_precision` exercises both layouts from a single build.
Do NOT run the Godot unit tests with a double-precision guest against a
single-precision Godot binary - the layouts have to match end to end.

When dealing with object references, they are 32-bit integers which are stored in the data section of the Variant. When the sandbox stores this value, it's stored as a 64-bit value, so it won't matter if loaded as 32-bit or 64-bit int, however it does matter when storing the value: Use 64-bit sd instruction.
