extends Sandbox


# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	print(program.get_global_name())
	print(program is ELFScriptGlobal)
	var program_elf = program as ELFScriptGlobal
	print(program_elf.get_script_method_list())
	print(get_functions())
	# Make a function call into the sandbox
	print(vmcall("my_function", Vector4(1, 2, 3, 4)))
	
	# Create a callable Variant, and then call it later
	var callable = vmcallable("function3")
	callable.call(123, 456.0, "Test")

	# Pass a function to the sandbox as a callable Variant, and let it call it
	var ff : Callable = vmcallable("final_function");
	vmcall("trampoline_function", ff)
