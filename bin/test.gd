class_name Test123
extends Sandbox


# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	#var program_elf = program as ELF_test
	#print(program_elf)
	#program_elf.failing_function()
	print(program.get_global_name())
	print(get_functions())
	# Make a function call into the sandbox
	print(vmcall("my_function1", Vector4(1, 2, 3, 4)))
	
	# Create a callable Variant, and then call it later
	var callable = vmcallable("function3")
	callable.call(123, 456.0, "Test")

	# Pass a function to the sandbox as a callable Variant, and let it call it
	var ff : Callable = vmcallable("final_function123");
	vmcall("trampoline_function", ff)
