extends Node


# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	var v = Sandbox.new();
	v.load(f.get_buffer(f.get_length()), []);


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta: float) -> void:
	pass
