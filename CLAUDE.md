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

Resource handler for `.elf` files. No ELF parsing — libriscv's job.
Attaching one to a Node creates a Sandbox with that ELF loaded.

## Variant ABI

Layout: 4B type, 4B pad, 16B inline data = 24B (single-precision real_t).
Double-precision (`real_t = double`) widens vector payloads → 40B Variant.
`variant_layout.h` describes the layout; backend emits via `emit_flr`/`emit_fsr`/
`emit_fadd_r` so both builds share one code path. `CompilerOptions::double_precision`
selects at run time.

`ECALL_VGET`/`ECALL_VSET` take an object handle or scoped-variant index. Non-OBJECT
indices read/write the built-in's own member via `Variant::get_named`/`set_named`
— `transform.origin`, `basis.x`, `aabb.size` (payloads not inline). No object
involved, so no allowlist applies; restricted guests may use them (`test_fuzz.gd`
documents the split). Writes go through `get_mutable_scoped_variant()`, COW-ing
a borrowed caller Variant first: built-ins are values in GDScript.

Register convention: A0 = return Variant*, A1–A7 = argument Variant*.
`vmcallv()` keeps Variants boxed. Only live fields need valid data
(e.g. INTEGER: m_type + v.i = 12/24B). Object refs stored as 64-bit (`sd`).

`Variant::FLOAT` (v.f) is always `double`. `real_t` defaults to `float` and
governs vector components. `int + float` → `float`.

## Object handles

Guest handle = ObjectID | `OBJECT_HANDLE_TAG` (bit 62). Resolved via
`gdextension_interface_object_get_instance_from_id`.

Unrestricted: direct resolve, `ObjectBindingCache` (direct-mapped by id, no
mutex), handles persist across calls, `references_max` unenforced.

Restricted: `scoped_objects` = capability set, bounded by `references_max`,
extended by `m_allowed_objects` (ObjectID hash set).

RefCounted lifetime: intra-call via `scoped_refs` + `ref_dedup`, bounded by
`MAX_UNRESTRICTED_REFS`; cross-call via `ECALL_OBJ_RETAIN` on globals. Retain
refused while restricted; release always permitted.

Object Variant store is raw 24B move, never `VASSIGN`. Compiler emits retain for
globals with `IRGlobalVar::holds_object`.

## Script members

`SafeGDScriptInstance` runs `_init()` on creation, after
`create_instance_record()` lays down member defaults. Parameterized `_init()`
belongs to `Class.new(args)`; refused on a node.

Every non-const member registered with host at startup
(`IRGlobalVar::publishes_to_host()`), so `Object::get`/`set` reaches it like
a GDScript member. `@export` carries inspector usage flags; plain `var` carries
`PROPERTY_USAGE_SCRIPT_VARIABLE` alone — no inspector row, not scene-saved.
`emit_missing_export_accessors` synthesizes the missing half for members with
only a setter or getter. `Sandbox::MAX_PROPERTIES` = 256;
`MAX_GUEST_PROPERTY_SLOTS` (32) is the separate ABI-fixed length of the C++
guest API's `properties[]` array.

Member lifetime follows Variant type. Typed complex → `ECALL_VASSIGN`;
compiler-typed OBJECT → `ECALL_OBJ_RETAIN`. Untyped (or class-name declared,
not a Variant type) can hold either across calls, so storage is host-side:
`ECALL_VSTORE_GLOBAL` reads both tags, releases old value, copies scoped value
into a distinct permanent slot (shared slots dangle on release), retains
objects, copies inline payload verbatim.

`var x = null` declares no type — NIL is the held value, not the slot type —
so the slot stays untyped; first real assignment is not reclassification.

Declared type defaults via `__init_members` with zero-arg `CONSTRUCT` when no
compile-time form exists (`Transform2D` = IDENTITY, `Color` opaque black, `RID`
empty). Only `Object` starts NIL, matching GDScript. Also a lifetime invariant:
untouched slots carry VASSIGN's INT32_MIN sentinel, and `api_vassign` returns
the *source's* index — scoped, so a complex member assigned into an empty slot
dangled on call return.

