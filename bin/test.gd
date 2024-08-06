class_name Test123
extends Sandbox

@export var program_elf: ELF_src_src

# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	print("cast working? " + str(program_elf))
	print(get_functions())
	print(vmcall("my_function", Vector4(1, 2, 3, 4)))
