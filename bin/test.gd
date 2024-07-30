extends Sandbox


# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	# Make a function call into the sandbox
	print(vmcall("my_function", Vector4(1, 2, 3, 4)))
	
	# Create a callable Variant, and then call it later
	var callable = vmcallable("function3")
	callable.call(123, 456.0, "Test")

	# Pass a function to the sandbox as a callable Variant, and let it call it
	var ff : Callable = vmcallable("final_function");
	vmcall("trampoline_function", ff)
