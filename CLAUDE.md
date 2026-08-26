Godot Sandbox: RISC-V ELF execution inside the Godot engine via libriscv.
Sandbox node loads an ELF, exports functions/properties, optionally attaches as
a Script (forwarding calls into the guest). JIT or interpreter; both outperform
GDScript. Whitespace is tabs.

Source layout: `src/sandbox.cpp`/`.h`, `src/sandbox_*.cpp` (node, syscalls, restrictions).
`src/cpp/*` (ScriptLanguage). `src/safegdscript/*` (SafeGDScript wrapper).

## Guest API

Mirrors the public GDScript API. Full Variant support; common container methods
have dedicated syscalls, remainder via vcall. Syscall numbers in `syscalls.h`
are ABI-stable — append only.

## ELFScript

Resource handler for `.elf` files. No ELF parsing — that is libriscv's job.
Placing one in a Node creates a Sandbox with that ELF loaded.

## Variant ABI

Layout: 4B type, 4B pad, 16B inline data = 24B (single-precision real_t).
Double-precision (`real_t = double`) widens vector payloads → 40B Variant.
Layout described by `variant_layout.h`; backend emits through `emit_flr`/`emit_fsr`/
`emit_fadd_r` so both builds share one code path. `CompilerOptions::double_precision`
selects at run time.

Register convention: A0 = return Variant*, A1–A7 = argument Variant*.
`vmcallv()` keeps Variants boxed. Only the live fields need valid data
(e.g. INTEGER: m_type + v.i = 12/24B). Object refs stored as 64-bit (`sd`);
read width is irrelevant.

`Variant::FLOAT` (v.f) is always `double`. `real_t` defaults to `float` and
governs vector components. `int + float` → `float`.

## Object handles

Guest handle = ObjectID | `OBJECT_HANDLE_TAG` (bit 62). Resolved via
`gdextension_interface_object_get_instance_from_id()`.

Unrestricted mode (empty allowed-objects, no JIT callback): direct resolve,
`ObjectBindingCache` (direct-mapped by id, no mutex), handles persist across
calls, `references_max` unenforced.

Restricted mode: `scoped_objects` = capability set, bounded by `references_max`,
extended by `m_allowed_objects` (ObjectID hash set).

RefCounted lifetime: intra-call via `scoped_refs` + `ref_dedup`, bounded by
`MAX_UNRESTRICTED_REFS`; cross-call via `ECALL_OBJ_RETAIN` on globals. Retain
refused while restricted; release always permitted.

Object Variant store is raw 24B move, never `VASSIGN`. Compiler emits retain for
globals with `IRGlobalVar::holds_object`.

## Script members

`SafeGDScriptInstance` runs `_init()` on creation, after
`create_instance_record()` lays down member defaults. `_init()` with arguments
belongs to `Class.new(args)` and is refused on a node.

Member lifetime follows Variant type. Typed complex members → `ECALL_VASSIGN`;
compiler-typed OBJECT → `ECALL_OBJ_RETAIN`. Untyped members (or class-name
declared, which is not a Variant type) can hold either across calls, so storage
moves to the host: `ECALL_VSTORE_GLOBAL` reads both tags, releases the old
value, copies scoped value into its own permanent slot (distinct index — shared
slots would dangle on release), retains objects, copies inline payload verbatim.

`var x = null` declares no type — NIL is the held value, not the slot type —
so the slot stays untyped and the first real assignment is not a
reclassification.

## Native backends

Two of them, and by default only one is built:

- **asmjit** (`RISCV_ASMJIT`) — in-process JIT to x86-64/AArch64. Default ON
  wherever asmjit has a ujit code generator; `ext/CMakeLists.txt` then defaults
  `RISCV_BINARY_TRANSLATION` (and libtcc) OFF, which no-ops
  `emit_binary_translation()`, `try_compile_binary_translation()` and
  `load_binary_translation()`. Gate on `has_feature_binary_translation()`.
- **C99 binary translator** (`RISCV_BINARY_TRANSLATION`) — emits C, compiles it
  out of process. What the SCons addon ships, and the only path for iOS/Web/
  Switch. Still the default where asmjit has no code generator (x86-32, 32-bit
  ARM).

`has_feature_jit()` covers either JIT. `is_jit()` is already true for a segment
that is only on its way to being compiled, so poll `is_binary_translated()` to
learn the code landed.

