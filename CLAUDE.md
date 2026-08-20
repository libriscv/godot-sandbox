This project is a sandbox addon for the Godot engine. It implements a Sandbox node which can load a RISC-V ELF binary, which can then execute code and access the host Godot instance. The access can be restricted by class names, methods, properties etc. Execution is memory safe and has an optional execution timeout using instruction counting. The underlying emulator is libriscv. The emulator runs either in JIT or interpreter mode. This can be toggled during init. Both JIT and interpreter modes are faster than GDScript. Whitespace is tabs.

The Sandbox node can be instantiated, given an ELF and then it can export functions and properties making it a sort-of script-like instance. It can also be used as a Script, by attaching it to the Script of any Node. In that case, calling functions on the node will be forwarded to the script, which again calls into the sandboxed guest program. The sandbox node is implemented in src/sandbox.cpp and src/sandbox.h as well as a few src/sandbox_*.cpp files. The cpp ScriptLanguage is in src/cpp/*. The sandbox API is implemented in src/sandbox_syscalls.cpp and sandbox_syscalls_*.cpp.

The API inside the Sandbox follows the public GDScript API closely, simply because it implements the complete Variant with all types, as well as the ability to call functions on objects.

The Sandbox API is written in C++. It accesses the host using system calls. Most system calls are dedicated to handling methods in the very common Variant types. Eg. the most important methods in PackedVector3Array will have dedicated system calls, while the rest are achieved through vcall (a call on a Variant which holds the PackedVector3Array). This also means that the Godot Sandbox API is complete, matching the GDScript capabilities.

ELFScript is a Godot-specific Resource type that is the result of loading and ELF into a Godot Project. It can be placed into a Node, which will make a Sandbox with that resource (ELF) loaded. It really has nothing to do with the ELF format. It's just a handler for resources that end with .elf (which _are_ ELF binaries), but any actual ELF parsing happens only in the libriscv emulator.

The host-side and guest-side share a common system call API for all languages supported. The system calls are defined in syscalls.h, and are fixed numbers that cannot be reassigned, as that would break existing users programs. Instead new functionality is added as new system calls, or as options to existing system calls when that is possible.

There is an ongoing GDScript-to-RISC-V compiler project under the src/gdscript/compiler folder. It's parsing GDScript into AST, then to IR and it will finally be transformed to 64-bit RISC-V and packed into an ELF container as the last step. At that point the goal is to make it executable inside Godot Sandbox. It has written like a CMake library, and it currently being used in the unit tests. One unit test compiles an ELF inside the sandbox and then runs the result in another sandbox. When running tests for the compiler, they should have a timeout as loops may run forever. Do NOT under any circumstance disable tests, FIX the problem. The unit tests can be executed with `ctest .` in the src/gdscript/compiler/build folder.

### Global functions

`@GlobalScope` functions (`print`, `abs`, `sin`, `clamp`, `lerp`, `str`, `len`,
...) are not methods on the owner node. A call to one must never fall through to
the self-call path: Godot accepts the resulting VCALL and drops it silently.

One row per global in `src/gdscript/compiler/globals.h`: name, arity, return
type, lowering (inline integer, inline float, `ECALL_UTILITY`, or a run-time
choice between inline and host when argument types are unknown).

- `int()`, `float()`, `bool()`, `String()` are rows in the same table. The first
  three lower inline when the argument is a known number or bool, host otherwise
  (`int("42")` == 42 needs Godot's parse).
- `randi`, `randf`, `randi_range`, `randf_range`, `randfn` are marked impure, so
  DCE keeps an unused draw. The IR interpreter refuses them, which excludes them
  from the differential and optimizer-invariance corpus.
- `randomize()` and `seed()` are absent: they mutate host RNG state. Same rule
  for anything else reaching engine state.

### Operators

Precedence in `parser.cpp`, loosest first: `as`, ternary, `or`, `and`, `not`,
`in`/`not in`, equality, comparison, `|`, `^`, `&`, shifts, `+`/`-`, `*`/`/`/`%`,
`is`/`is not`, `**`, unary `-`/`~`/`+`, call and subscript.

Two entries deviate from the GDScript manual; the engine is authoritative and
`test_gdscript_compiler.gd` checks both against it:

- `**` is left-associative and binds looser than a leading `-`: `2 ** 3 ** 2` is
  `(2**3)**2` == 64, `-2 ** 2` is `(-2)**2` == 4.
- `not` binds looser than comparison: `not a == b` is `not (a == b)`.

Lowering:

- `**` and `in` always go to the host via `ECALL_VEVAL` (`OP_POWER` = 13,
  `OP_IN` = 24); no native path. No inline expansion reproduces Godot's answer
  for every type pair, and only the host knows whether the right operand of `in`
  is an Array, Dictionary or String. The IR interpreter refuses both, so neither
  appears in the differential or optimizer-invariance corpus.
- `is` is a `lw`/`xori`/`seqz` on the Variant type tag, no syscall. Exact tag
  compare, not convertibility: `1.0 is int` is false. Folds to a constant when
  the type is already tracked. Class names are rejected (inheritance walk needs
  the engine).
- `as` is limited to `int`, `float`, `bool`, `String` and lowers to the
  constructor of the same name. Class names are rejected: a failed cast yields
  null, which the compiler cannot decide.

### Enums

`enum Mode { IDLE, RUN = 5, STOP }` is compiler-only, like a struct. Members are
compile-time integers; nothing of an enum reaches the IR. `Mode.STOP`, and a
bare `RIGHT` from an unnamed `enum { LEFT, RIGHT }`, become the immediate they
stand for, typed `int` so a compare against one stays off the VEVAL path. An
undeclared member is a compile error; a local of the same name shadows it.

### Iterating a container

- `for v in <array>`: walked by position via `ECALL_ARRAY_SIZE` and
  `ECALL_ARRAY_AT`.
- `for k in <dictionary>`: the loop setup replaces the Dictionary with
  `Dictionary_Op::GET_KEYS` once, before the loop. Guarded by one `TYPE_TEST`
  when the type is unknown, unguarded when known.
- `GET_KEYS`/`GET_VALUES` were already numbered in `syscalls.h` and implemented
  in `api_dict_ops`. Having no key argument, they take the result Variant
  pointer in a2, not a3.

### Statement layout

- A newline inside `(`, `[` or `{` is layout, not a statement end, so argument
  lists and literals may span lines.
- `\` is an explicit line continuation; `;` separates statements on one line.
- Trailing commas are allowed in literals and argument lists.
- `{key = value}` is the Lua-style spelling of `{"key": value}`.
- An unclosed bracket swallows the rest of the file, so the lexer reports it at
  the position it was opened, not at EOF.
- `static` and `class_name` are parsed and dropped: there is no class instance,
  and the registered script name is a project fact.
- Container element types (`Array[int]`, `Dictionary[String, int]`) are parsed
  and dropped: every value the compiler moves is a Variant, and Godot enforces
  typed containers at the boundary.

### Structs

`struct` is compiler-only sugar for a Dictionary with a fixed key set:

```gdscript
struct BankAccount:
	var balance = 0
	var loan: int = 0
```

- Constructed with `BankAccount.new(...)` or `BankAccount(...)`, positionally,
  by name (`BankAccount.new(loan = 50)`), or both; omitted fields take their
  declared default.
- The instance is an ordinary Dictionary Variant: Godot sees
  `{"balance": 0, "loan": 0}`, and a Dictionary from Godot works as a struct
  parameter.
- A field access is a Dictionary get/set, never `VGET`/`VSET`: the property
  syscalls target Object properties and throw on a Dictionary. This holds for
  plain dictionaries too, so `d.key` means `d["key"]` as in GDScript.
- An undeclared field name is a compile error wherever the struct is known:
  from `.new()`, a `: BankAccount` hint on a variable, parameter or global, or a
  `-> BankAccount` return type.
- Nothing of a struct survives into the IR. `tests/test_structs.cpp` covers the
  lowering.

### Function signatures

Godot calls an exported function through its symbol, and the Sandbox ABI hands
the guest one Variant pointer per argument and no count. An argument the caller
left out is therefore a null pointer, and the guest faults reading a Variant out
of it -- so the arity has to reach Godot, and the ELF cannot carry it: the
symbol table has names alone.

`IRProgram::signatures` (`function_signature.h`) is what the compiler publishes
beside the ELF: parameter names, declared types, return type, and the count a
caller must supply. `Compiler::get_function_signatures()` holds the last
compile's, `get_function_signatures()` in `gdscript.elf` hands them out, and
`SafeGDScript::update_methods_info()` turns them into the `MethodInfo` list
Godot checks calls against. `SafeGDScriptInstance::callp` re-checks, for the
paths Godot does not pre-check itself.

- The table crosses the boundary as one `PackedByteArray`, not an Array of
  Dictionaries. Everything a guest hands out that is not inlined in a Variant
  costs a scoped variant, and `Sandbox::MAX_REFS` is 100: containers per
  function run any real script into the cap. The format is documented in
  `function_signature.h` and encoded/decoded by the one file both sides compile,
  `function_signature.cpp`.
- Defaults are the same problem seen from the callee, which cannot fill one in
  either, having no way to tell whether it was given the argument. A default
  that folds to a constant travels with the signature and the host appends it;
  one that does not fold leaves the parameter required for a call from Godot,
  which is refused rather than guessed at. A call inside the program is
  unaffected: `gen_call` materialises the full expression at the call site.
- Despite the `GDExtensionVariantPtr *` in `GDExtensionMethodInfo`, the engine
  reads `default_arguments` back as one contiguous array of `Variant`.
- `tests/test_signatures.cpp` covers what the compiler publishes;
  `test_sgd_*` in `test_gdscript_compiler.gd` covers the arity Godot enforces.

### Dispatch

A fetch-decode-execute loop whose execute step is one `match` on an opcode is
the hot path for the logic CPUs people build with this, so `match` has two
lowerings. `tests/test_switch.cpp` covers both; `tests/tests/test_cpu.sgd` is a
sixteen-opcode machine the Godot tests run end to end.

A `const` whose initializer folds is materialised at its use as an immediate,
not loaded from the global data area. The point is the type, not the saved load:
`LOAD_GLOBAL` carries no type, so `match op: OP_ADD:` compared an untyped
Variant against an untyped Variant and every arm became a `VEVAL` syscall. A
container const is exempt — it is a handle, and every read must yield the same
container.

Dense integer constant patterns lower to `SWITCH`: a table of `jal`
instructions in the instruction stream, entered with

```
ld     t0, subject          # the integer out of its Variant
li     t1, count
bgeu   t0, t1, past         # one unsigned compare covers both ends
auipc  t1, 0                # t1 = here; the table starts here + 12
sh2add t0, t0, t1           # Zba, which libriscv decodes unconditionally
jr     12(t0)
```

Dispatch is constant time in the opcode count and needs no relocation.
`MIN_SWITCH_CASES`, `MAX_SWITCH_SPREAD` and `MAX_SWITCH_ENTRIES` in
`codegen.cpp` set the density threshold.

The table is a fast path, not a replacement. It falls through when the subject
is not an integer or is out of range. What follows it depends on what the
compiler knows: a `JUMP` to the wildcard when the subject is typed `int`,
otherwise the full chain of equality tests, since `match 3.0` must still reach
the `3:` arm and `match true` the `1:` arm. One pattern the compiler cannot
evaluate disqualifies the whole match: a variable pattern may cover a value the
table also covers, and GDScript takes the arm written first.

### Match patterns

An arm is a list of patterns, any one of which takes it, optionally followed by
`when <condition>`. Five kinds; only the first is an equality test:

```gdscript
match value:
	1, 2:                       # value: subject == pattern
		...
	[1, var rest, ..]:          # Array of that shape, elementwise
		...
	{"kind": "circle", "r": var r}:  # Dictionary with those keys
		...
	var v when v < 0:           # binds what it matched; the guard may decline
		...
	_:                          # wildcard, binds nothing
		...
```

Arms form a chain: each tests its patterns, then its guard, then falls into its
body or on to the next arm. Three properties the tests pin down:

- A guard runs after the pattern matched and its bindings exist, and a declining
  guard continues the chain instead of leaving the match, so two arms may bind
  the same name and be told apart by their guards. Any guard disqualifies the
  jump table (an entry jumping straight to a body would run a declined arm), and
  a guarded `_` is not a catch-all.
- A binding is a copy: assigning to the name in the body must not reach the
  subject. It is declared in the arm's scope, which covers both test and body,
  so arms may reuse the name.
- A container pattern tests the type tag first (`TYPE_TEST`, no syscall), then
  the length, then the elements: a short Array is never indexed past its end,
  and an integer subject is never asked for a length. When the subject's type is
  known not to be the container's, the whole arm is one jump and no
  destructuring is emitted.

Syscalls: Arrays use `ECALL_ARRAY_SIZE` and `ECALL_ARRAY_AT`; Dictionaries use
`ECALL_DICTIONARY_OPS` with `GET_SIZE`, `HAS` and `GET`, where the keyed
operations pass the key in a2 and the result in a3. Without a trailing `..` the
size is part of the pattern — `{"kind": var k}` does not match a Dictionary that
also has `"r"` — which is why the size is queried at all. The IR interpreter has
no containers, so container patterns are excluded from the differential and
optimizer-invariance corpus, as `**` and `in` are.

`tests/test_match.cpp` covers the lowering. `test_match_bindings_and_guards` and
`test_match_container_patterns` in `tests/tests/test_gdscript_compiler.gd` run
each pattern in a real sandbox and against the engine's own `match` on the same
values.

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

These checks run from `src/gdscript/compiler/build` with `ctest .`, and exist so
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
- `test_operators` — operator lowering and precedence, including the two places
  the engine differs from the manual
- `test_switch` — const folding, the jump table density thresholds, what the
  table must not decide alone, and the emitted dispatch sequence
- `test_match` — the non-value pattern kinds: bindings, guards, array and
  dictionary patterns, and the syscalls each does and does not make

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