A **struct**-typed declaration (`var p: Point`, member, local or field) is a
fresh instance: a struct is a value. A **class**-typed one is not — a class is
an object and GDScript leaves it null. It gets DICTIONARY's empty default rather
than NIL, because the slot is typed DICTIONARY and an empty complex slot has to
carry the sentinel above. Constructing one eagerly also ran the class's
`ECALL_CLASS_BIND` during `load_buffer` and leaked the engine object it built.

## Native backends

Two backends; only one built per target:

- **asmjit** (`RISCV_ASMJIT`) — in-process JIT to x86-64/AArch64. Default ON
  where asmjit has a ujit codegen; `ext/CMakeLists.txt` then defaults
  `RISCV_BINARY_TRANSLATION` (and libtcc) OFF, no-oping
  `emit_binary_translation()`, `try_compile_binary_translation()`,
  `load_binary_translation()`. Gate on `has_feature_binary_translation()`.
- **C99 binary translator** (`RISCV_BINARY_TRANSLATION`) — emits C, compiles
  out-of-process. SCons addon path; only option for iOS/Web/Switch. Default
  where asmjit has no codegen (x86-32, 32-bit ARM).

`has_feature_jit()` covers either backend. `is_jit()` is true for segments
still being compiled; poll `is_binary_translated()` to confirm code landed.

Windows cross-build: `mingw_toolchain.cmake`. `CMAKE_SYSTEM_NAME` is load-bearing
— without it CMake still targets Linux host, `CMAKE_SIZEOF_VOID_P` is empty
(godot-cpp bit-width `math(EXPR)` fails configure), and asmjit selects
mmap/shm_open instead of PE-appropriate backend.

## Guest memory checks

Decided at load by `unchecked_memory_wanted()` (`sandbox.h`), reported by
`get_unchecked_memory()`. Two independent knobs:

- **Unrestricted** → unchecked: JIT elides bounds checks (~50% cost on tight
  loops; nothing guest-reachable is guarded anyway).
- **Restricted** → always bounds-checked.
- `binary_translation_nbit_as` (**on by default**) AND-masks every access into
  the arena. Unrestricted: acts as guard — stray addresses wrap instead of
  reaching host (runaway stack faults guest, not Godot). Restricted: acts as
  speedup — mask replaces check (loses rodata guard, which cannot crash host).

Requires Po2 arena — hence `MAX_VMEM = 32`. Non-Po2 `memory_max` silently
disqualifies the mask.

int loop vs GDScript, JIT: unchecked+unmasked 21x, masked 16x either mode,
checked 9.8x.

Tests: `test_an_unrestricted_sandbox_runs_unchecked`,
`test_the_masked_arena_is_on_by_default` in `test_restrictions.gd`.

## GDScript-to-RISC-V compiler

`src/gdscript/compiler/`. GDScript → AST → IR → RV64 ELF targeting Godot
Sandbox.

### Where the compiler runs

`GDScriptCompilerBackend` (`src/safegdscript/compiler_backend.h`) is the one
interface the `.sgd` host drives. Two implementations:

- **sandboxed** (`sandboxed_compiler.cpp`) — `gdscript.elf` in its own Sandbox,
  reached by vmcall. A compiler bug faults that machine, not the process. Every
  entry point a previous release lacked is guarded by `has_function()`.
- **direct** (`direct_compiler.cpp`) — the same compiler linked into the
  extension, called in process. Faster (no guest machine, no ecall per answer)
  and gdb sees the compiler's own frames; a crash in it is a Godot crash.

`gdscript_compiler::Policy` picks per script, from the build setting
`SAFEGDSCRIPT_COMPILER` (CMake) / `safegdscript_compiler=` (SCons):

| value | policy |
| --- | --- |
| `sandboxed` | `gdscript.elf` for everything |
| `restricted` | direct, except a restricted Sandbox's scripts. **Default** |
| `direct` | direct for everything, restricted source included |

