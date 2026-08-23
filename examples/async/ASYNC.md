# Async

A cutscene that awaits a timer, a player signal and the next frame. The cutscene
is in the sandboxed `director.sgd` script. `main.gd` is the host game and awaits
it like any other coroutine.

```
main.gd        the host game
director.sgd   the cutscene, sandboxed
main.tscn      Log / Status / Knock / Runner / Director
```

## What crosses the boundary

A function that reaches an `await` suspends the call and returns a `Signal`:

```gdscript
var result = await director.cutscene(advance)
```

## What a coroutine can await

Anything that is a `Signal` on the guest side:

| Awaited                             | Waits for            |
| ----------------------------------- | -------------------- |
| `get_tree().create_timer(t).timeout` | Wall-clock time      |
| `get_tree().process_frame`          | The next frame       |
| `$"../Node".some_signal`            | A node in the scene  |
| a `Signal` parameter                | Whatever the host emits |

Note that awaiting on a non-Signal result does not suspend. Godot returns
the value immediately, so the function continues without yielding.