Windows cross-build: `mingw_toolchain.cmake`. `CMAKE_SYSTEM_NAME` is load-bearing
— without it CMake still describes a Linux host, `CMAKE_SIZEOF_VOID_P` comes back
empty and godot-cpp's bit-width `math(EXPR)` fails the configure, while asmjit
selects its mmap/shm_open backend for a PE target.

## Guest memory checks

Decided at load by `unchecked_memory_wanted()` (`sandbox.h`), reported by
`get_unchecked_memory()`. Two independent knobs:

- **Unrestricted** → unchecked: JIT emits no bounds check. Nothing the guest can
  reach is guarded anyway, and the check costs ~50% on a tight loop.
- **Restricted** → always bounds-checked.
- `binary_translation_nbit_as` (**on by default**) AND-masks every access into
  the arena. Unrestricted it is the *guard*: a stray address wraps instead of
  reaching the host, so a runaway stack faults the guest rather than killing
  Godot. Restricted it is a *speedup*: the mask replaces the check (loses the
  rodata guard, which cannot crash the host).

Requires a power-of-two arena — hence `MAX_VMEM = 32`. A non-Po2 `memory_max`
silently disqualifies the mask.

int loop vs GDScript, JIT: unchecked+unmasked 21x, masked 16x either mode,
checked 9.8x.

Tests: `test_an_unrestricted_sandbox_runs_unchecked`,
`test_the_masked_arena_is_on_by_default` in `test_restrictions.gd`.

## GDScript-to-RISC-V compiler

`src/gdscript/compiler/`. Parses GDScript → AST → IR → RV64 ELF. Target:
executable inside Godot Sandbox.

### @GlobalScope functions

One row per global in `globals.h`: name, arity, return type, lowering (inline
int/float, `ECALL_UTILITY`, or run-time choice). `int()`/`float()`/`bool()`/
`String()` are rows in the same table — inline when argument type is known
numeric/bool, host otherwise. `randi`/`randf`/`randi_range`/`randf_range`/`randfn`
marked impure (DCE preserves side effect); IR interpreter refuses them →
excluded from differential/optimizer-invariance corpus. `randomize()`/`seed()`
absent (mutate host RNG state).

### Operators

Precedence in `parser.cpp` (loosest → tightest): `as`, ternary, `or`, `and`,
`not`, `in`/`not in`, equality, comparison, `|`, `^`, `&`, shifts, `+`/`-`,
`*`/`/`/`%`, `is`/`is not`, `**`, unary `-`/`~`/`+`, call/subscript.

Engine-authoritative deviations from the GDScript manual (tested in
`test_gdscript_compiler.gd`):
- `**` left-associative, binds below unary: `2**3**2` = 64, `-2**2` = 4.
- `not` binds below comparison: `not a == b` ≡ `not (a == b)`.

Lowering:
- `**`/`in` → `ECALL_VEVAL` (OP_POWER=13, OP_IN=24); no native path. IR
  interpreter refuses both → excluded from corpus.
- `is <builtin_type>` → `lw`/`xori`/`seqz` on type tag (exact compare, not
  convertibility). Folds when type tracked.
- `is <class_name>` → `gen_class_test`: `Object.is_class()` engine chain, then
  `get_script()`/`get_global_name()`/`get_base_script()` script chain.
  Non-object → false. On tracked class instances: own name/ancestors fold to
  true, sibling classes fold to false, engine names run the walk on `@base`.
- `as <type>` → constructor for builtins; `is` + value-or-null for classes,
  preserving instance declaration.

### Built-in constructors

`INLINE_CONSTRUCTORS` (codegen.cpp): types whose payload fits inline Variant
bytes — `MAKE_VECTOR2` etc., no syscall. `HOST_CONSTRUCTORS`: Transform2D/3D,
Basis, Quaternion, AABB, Projection, StringName, NodePath, RID, Signal —
delegates to `gdextension_interface_variant_construct` via `ECALL_VCONSTRUCT`.
Resolves overloads and conversions, max 8 arguments.

Inline types with no component-wise form (`Vector2(Vector2i)`, `Color(String)`)
fall through to `ECALL_VCONSTRUCT`. `Vector2(1)` is a compile error (no
single-scalar ctor). Array assigned to declared `Packed*Array` converts through
that type's constructor.

### Enums