`restricted` means the source is a mod, not the project's: hostile input keeps
the machine between it and Godot. A policy wanting direct falls back to
sandboxed when the build linked no compiler in — `sandboxed` is also the only
value that does not link the compiler library into the addon.

`backend_for(restricted)` chooses; `prepare()` writes every input (restriction,
autoloads, global classes, base sources) so nothing survives from the last
build. Backends are stateful — the answers about a build are read back one
question at a time — so a compile and the questions after it must use the same
one.

### Union types

SafeGDScript accepts union hints such as `int | String` and nullable unions
such as `Node | null` on locals, members, parameters, returns, signals and
record fields. A slot still holds one ordinary Variant: known assignments are
checked while compiling and unknown values get one `TYPE_TEST_MASK` guard at
run time. A union without an initializer uses null when allowed, otherwise the
first member's normal default. Union parameters, returns and properties are
published to Godot as Variant because the engine ABI has no union type.

`is`, null/truthiness checks, `match` value types and `match typeof(x)` narrow
union locals for typed lowering. Members narrow only in branches that contain
no call and no assignment to that member, since either could invalidate a
re-read. `x is int | String` is one mask test. Union casts, exported unions and
multi-type unions containing script structs/classes are deliberately refused;
engine class members currently guard only the shared Object Variant tag.

### Nullable types

`T?` is accepted anywhere a type hint is accepted and is exactly the
single-member union `T | null`. Nullable locals, members, parameters and
returns accept only `T` or NIL; known assignments are checked at compile time
and unknown assignments use one `TYPE_TEST_MASK` guard. A nullable slot without
an initializer starts NIL, including nullable structs. A nullable member always
uses untyped Variant storage (`value_type == NONE`), even while it holds `T`:
keeping a concrete slot type while storing NIL corrupts host-side reads.

Known non-null assignments use the plain type's coercions, including `int` to
`float` widening and `Array` to packed-array construction. Null checks, `is`,
`assert` and the right side of `and` narrow a local to `T`, enabling the normal
inline member, arithmetic and iteration lowering. `if value:` narrows only the
true branch; its false branch is not NIL-only because values such as zero, an
empty string and `Vector2.ZERO` are falsy. Plain object hints remain nullable
for GDScript 4.x compatibility, so `Node?` is currently documentation-equivalent
to `Node`; value-typed nullable exports publish as Variant while object-typed
nullable exports retain their object class hint.

### @GlobalScope functions

One row per global in `globals.h`: name, arity, return type, lowering (inline
int/float, `ECALL_UTILITY`, or run-time dispatch). `int()`/`float()`/`bool()`/
`String()` in the same table — lowered inline when arg type is known
numeric/bool, host ecall otherwise. `randi`/`randf`/`randi_range`/`randf_range`/
`randfn` marked impure (DCE preserves side effect); IR interpreter refuses →
excluded from differential/optimizer-invariance corpus. `randomize()`/`seed()`
absent (mutate host RNG state).

### Operators

Precedence in `parser.cpp` (loosest → tightest): `as`, ternary, `or`, `and`,
`not`, `in`/`not in`, equality, comparison, `|`, `^`, `&`, shifts, `+`/`-`,
`*`/`/`/`%`, `is`/`is not`, `**`, unary `-`/`~`/`+`, call/subscript.

Engine-authoritative deviations from GDScript manual (tested in
`test_gdscript_compiler.gd`):
- `**` left-associative, binds below unary: `2**3**2` = 64, `-2**2` = 4.
- `not` binds below comparison: `not a == b` ≡ `not (a == b)`.

Lowering:
- `**`/`in` → `ECALL_VEVAL` (OP_POWER=13, OP_IN=24); no native lowering. IR
  interpreter refuses both → excluded from corpus.
- unary `-` → inline for typed INT (`neg`) and FLOAT (sign-bit XOR; preserves
  `-0.0` and NaN payloads), otherwise `ECALL_VEVAL` OP_NEGATE=10. Not lowered
  as `0 - x`: Godot defines no `int - Vector2`, so non-numeric negation
  silently yielded NIL. `-Color` inverts per engine semantics.
