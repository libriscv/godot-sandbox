extends Node


# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	var v = Sandbox.new()
	var f = FileAccess.open("res://test.elf", FileAccess.READ)
	v.load(f.get_buffer(f.get_length()), [])

	# Make a function call into the sandbox
	print(v.vmcall("my_function", Vector4(1, 2, 3, 4)))
	
	# Create a callable Variant, and then call it later
	var callable = v.vmcallable("function3")
	callable.call(123, 456.0, "Test")

	# Pass a function to the sandbox as a callable Variant, and let it call it
	var ff : Callable = v.vmcallable("final_function");
	v.vmcall("trampoline_function", ff)


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta: float) -> void:
	pass
