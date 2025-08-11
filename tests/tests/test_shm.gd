extends GutTest

var Sandbox_TestsTests = load("res://tests/tests.elf")

func test_shared_memory():
	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)

	assert(s.has_function("test_shm"))

	# Create a packed array of floats
	var array = PackedFloat32Array([1.0, 2.0, 3.0, 4.0, 5.0])

	# Share the array with the node read+write
	var vaddr = s.share_float32_array(true, array)
	var result = s.vmcall("test_shm", vaddr, array.size())

	# The function should double the values in the array
	assert_eq_deep(result, PackedFloat32Array([2.0, 4.0, 6.0, 8.0, 10.0]))

	# After unsharing, the original array should also reflect the changes
	assert_true(s.unshare_array(vaddr), "Unsharing the array should succeed")
	assert_eq_deep(array, PackedFloat32Array([2.0, 4.0, 6.0, 8.0, 10.0]))

	s.queue_free()