- `is <builtin_type>` → `lw`/`xori`/`seqz` on type tag (exact match, not
  convertibility). Folds when type tracked.
- `is <class_name>` → `gen_class_test`: `Object.is_class()` engine chain, then
  `get_script()`/`get_global_name()`/`get_base_script()` script chain.
  Non-object → false. Tracked instances: own name/ancestors fold true, siblings
  fold false, engine names walk `@base`.
- `as <builtin>` → single-arg ctor. int/float/bool/String via globals rows
  (inline when numeric); rest via `ECALL_VCONSTRUCT`. Tracked match folds;
  mismatch = run-time error, never null. `as Variant` = identity.
  `Variant::type_from_name` (variant_types.h) resolves name → type, shared with
  `is` and type hints.
- `as <class_name>` → `is` + value-or-null, preserving instance declaration.
  Both forms parse to `CastExpr`; codegen discriminates via `type_from_name`.

### Built-in constructors

`INLINE_CONSTRUCTORS` (codegen.cpp): types whose payload fits inline Variant
bytes — `MAKE_VECTOR2` etc., no syscall. `HOST_CONSTRUCTORS`: Transform2D/3D,
Basis, Quaternion, AABB, Projection, StringName, NodePath, RID, Signal —
lowered to `gdextension_interface_variant_construct` via `ECALL_VCONSTRUCT`.
Resolves overloads and conversions, max 8 args.

Inline types with no component-wise form (`Vector2(Vector2i)`, `Color(String)`)
fall through to `ECALL_VCONSTRUCT`. `Vector2(1)` = compile error (no
single-scalar ctor). Array assigned to declared `Packed*Array` converts via
that type's ctor.

### Enums

Compiler-only. Members fold to integer immediates, typed `int`. Nothing reaches
IR. Undeclared member = compile error; locals shadow.

Enum *as a value* = the Dictionary GDScript exposes, materialized at use site
by `gen_enum_dictionary` (String keys, `MAKE_DICTIONARY`). `E.values()`,
`E.keys()`, `E[name]`, `for k in E` read it; `E.MEMBER` resolves first and
still folds (no Dictionary materialized).

Exception: engine-constant initializers (`PhysicsServer2D.SHAPE_CAPSULE`) have
no compile-time value. `Parser::holds_engine_constant` keeps the initializer in
`EnumDecl::owned_values`; `Member::value_expr` points at it, `value` becomes
auto-increment offset. `gen_enum_member` re-evaluates per use. Folding refused;
bad literal (`A = "hi"`) = compile error.

### Container iteration

- `for v in <array>`: `ECALL_ARRAY_SIZE` + `ECALL_ARRAY_AT`.
- `for k in <dict>`: setup replaces Dictionary with `Dictionary_Op::GET_KEYS`
  once. `TYPE_TEST` guard when type unknown.
- `GET_KEYS`/`GET_VALUES`: result Variant* in a2 (no key argument).
- `for c in <string>` (known String): `gen_string_walk`.
  `ECALL_STRING_BATCH` returns up to 16 chars in consecutive scoped slots as
  `(first_index << 32) | count`; yielding a character is `MAKE_SCOPED` on an
  index (tag store, no syscall). String is a value type — walk COWs the Variant
  first. Two scopes: outer for batch (released on refill), inner marked after
  each refill and released every pass. Host clamps count against
  `references_max`. Untyped iterables use four-way `TYPE_TEST` dispatch with
  per-character `ECALL_STRING_AT`.

### Scoped variant release

`SCOPE_MARK`/`SCOPE_RELEASE` (`open_scope`/`emit_scope_release`, `codegen.cpp`)
bound a call's live set. Loops mark above loop label, release under it; blocks
(`if`/`else`/`match` arms, via `push_block_scope`/`pop_block_scope`) mark at
entry, release at `pop_scope`, so `break`/`continue`/`return` skip the release
and the enclosing loop or call reclaims instead. Without block scopes
`references_max` (`Sandbox::MAX_REFS`, 100) bounds *total* temporaries, not
live ones, and long straight-line functions exhaust it pointlessly.

