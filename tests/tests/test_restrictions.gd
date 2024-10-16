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

	# Allow the parent object using a callback
	s.remove_allowed_object(n.get_parent())
	s.set_object_allowed_callback(func(sandbox, obj): return obj == n.get_parent())
	# Now restrictions are in place
	exceptions = s.get_exceptions()
	s.vmcall("access_a_parent", n)
	# The function should *NOT* have thrown an exception, as we allowed the parent object
	assert_eq(s.get_exceptions(), exceptions)

	# Setting a callback for allowed classes changes ClassDB instantiation from unrestricted to restricted
	# creates_a_node
	assert_eq(s.has_function("creates_a_node"), true)
	# Add an allowed class (Node)
	s.set_class_allowed_callback(func(sandbox, name): return name == "Node")
	# Now restrictions are in place
	assert_true(s.is_allowed_class("Node"), "Node should be allowed")
	exceptions = s.get_exceptions()
	s.vmcall("creates_a_node")
	# The function should *NOT* have thrown an exception, as we allowed the Node class
	assert_eq(s.get_exceptions(), exceptions)

	# Now only allow the class Node2D
	s.set_class_allowed_callback(func(sandbox, name): return name == "Node2D")
	# Now restrictions are in place
	assert_true(s.is_allowed_class("Node2D"), "Node2D should be allowed")
	assert_false(s.is_allowed_class("Node"), "Node should not be allowed")
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
	assert_eq(s.get_exceptions(), exceptions + 1, "Should have thrown an exception")

	s.queue_free()

func test_insanity():
	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)

	assert_eq(s.has_function("access_an_invalid_child_node"), true)

	s.enable_restrictions()
	s.set_class_allowed_callback(func(sandbox, name): return name == "Node")
	#s.set_object_allowed_callback(func(sandbox, obj): return obj.get_name() == "Node")
	#s.set_method_allowed_callback(func(sandbox, obj, method): return method == "get_name")

	var exceptions = s.get_exceptions()
	s.vmcall("access_an_invalid_child_node")

	assert_eq(s.get_exceptions(), exceptions + 1)

	s.queue_free()
