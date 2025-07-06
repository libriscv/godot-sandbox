extends GutTest

var Sandbox_TestsTests = load("res://tests/tests.elf")
var cpp = load("res://tests/tests.cpp")

func test_set_script():
	var n = Node.new()
	n.set_script(cpp)
	# Create an ELFScript instance
	var nn = Sandbox.new()
	nn.set_script(Sandbox_TestsTests)
	# Sanity check that we can use the ELFScript
	assert_true(nn.execution_timeout > 0, "Can use property execution_timeout from ELFScript")
	assert_true(nn.is_allowed_object(n), "Can use is_allowed_object function from Node with ELFScript")
	# Connect the CPPScript to the ELFScript
	nn._connect_to(n)
	# Attach n under nn
	nn.add_child(n)

	# Verify that we can call methods from the ELF program
	assert_eq(n.test_int(1234), 1234, "Can call test_int function directly")
	assert_eq(n.get_tree_base_parent(), nn, "Verify node hierarchy is correct")

	# Verify that we also can call Sandbox-related functions through the Node
	assert_true(n.execution_timeout > 0, "Can use property execution_timeout from Node")
	# Also obscure functions
	assert_true(n.is_allowed_object(n), "Can use is_allowed_object function from Node")

	# Cleanup
	nn.queue_free()
	n.queue_free()
