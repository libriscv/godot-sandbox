extends GutTest

func test_restrictions():
	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)

	# Adding an allowed object changes the Sandbox from unrestricted to restricted
	# access_a_parent
	assert_eq(s.has_function("access_a_parent"), true)
	# Create a new node
	var n = Node.new()
	n.name = "Node"
	# Set the sandbox as the parent of the node, so it can be accessed
	s.add_child(n)
	# Add an allowed object
	s.allow_object(n)
	# Now restrictions are in place
	var exceptions = s.get_exceptions()
	s.vmcall("access_a_parent", n)
	# The function should have thrown an exception, as we didn't allow the parent object
	assert_eq(s.get_exceptions(), exceptions + 1)

	# Allow the parent object
	s.allow_object(n.get_parent())
	# Now restrictions are in place
	exceptions = s.get_exceptions()
	s.vmcall("access_a_parent", n)
	# The function should *NOT* have thrown an exception, as we allowed the parent object
	assert_eq(s.get_exceptions(), exceptions)

	# Adding an allowed class changes ClassDB instantiation from unrestricted to restricted
	# creates_a_node
	assert_eq(s.has_function("creates_a_node"), true)
	# Add an allowed class
	s.allow_class("Node")
	# Now restrictions are in place
	exceptions = s.get_exceptions()
	s.vmcall("creates_a_node")
	# The function should *NOT* have thrown an exception, as we allowed the Node class
	assert_eq(s.get_exceptions(), exceptions)

	# Remove the allowed class Node, but allow the class Node2D
	s.remove_allowed_class("Node")
	s.allow_class("Node2D")
	# Now restrictions are in place
	exceptions = s.get_exceptions()
	s.vmcall("creates_a_node")
	# The function should have thrown an exception, as we only allowed the Node2D class
	assert_eq(s.get_exceptions(), exceptions + 1)

	# Disable all restrictions
	s.disable_all_restrictions()
	# Now restrictions are disabled
	exceptions = s.get_exceptions()
	s.vmcall("creates_a_node")
	# The function should *NOT* have thrown an exception, as we disabled all restrictions

	# Enable restrictions (by adding dummy values to allowed_classes and allowed_objects)
	s.enable_restrictions()
	# Now restrictions are enabled
	exceptions = s.get_exceptions()
	s.vmcall("creates_a_node")
	# The function should have thrown an exception, as we enabled restrictions
	assert_eq(s.get_exceptions(), exceptions + 1)

	s.queue_free()
