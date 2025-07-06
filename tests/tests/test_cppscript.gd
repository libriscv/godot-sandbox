extends GutTest

var Sandbox_TestsTests = load("res://tests/tests.elf")
var cpp = load("res://tests/tests.cpp")

func test_set_script():
	var n = Node.new()
	n.set_script(cpp)
	# Create an ELFScript instance
	var nn = Node.new()
	nn.set_script(Sandbox_TestsTests)
	# Connect the CPPScript to the ELFScript
	nn._connect_to(n)
	# Attach n under nn
	nn.add_child(n)

	assert_eq(n.test_int(1234), 1234, "Can call test_int function directly")
	assert_eq(n.get_tree_base_parent(), nn, "Can call get_tree_base_parent function directly")

	# Cleanup
	nn.queue_free()
	n.queue_free()
