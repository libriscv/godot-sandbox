extends Node

@export var node: Node

# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	print(node)
	var cast = node as Test123
	print(cast)


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta: float) -> void:
	pass