Compiler-only. Members fold to integer immediates, typed `int`. Nothing reaches
IR. Undeclared member = compile error; local shadows.

Exception: engine-constant initializers (`PhysicsServer2D.SHAPE_CAPSULE`) have
no compile-time value. `Parser::holds_engine_constant` keeps the initializer in
`EnumDecl::owned_values`; `Member::value_expr` points at it, `value` becomes
auto-increment offset. `gen_enum_member` re-evaluates per use. Folding refused;
bad literal (`A = "hi"`) still a compile error.

### Container iteration

- `for v in <array>`: `ECALL_ARRAY_SIZE` + `ECALL_ARRAY_AT`.
- `for k in <dict>`: setup replaces Dictionary with `Dictionary_Op::GET_KEYS`
  once. `TYPE_TEST` guard when type unknown.
- `GET_KEYS`/`GET_VALUES`: result Variant* in a2 (no key argument).
- `for c in <string>` (known String): `gen_string_walk`.
  `ECALL_STRING_BATCH` returns up to 16 chars in consecutive scoped slots as
  `(first_index << 32) | count`; handing out a character is `MAKE_SCOPED` on an
  index (tag store, no syscall). String is a value type — walk copies the
  Variant first. Two scopes: outer for batch (released on refill), inner marked
  after each refill and released every pass. Host clamps count against
  `references_max`. Untyped iterables use four-way `TYPE_TEST` dispatch with
  per-character `ECALL_STRING_AT`.

### Statement layout

