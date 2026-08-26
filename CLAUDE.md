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
- `static` is parsed and dropped: there is no class instance.
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

### Reaching the engine

`class_name`, `extends`, nested classes with engine bases, and
`preload()`/`load()` reach past the program. Two restriction gates:

- **Per-use** -- `Class.new()` and `load()` checked at run time by
  `is_allowed_class`/`is_allowed_resource` callbacks. Always emitted by the
  compiler.
- **Structural** -- `class_name`, `extends`, native-base nested class refused at
  compile time when `CompilerOptions::restricted` is set. `SafeGDScript` reads
  `Sandbox::is_class_access_restricted()`. Changing restrictions
  (`set_restrictions()`, `set_class_allowed_callback()`) triggers
  `safegdscript_class_restrictions_changed()`, recompiling any `.sgd` whose
  `compiled_restricted` disagrees.

`class_name`/`extends` never reach machine code. Carried as metadata through
`IRProgram` → `Compiler` → `get_script_class_name()`/`get_script_base_class()`
→ `SafeGDScript::_get_global_name()`/`_get_instance_base_type()`. Path-based
extends (`extends "res://base.gd"`) kept verbatim, does not answer
`_get_instance_base_type()`.

Nested `class X extends <EngineClass>`: Dictionary instance with `"@base"` entry
holding the engine object. Resolution: class chain → script → base. Unresolved
access becomes `VGET`/`VSET`/`VCALL` on `@base`. Handle reloaded from Dictionary
per access. Handle lifetime follows **Object handles** rules.

Tests: `tests/test_classes.cpp`; `test_sgd_a_class_can_extend_an_engine_class`
and neighbours in Godot.

### Object handles

Guest Object handle = ObjectID | `Sandbox::OBJECT_HANDLE_TAG` (bit 62). Tag
disambiguates from scoped-variant indices. Resolved via
`gdextension_interface_object_get_instance_from_id()` (total function, null for
invalid ids).

Mode decided by `is_object_access_unrestricted()` (empty allowed-objects list
AND no JIT callback). Cannot switch from unrestricted to restricted mid-call.

- **Unrestricted** -- `get_object_from_address()` resolves directly via object
  database. Handles persist across calls. `ObjectBindingCache` (direct-mapped by
  id) skips mutex. `references_max` not enforced.
- **Restricted** -- `state().scoped_objects` is the capability set, capped at
  `references_max`. `m_allowed_objects` (ObjectID hash set) extends it.

RefCounted lifetime:
- Intra-call: `state().scoped_refs` with dedup via `ref_dedup` table. Bounded by
  `MAX_UNRESTRICTED_REFS`.
- Cross-call: `ECALL_OBJ_RETAIN` on globals. `retain_global_object()` reads the
  guest slot; reassigning the slot releases the previous ref. Retain refused
  while restricted; release always permitted.

Object Variant store is a raw 24-byte move, never `VASSIGN` (`OBJECT` is not a
complex variant type). Compiler emits retain for globals with
`IRGlobalVar::holds_object` (set by class type hint or observed `OBJECT` store).

Tests: `tests/test_object_retain.cpp`;
`test_sgd_a_node_member_survives_the_call_that_set_it`,
`test_sgd_a_refcounted_member_is_kept_alive`,
`test_sgd_touching_more_objects_than_references_max` in Godot.

### Function signatures

ELF symbols carry names only, not arity. Missing arguments are null pointers →
guest fault. `IRProgram::signatures` (`function_signature.h`) publishes param
names, types, return type, and count alongside the ELF.

Path: `Compiler::get_function_signatures()` → `get_function_signatures()` in
`gdscript.elf` → `SafeGDScript::update_methods_info()` → `MethodInfo`.
`SafeGDScriptInstance::callp` re-checks arity.

- Serialized as one `PackedByteArray` (not Array of Dictionaries) to stay under
  `Sandbox::MAX_REFS`. Format in `function_signature.h`, codec in
  `function_signature.cpp`.
- Constant-foldable defaults travel with the signature; non-foldable defaults
  leave the parameter required from Godot. Internal calls unaffected: `gen_call`
  materialises defaults at the call site.
- Engine reads `default_arguments` as contiguous `Variant` array despite the
  `GDExtensionVariantPtr *` in `GDExtensionMethodInfo`.
- Tests: `tests/test_signatures.cpp`; `test_sgd_*` in
  `test_gdscript_compiler.gd`.

### What the editor reads

Signatures also carry `line` (1-based, from `func` token) and `description`
(`##` doc-comment block above declaration).

- `_get_member_line()` returns -1 for unknown names (not 0).
- `_get_documentation()` builds `DocData::ClassDoc` keyed by
  `_get_doc_class_name()`. Key mismatch silently drops the entry.
