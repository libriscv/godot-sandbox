# Testing

`@test` functions live next to the code they cover, inside the sandbox.

```
inventory.sgd  the code and its tests
main.gd        a scene that runs them and shows the report
main.tscn      Summary / Report / Rerun
```

## Writing a test

- `@test` function, no arguments, not a coroutine
- Fails on a failed `assert()` or any other sandbox exception
- Each test gets a fresh script instance (`_init()` reruns, member defaults reset)
- Instances of one script share a machine (one record, not a program load)
- No tree: `_ready`, `_enter_tree`, `_process` do not run, `$Child` is null
- Tree-dependent tests belong in GUT

```gdscript
@test func test_an_equal_item_stacks() -> void:
	add("coin")
	assert(add("coin", 4) == true, "the second coin stacks")
	assert(total("coin") == 5)
```

## Running from the editor

- Right-click the script in FileSystem dock, Scene dock or script list → **Run Tests**
- Right-click inside the code → **Run Test at Cursor**
- Failures land in Output dock and jump to the failing line

## Running headlessly

```
godot --headless --path . -s addons/godot_sandbox/run_sgd_tests.gd
```

No paths → walks `res://` for `.sgd` files containing `@test`.
Exit code = failures + errors, capped at 125.

| Option | Effect |
| --- | --- |
| `--only <name>` | Run only the named `@test`. Repeatable |
| `--list` | List scripts and tests, then exit |
| `--json` | JSON report to stdout, nothing else |
| `--junit <path>` | JUnit XML report to `<path>` |
| `--fail-fast` | Stop after the first script that fails |
| `--help` | Usage text |

## CI (JUnit)

```yaml
- run: godot --headless --path . -s addons/godot_sandbox/run_sgd_tests.gd -- --junit report.xml
- uses: mikepenz/action-junit-report@v4
  with:
    report_paths: report.xml
```

## GUT

Add `res://addons/godot_sandbox/gut` to GUT's test directories.
Every `@test` becomes a GUT test case.

To narrow the search, subclass the bridge:

```gdscript
extends "res://addons/godot_sandbox/gut/test_safegdscript.gd"

func sgd_paths() -> PackedStringArray:
	return PackedStringArray(["res://scripts"])
```

## Shipping

Exported builds compile with tests off. No `@test` function reaches the ELF.

## Running this project

The examples ship without the addon. Install Godot Sandbox into `res://addons/godot_sandbox` first, or open from a checkout that already has it.
