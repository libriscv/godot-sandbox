extends Node2D

## The host game. The cutscene is director.sgd, running in a sandbox.
##
## A sandboxed function that reaches an `await` returns a Signal instead of a
## value. The call site is identical to a .gd coroutine.

## Emitted when the player answers. The guest awaits this directly.
signal advance(answer : String)

# Untyped on purpose: cutscene() comes from the .sgd script, not from Node.
@onready var director = $Director
@onready var log_label : Label = $Log
@onready var status : Label = $Status
@onready var knock : Button = $Knock

func _ready() -> void:
	knock.pressed.connect(func(): advance.emit("hello?"))
	_play()

func _play() -> void:
	while true:
		# The guest suspends on its first await and returns a Signal.
		# Awaiting that Signal yields the coroutine's return value.
		var result = await director.cutscene(advance)
		print("[main] ", result)
		log_label.text = str(result)
		await get_tree().create_timer(1.5).timeout

func _process(_delta: float) -> void:
	# Each suspended coroutine holds a copy of the guest function's locals
	# until it is resumed or dropped.
	status.text = "suspended coroutines: %d" % director.get_coroutine_count()