Outward-escaping values spilled to frame slots before release
(`spill_all_registers`); host's release walk rescues handles found in frame,
globals area, and members area — enclosing locals, members, return values all
survive. `plan_release_clears` zeroes dead slots (per liveness), preventing
rescue of unreferenced values. Coroutines take no scope: suspension restores
slots but not the mark.

`RISCVCodeGen::plan_scopes` elides both halves of pairs whose span allocates
nothing (`instruction_may_allocate_scoped`) — most blocks emit nothing, which
matters because `SCOPE_RELEASE` is an ecall + full spill. Surviving scopes
packed into frame slots (`m_fn.scope_slots`); elided blocks cost nothing.

Tests: `tests/test_scope.cpp`.

### Statement layout

Newline inside `(`/`[`/`{` = continuation. `\` = explicit continuation; `;` =
statement separator. Trailing commas allowed. `{key = value}` = Lua-style
`{"key": value}`. Unclosed bracket swallows file; diagnosed at open position.
`static` parsed and dropped (no class instance). Typed containers (`Array[int]`)
parsed and dropped (Variant-only; Godot enforces at boundary), after `as` as
well as on declarations. `.5` is a float literal (`..` and `a.b` are not).
Nested `class X:` takes `extends` from its own line or body's first line, not
both.

### Structs

Compiler-only Dictionary with fixed key set. Instance = ordinary Dictionary
Variant (`{"balance": 0, "loan": 0}`). Construction: positional, named, or
mixed. Field access → Dictionary get/set (never `VGET`/`VSET` — those target
Object properties). Undeclared field = compile error when struct type known.
`d.key` ≡ `d["key"]` for plain Dictionaries too. Nothing survives into IR.
Tests: `tests/test_structs.cpp`.

### Project context

Autoloads and `class_name` scripts unavailable from source.
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
`ClassDB.class_call_static`. No object involved — `ClassDB`/`OS` in
`global_singleton_list`. Trade-off: misspelled class name → run-time null, not
compile error.

`self.method` as value → Callable (same as bare `method`); without it, reads as
owner property → null.

### Classes and engine interop

`class_name`/`extends` head the file (only annotations and each other may
precede). Neither emitted to machine code — carried as metadata through
`IRProgram` → `Compiler` → `SafeGDScript::_get_global_name()`/
`_get_instance_base_type()`. Path-based extends kept verbatim.

Two restriction gates:
- Per-use: `Class.new()`/`load()` checked at run time (`is_allowed_class`/
  `is_allowed_resource`). Always emitted.
- Structural: `class_name`/`extends`/native-base nested class refused at compile
  time when `CompilerOptions::restricted`. `SafeGDScript` reads
  `Sandbox::is_class_access_restricted()`. Restriction changes trigger
  `safegdscript_class_restrictions_changed()`, recompiling any `.sgd` whose
  `compiled_restricted` disagrees.

Class body: `var`, `func`, `const`, `static func`. `const` folds at use site;
non-foldable refused. Reached as `Class.NAME` or by local shadow. Inherited
along declared chain. `static func`: no `self`, no fields; instance method
invoked as `Class.method()` refused. `static var` not a class member.

Nested `class X extends <EngineClass>`: Dictionary with `"@base"` holding
engine object. Unresolved access → `VGET`/`VSET`/`VCALL` on `@base`. Handle
reloaded from Dictionary per access, lifetime per Object handle rules. `signal`
in nested class = parse error.

Dictionary ↔ engine bridge in `sandbox_syscalls.cpp` (`class_instance_base()`):
- Engine methods receive `@base`, not the Dictionary (`add_child(m)` works).
  Built-in Variant calls not rewritten.
- Dictionary methods not in Dictionary's own set → retargeted to `@base` via
  `get_object_from_address()` (restrictions apply). Dictionary methods win.

`"@class"` tag enables `is`/`as` on untracked instances. Full chain declared in
file → compile-time resolution (one Dictionary get vs. set). No dynamic dispatch
on untyped calls. Type hints settle dispatch; method returning bare `self`
settles it for chaining.

Class extending nothing = RefCounted. `typeof()` = `DICTIONARY`; equal fields
compare equal.

Untracked access: `.x` on unknown type falls through to `@base` when key absent
(`gen_dynamic_member_get`/`_set`). Emitted only when script declares a class
with engine base. `extends <EngineClass>` at script level: bare name falls
through to `VGET`/`VSET` on owner. `SafeGDScript::_instance_create()` refuses
owner not matching declared base.

Tests: `tests/test_classes.cpp`.

### Nested-class script instances

A nested class with an engine base is also a Script resource of its own
(`SafeGDScriptClass`), and its `@base` object carries a script instance of that
(`SafeGDScriptClassInstance`) — `src/safegdscript/script_class_safegdscript.cpp`.
The Dictionary stays the guest-side identity; the instance is the host-side one
and holds that same Dictionary, so `obj.get("field")` and the guest see one set
of values (Godot's Dictionary is a handle). Without it Godot only ever saw a bare
`CanvasGroup`: no `_ready`/`_process`/`_input`, no `has_method`, no `get_script`.

`gen_class_construct` emits `ECALL_CLASS_BIND` after `MAKE_DICTIONARY` and before
the class's `_init`, so `_init` already runs with the script attached. The IR
interpreter refuses `CALL_SYSCALL`, so the corpus is unaffected.

- `ClassSignature` (`function_signature.h`) publishes name, declared base,
  `native_base()`, inherited fields and declared methods; its own blob and its
  own `class_signatures` entry point, because a section appended to the function
  blob would not decode against an older `gdscript.elf`. Lifted methods now carry
  full signatures under `@Class.method`, minus the synthetic `self`.
- Dispatch: `callp` resolves `@Class.name` up the declared chain via
  `cached_address_of`, checks arity with the same helper the outer instance uses
  (`call_arguments.h`), and passes the Dictionary in the first slot.
- `_can_instantiate()` is **true**: `Object::set_script()` only builds an
  instance when the Script says so. Attaching one by hand is refused in
  `_instance_create()` instead, where `pending_self` is empty.
- `super.method()` on a native base becomes `ECALL_VCALL_SUPER`
  (`IRInstruction::super_call`). The handler arms `bypass_once()`, `callp`
  answers `INVALID_METHOD` once, and `Object::callp` falls through to the
  engine's `MethodBind`. Only a name the class *shadows* needs it; anything it
  does not declare already falls through. Engine virtuals with no `MethodBind`
  (`super._ready()`) fail — GDScript refuses those at parse time.
- **RefCounted bases get no script.** `@base` is a strong reference, the object
  owns the instance, the instance holds the Dictionary: a cycle whose only exit
  is `refcount_decremented()`, which would mean dropping the last reference from
  inside the object's own `unreference()`. `extends Resource` stays what it was.
- A member declared with a nested class type (`var l: Launcher2D`) is
  constructed by `__init_members`, so its bind runs during `load_buffer`.
  `create_sandbox()` registers the machine in `sandbox_instances` *before*
  loading for that reason.
- Classes are rebuilt in place on recompile (`rebuild_nested_classes`), so
  `get_script()` is identity-stable across a reload; a class dropped from the
  source is invalidated and its instances answer `INVALID_METHOD`.
- `_notification` now dispatches on the outer `SafeGDScriptInstance` too.

Tests: `test_sgd_a_nested_class_*` in `tests/tests/test_gdscript_compiler.gd`,
`test_the_bind_syscall_is_emitted_for_an_engine_base` and
`test_class_signatures_are_published` in `tests/test_classes.cpp`.

### Function signatures

ELF symbols carry names only. `IRProgram::signatures` (`function_signature.h`)
publishes param names, types, return type, count alongside the ELF.
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
`_get_doc_class_name()` — key mismatch silently drops the entry. Completion:
`_get_script_method_list()` for `$Node.`/typed-var/`preload(...)`;
`_get_method_info()` for arg hints. `get_node("Node").` resolves only for
`$`/`%` literals. These virtuals are not GDScript-callable.

### Match/switch

Dense integer patterns → `SWITCH` jump table (O(1), no relocation). Density
thresholds: `MIN_SWITCH_CASES`, `MAX_SWITCH_SPREAD`, `MAX_SWITCH_ENTRIES` in
`codegen.cpp`. Table = fast path; falls through on non-integer/out-of-range.
Typed `int` subject → `JUMP` to wildcard; otherwise full equality chain.
Variable patterns disqualify table.

`const` materialized as immediate at use site (not `LOAD_GLOBAL`) — gives match
typed operands, avoiding `VEVAL`. Container consts exempt (handle identity).

Five pattern kinds: value, Array destructure, Dictionary destructure, binding
(`var x`), wildcard (`_`). Optional `when` guard per arm; guard declining →
continues chain (does not exit match). Guarded `_` not a catch-all. Bindings
are copies, scoped to arm. Container patterns: `TYPE_TEST` → length → elements;
known type mismatch → elided (one jump). Array: `ECALL_ARRAY_SIZE`/
`ECALL_ARRAY_AT`. Dictionary: `ECALL_DICTIONARY_OPS` with `GET_SIZE`/`HAS`/`GET`.
Without trailing `..`, size is part of pattern. Container patterns excluded from
corpus.

Tests: `tests/test_switch.cpp`, `tests/test_match.cpp`.

### Debugging

`assert(cond, msg)`: literal message baked into `THROW`. Non-literal evaluated
on failing arm, passed as Variant in syscall's spare slot; `api_throw` prints
String payload. Trailing count on `THROW` distinguishes the two forms.

**Line table** (`line_table.h`): address→line map, always produced. Ascending by
address.

**Shadow stack** (`debug_layout.h`, `riscv_debug.cpp`): push/pop per call,
requires `CompilerOptions::debug_info`. Frames record return address (outer
frame line = call site).

**Breakpoints**: compile-time only. `ECALL_BREAKPOINT` emitted at requested
lines; zero cost when unset. Changing the set recompiles all instances. Non-empty
set implies `debug_info`. Saves/restores `a0`/`a7` around ecall. Emitted below
labels (not above — back edges would skip). Dead code gets no stop. Break =
non-returning ecall (no guest insns burned); continue = ecall return. One
program stopped at a time; `vmcall_internal` preempts via `preempt_internal`.
Stopped script refuses rebuild.

**Editor** (GDExtension — no toggle notifications, no engine-stop control):
`_frame()` polls `EngineDebugger` every 6th frame.
`EngineDebugger::script_debug()` replaces wait loop. Step Into/Over = next
breakpoint. Locals/members empty. Runtime-built scripts: `take_over_path()`
before setting source. `breakpoint_hit` listeners take priority over editor
stop. `-d` flag required.

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

Both accept host-equivalent project context:
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

Adding an opcode to `ir_opcodes.def` triggers compile errors at every
`IROpcode` switch — no `default:`, built with `-Werror=switch`.

`STRIPPED=ON` links with `--strip-debug`, not `-s`: Sandbox reads the guest's
public API from `.symtab`; `--strip-all` destroys it.

### Integration tests

Compiler integration tests under `tests/tests/`. ELFs inspected with
`riscv64-linux-gnu-objdump`.

Run from `tests/` folder:
```
./run_unittests.sh -gselect compiler      # GDScript compiler tests
./zig_unittests.sh -gselect compiler      # Zig C++ unit tests
```

The compiler runs INSIDE a sandbox instance in all unit tests — failure from
`compile_to_elf` = compiler crashed inside the sandbox.

Always use a timeout for test execution (loops may diverge). Never disable
tests — fix the root cause.