Newline inside `(`/`[`/`{` is continuation, not statement end. `\` = explicit
continuation; `;` = statement separator. Trailing commas allowed. `{key = value}`
= Lua-style `{"key": value}`. Unclosed bracket swallows file; reported at open
position. `static` parsed and dropped (no class instance). Typed containers
(`Array[int]`) parsed and dropped (Variant-only; Godot enforces at boundary),
after `as` as well as on a declaration. `.5` is a float literal (`..` and `a.b`
are not). A nested `class X:` takes `extends` from its own line or the body's
first line, not both.

### Structs

Compiler-only Dictionary with fixed key set. Instance = ordinary Dictionary
Variant (`{"balance": 0, "loan": 0}`). Construction: positional, named, or
mixed. Field access → Dictionary get/set (never `VGET`/`VSET` — those target
Object properties). Undeclared field = compile error when struct type known.
`d.key` ≡ `d["key"]` for plain Dictionaries too. Nothing survives into IR.
Tests: `tests/test_structs.cpp`.

### Project context

Autoloads and `class_name` scripts are unavailable from source.
`SafeGDScript::set_compiler_project_context()` reads ProjectSettings before each
compile (`set_autoloads`/`set_global_classes`, guarded by `has_function` for
older compiler ELFs). Stored in `CompilerOptions::autoloads`/`global_script_classes`.

- Autoload → `ECALL_GET_OBJ` on bare name. Resolves `/root/<name>` via
  `Engine::get_main_loop()`, not the owner's tree (`_init()` runs before tree
  entry).
- Script class `.new(args)` → `LOAD_RESOURCE` of path + `VCALL "new"`. No
  ClassDB entry; `ECALL_NODE_CREATE` takes no args. Both checked:
  `is_allowed_resource`, then `is_allowed_method`.

Unresolved capitalized name → type. `Class.CONSTANT` →
`ClassDB.class_get_integer_constant`, `Class.method(args)` →
`ClassDB.class_call_static`. No object involved — `ClassDB`/`OS` are in
`global_singleton_list`. Trade-off: misspelled class name → run-time null, not
compile error.

`self.method` as value → Callable (same as bare `method`); without it, reads as
owner property → null.

### Classes and engine interop

`class_name`/`extends` head the file (only annotations and each other may
precede). Neither reaches machine code — carried as metadata through `IRProgram`
→ `Compiler` → `SafeGDScript::_get_global_name()`/`_get_instance_base_type()`.
Path-based extends kept verbatim.

Two restriction gates:
- Per-use: `Class.new()`/`load()` checked at run time (`is_allowed_class`/
  `is_allowed_resource`). Always emitted.
- Structural: `class_name`/`extends`/native-base nested class refused at compile
  time when `CompilerOptions::restricted`. `SafeGDScript` reads
  `Sandbox::is_class_access_restricted()`. Restriction changes trigger
  `safegdscript_class_restrictions_changed()`, recompiling any `.sgd` whose
  `compiled_restricted` disagrees.

Class body: `var`, `func`, `const`, `static func`. `const` folds at use site —
non-foldable refused. Reached as `Class.NAME` or by shadowing bare name.
Inherited along declared chain. `static func`: no `self`, no fields; instance
method called as `Class.method()` refused. `static var` not a class member.

Nested `class X extends <EngineClass>`: Dictionary with `"@base"` holding
engine object. Unresolved access → `VGET`/`VSET`/`VCALL` on `@base`. Handle
reloaded from Dictionary per access, lifetime per Object handle rules. `signal`
in nested class = parse error.

Dictionary ↔ engine bridge in `sandbox_syscalls.cpp` (`class_instance_base()`):
- Engine method receives `@base`, not the Dictionary (`add_child(m)` works).
  Built-in Variant calls not rewritten.
- Dictionary method not in Dictionary's own set → retarget to `@base` via
  `get_object_from_address()` (restrictions apply). Dictionary methods take
  priority.

`"@class"` tag on instances enables `is`/`as` on untracked values. File
declares full chain → compile-time resolution (one Dictionary get vs. set). No
dynamic dispatch on untyped calls. Type hints settle dispatch; method returning
bare `self` settles it for chaining.

Class extending nothing = RefCounted. `typeof()` answers `DICTIONARY`; equal
fields compare equal.

Untracked access: `.x` on unknown type falls through to `@base` when key absent
(`gen_dynamic_member_get`/`_set`). Emitted only when script declares a class
with engine base. `extends <EngineClass>` at script level: bare name falls
through to `VGET`/`VSET` on owner. `SafeGDScript::_instance_create()` refuses
owner not matching declared base.

Tests: `tests/test_classes.cpp`.

### Function signatures

ELF symbols carry names only. `IRProgram::signatures` (`function_signature.h`)
publishes param names, types, return type, count alongside the ELF. Path:
`Compiler::get_function_signatures()` → `gdscript.elf` →
`SafeGDScript::update_methods_info()` → `MethodInfo`.
`SafeGDScriptInstance::callp` re-checks arity.

Serialized as one `PackedByteArray` (not Array of Dictionaries — stays under
`Sandbox::MAX_REFS`). Constant-foldable defaults travel with signature;
non-foldable → parameter required from Godot. Internal calls unaffected:
`gen_call` materializes defaults at call site.

Engine reads `default_arguments` as contiguous `Variant` array despite
`GDExtensionVariantPtr*` in `GDExtensionMethodInfo`.

### Editor integration

Signatures carry `line` (1-based, from `func` token) and `description` (`##`
doc-comment). `_get_member_line()` returns -1 for unknown (not 0).
`_get_documentation()` builds `DocData::ClassDoc` keyed by
`_get_doc_class_name()` — key mismatch silently drops entry. Completion:
`_get_script_method_list()` for `$Node.`/typed-var/`preload(...)`; `_get_method_info()`
for argument hints. `get_node("Node").` resolves only for `$`/`%` literals.
These virtuals are not GDScript-callable.

### Match/switch

Dense integer patterns → `SWITCH` jump table (O(1), no relocation). Density
thresholds: `MIN_SWITCH_CASES`, `MAX_SWITCH_SPREAD`, `MAX_SWITCH_ENTRIES` in
`codegen.cpp`. Table is fast path — falls through on non-integer/out-of-range.
Typed `int` subject → `JUMP` to wildcard; otherwise full equality chain. Any
variable pattern disqualifies table.

`const` materialized as immediate at use site (not `LOAD_GLOBAL`) — gives match
typed operands avoiding `VEVAL`. Container consts exempt (handle identity).

Five pattern kinds: value, Array destructure, Dictionary destructure, binding
(`var x`), wildcard (`_`). Optional `when` guard per arm. Guard declining →
continues chain (does not exit match). Guarded `_` not a catch-all. Bindings
are copies, scoped to arm. Container patterns: `TYPE_TEST` → length → elements.
Known type mismatch → skip (one jump). Array syscalls: `ECALL_ARRAY_SIZE`/
`ECALL_ARRAY_AT`. Dictionary: `ECALL_DICTIONARY_OPS` with `GET_SIZE`/`HAS`/`GET`.
Without trailing `..`, size is part of pattern. Container patterns excluded from
corpus.