- Completion: `_get_script_method_list()` for `$Node.`/typed-var/`preload(...)`;
  `_get_method_info()` for argument hints. `get_node("Node").` resolves for no
  script type (only `$`/`%` literals resolve).
- These virtuals are not GDScript-callable; verify with temporary
  `ClassDB::bind_method`.

### Dispatch

`match` on integer constants has two lowerings. `tests/test_switch.cpp` covers
both; `tests/tests/test_cpu.sgd` runs a 16-opcode machine end-to-end.

Folded `const` materialised as immediate at use site (not `LOAD_GLOBAL`), giving
the match typed operands that avoid `VEVAL`. Container consts exempt (handle
identity).

Dense integer patterns → `SWITCH` jump table:

```
ld     t0, subject
li     t1, count
bgeu   t0, t1, past
auipc  t1, 0
sh2add t0, t0, t1           # Zba
jr     12(t0)
```

O(1) dispatch, no relocation. Density thresholds: `MIN_SWITCH_CASES`,
`MAX_SWITCH_SPREAD`, `MAX_SWITCH_ENTRIES` in `codegen.cpp`.

Table is a fast path: falls through on non-integer or out-of-range. Typed `int`
subject → `JUMP` to wildcard; otherwise full equality chain (e.g. `match 3.0`
must reach `3:` arm). Any variable pattern disqualifies the table.

### Match patterns

Five pattern kinds: value (equality), Array destructure, Dictionary destructure,
binding (`var x`), wildcard (`_`). Optional `when` guard per arm.

```gdscript
match value:
	1, 2:                       # value
	[1, var rest, ..]:          # Array shape
	{"kind": "circle", "r": var r}:  # Dictionary keys
	var v when v < 0:           # binding + guard
	_:                          # wildcard
```

- Guards run after pattern match; declining guard continues chain (does not exit
  match). Any guard disqualifies the jump table. Guarded `_` is not a catch-all.
- Bindings are copies, scoped to the arm.
- Container patterns: `TYPE_TEST` (no syscall) → length → elements. Known
  type mismatch → skip (one jump, no destructuring).
- Array syscalls: `ECALL_ARRAY_SIZE`, `ECALL_ARRAY_AT`. Dictionary:
  `ECALL_DICTIONARY_OPS` with `GET_SIZE`/`HAS`/`GET` (key in a2, result in a3).
- Without trailing `..`, size is part of the pattern.
- Container patterns excluded from differential/optimizer-invariance corpus.

Tests: `tests/test_match.cpp`; `test_match_bindings_and_guards`,
`test_match_container_patterns` in `test_gdscript_compiler.gd`.

### Debugging a .sgd program

Three components; only the line table is zero-cost.

**Line table** (`line_table.h`): address→line map, always produced. Ascending by
address. Host access: `get_line_table()` / `SafeGDScript::get_line_table()`.

**Shadow stack** (`debug_layout.h`, `riscv_debug.cpp`): push/pop per call,
requires `CompilerOptions::debug_info`. Frames record return address (outer
frame line = call site). Read by `debug_safegdscript.cpp`;
`Sandbox::handle_exception` prefers it over symbol+offset.

**Breakpoints**: compile-time only. `ECALL_BREAKPOINT` emitted at requested
lines; zero cost when unset. Changing set recompiles all instances. Non-empty
set implies `debug_info`.

- Saves/restores `a0`/`a7` around syscall; `emit_ecall()` return-value guard
  waived.
- Emitted below labels (not above — back edges would skip).
- Optimized-away code gets no stop. `get_installed_breakpoints()` returns the
  surviving subset.
- Break = non-returning syscall. No guest instructions burned. Continue =
  return from syscall. Host thread blocks.
- One program stopped at a time. `vmcall_internal` preempts via
  `preempt_internal`. Stopped script refuses rebuild.

**Editor integration** (GDExtension — no toggle notifications, no engine-stop
control):

- `_frame()` polls `EngineDebugger` every 6th frame (only while active).
  Delta-applied. `compile_source_to_elf()` reads breakpoints at first build.
- Runtime-built scripts: `take_over_path()` before setting source.
- `EngineDebugger::script_debug()` replaces the wait loop. Step Into/Over = next
  breakpoint. Locals/members empty.
- `breakpoint_hit` listeners take priority over editor stop.

API: `set_breakpoint()`, `set_breakpoints()`, `clear_breakpoints()`, signal
`breakpoint_hit(script, line)` on `SafeGDScript`.

Tests: `test_breakpoints.cpp`, `test_sgd_breakpoint_*` in
`test_gdscript_compiler.gd`. `tests/run_debugger_test.sh` drives editor path.

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
that a compiler bug fails a build rather than a user's program at run time.
What each one is for:

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
