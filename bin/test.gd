extends Node

# NOTE: Build main.cpp (Ctrl+S) before running this demo project!
func _ready() -> void:
	# Casting the Sandbox to the unique program type gives us auto-completion in GDScript
	var sandbox = get_node("Sandbox") as Sandbox_SrcSrc
	# Prints the Vector4 argument, returns 123
	print(sandbox.my_function(Vector4(1, 2, 3, 4)))
	# Prints the string and array arguments
	sandbox.my_function2("Hello World!", Array([]))