Tests: `tests/test_switch.cpp`, `tests/test_match.cpp`.

### Debugging

`assert(cond, msg)`: literal message baked into `THROW`. Non-literal evaluated
on failing arm, passed as Variant in the syscall's spare slot; `api_throw`
prints String payload as message. Trailing count on `THROW` distinguishes form.

**Line table** (`line_table.h`): address→line map, always produced. Ascending by
address.

**Shadow stack** (`debug_layout.h`, `riscv_debug.cpp`): push/pop per call,
requires `CompilerOptions::debug_info`. Frames record return address (outer
frame line = call site).

**Breakpoints**: compile-time only. `ECALL_BREAKPOINT` emitted at requested
lines; zero cost when unset. Changing set recompiles all instances. Non-empty
set implies `debug_info`. Saves/restores `a0`/`a7` around syscall. Emitted below
labels (not above — back edges would skip). Optimized-away code gets no stop.
Break = non-returning syscall (no guest instructions burned); continue = return
from syscall. One program stopped at a time; `vmcall_internal` preempts via
`preempt_internal`. Stopped script refuses rebuild.

**Editor** (GDExtension — no toggle notifications, no engine-stop control):
`_frame()` polls `EngineDebugger` every 6th frame. `EngineDebugger::script_debug()`
replaces wait loop. Step Into/Over = next breakpoint. Locals/members empty.
Runtime-built scripts: `take_over_path()` before setting source.
`breakpoint_hit` listeners take priority over editor stop. `-d` flag required.

API: `set_breakpoint()`, `set_breakpoints()`, `clear_breakpoints()`, signal
`breakpoint_hit(script, line)` on `SafeGDScript`.

## Build and test

### Debugging tools (in compiler build folder)

`dump_ir`: GDScript → IR inspection.
```
cat script.gd | ./dump_ir                  # optimized IR
cat script.gd | ./dump_ir --no-optimize    # unoptimized IR
cat script.gd | ./dump_ir --codegen        # with register allocation
cat script.gd | ./dump_ir -v --codegen     # verbose operands
```

Both tools take the project context the host would supply:
`--autoload Global`, `--global-class Runner=res://runner.gd` (repeatable).

`gdscript_to_riscv`: GDScript → RV64 ELF → disassembly.
```
cat script.gd | ./gdscript_to_riscv            # all functions
cat script.gd | ./gdscript_to_riscv -f test    # specific function
```

### Compiler tests (ctest in `src/gdscript/compiler/build`)

- `test_ir_verifier` — operand roles, def-use, label resolution, type hints.
  Runs between every optimizer pass in debug builds.
- `test_opt_invariance` — optimizer must not change computed results. Bisects
  and names the offending pass on mismatch.
- `test_differential` — IR interpreter vs. libriscv machine on produced ELF.
  `--file program.gd` for reduction.
- `test_fuzz` — seeded generator (`tests/gdscript_generator.h`) + shrinker
  feeding the above two. `tests/fuzz_nightly.sh` for extended runs.
- `test_globals` — compile-time global functions.
- `test_operators` — operator lowering/precedence including engine deviations.
- `test_switch` — const folding, density thresholds, dispatch sequence.
- `test_match` — bindings, guards, container patterns, syscall accounting.

`GDSC_PASSES=<names>` selects optimizer passes (all tools including `dump_ir`).
`GDSC_PASSES=none` disables optimizer.

Adding an opcode to `ir_opcodes.def` is a compile error at every `IROpcode`
switch — no `default:`, built with `-Werror=switch`.

`STRIPPED=ON` links with `--strip-debug`, not `-s`: the Sandbox reads a guest
program's public API out of `.symtab`, and `--strip-all` leaves it unable to find
a single function.

### Integration tests

GDScript compiler unit tests under `tests/tests/`. RISC-V ELFs inspected with
`riscv64-linux-gnu-objdump`.

Run from `tests/` folder:
```
./run_unittests.sh -gselect compiler      # GDScript compiler tests
./zig_unittests.sh -gselect compiler      # Zig C++ unit tests
```

The compiler runs INSIDE a sandbox instance in all unit tests — a failure
originating from `compile_to_elf` means the compiler crashed inside the sandbox.

Always use a timeout for test execution (loops may diverge). Never disable
tests — fix the problem.
