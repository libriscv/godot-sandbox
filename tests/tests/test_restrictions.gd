extends GutTest

var Sandbox_TestsTests = load("res://tests/tests.elf")

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
	s.add_allowed_object(n)
	# Now restrictions are in place
	var exceptions = s.get_exceptions()
	s.vmcall("access_a_parent", n)
	# The function should have thrown an exception, as we didn't allow the parent object
	assert_engine_error("Exception: Node::get_parent(): Parent is not allowed")
	assert_eq(s.get_exceptions(), exceptions + 1)

	# Allow the parent object
	s.add_allowed_object(n.get_parent())
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
	assert_engine_error("Class name is not allowed")
	assert_engine_error("Exception: Class name is not allowed")
	assert_eq(s.get_exceptions(), exceptions + 1)

	# Disable all restrictions
	s.restrictions = false
	# Now restrictions are disabled
	exceptions = s.get_exceptions()
	s.vmcall("creates_a_node")
	# The function should *NOT* have thrown an exception, as we disabled all restrictions

	# Enable restrictions (by adding dummy values to allowed_classes and allowed_objects)
	s.restrictions = true
	# Now restrictions are enabled
	exceptions = s.get_exceptions()
	s.vmcall("creates_a_node")
	# The function should have thrown an exception, as we enabled restrictions
	assert_engine_error("Class name is not allowed")
	assert_engine_error("Exception: Class name is not allowed")
	assert_eq(s.get_exceptions(), exceptions + 1, "Should have thrown an exception")

	s.queue_free()

func test_restriction_callbacks():
	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)

	s.set_object_allowed_callback(func(sandbox, obj): return obj.get_name() == "Test")
	var n = Node.new()
	n.name = "Test"
	assert_true(s.is_allowed_object(n), "Test node should be allowed")

	s.set_method_allowed_callback(func(sandbox, obj, method): return method != "free")
	assert_true(s.is_allowed_method(n, "queue_free"), "Node.queue_free() should be allowed")
	assert_false(s.is_allowed_method(n, "free"), "Node.free() should *NOT* be allowed")

	s.set_property_allowed_callback(func(sandbox, obj, property, is_set): return property != "owner")
	assert_true(s.is_allowed_property(n, "name"), "Node.get/set_name should be allowed")
	assert_false(s.is_allowed_property(n, "owner"), "Node.get/set_owner should *NOT* be allowed")

	s.set_class_allowed_callback(func(sandbox, name): return name == "Node")
	assert_true(s.is_allowed_class("Node"), "Node creation should be allowed")
	assert_false(s.is_allowed_class("Node2D"), "Node2D creation should *NOT* be allowed")

	s.set_resource_allowed_callback(func(sandbox, name): return name == "res://test.tscn")
	assert_true(s.is_allowed_resource("res://test.tscn"), "Resource should be allowed")
	assert_false(s.is_allowed_resource("res://other.tscn"), "Resource should *NOT* be allowed")

	s.queue_free()


func test_insanity():
	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)

	assert_true(s.has_function("access_an_invalid_child_node"), "access_an_invalid_child_node should exist")

	s.restrictions = true
	s.set_class_allowed_callback(func(sandbox, name): return name == "Node")
	assert_true(s.is_allowed_class("Node"), "Node should be allowed")

	#s.set_object_allowed_callback(func(sandbox, obj): return obj.get_name() == "Node")
	#s.set_method_allowed_callback(func(sandbox, obj, method): return method == "get_name")

	var exceptions = s.get_exceptions()
	s.vmcall("access_an_invalid_child_node")
	assert_engine_error("Banned method called: add_child")
	assert_engine_error("Exception: Banned method called: add_child")

	assert_eq(s.get_exceptions(), exceptions + 1)


	# access_an_invalid_child_resource
	assert_true(s.has_function("access_an_invalid_child_resource"), "access_an_invalid_child_resource should exist")
	s.set_method_allowed_callback(func(sandbox, obj, method): return method == "get_class")

	# allow a resource that can be loaded and instantiated
	s.set_resource_allowed_callback(func(sandbox, name): return name == "res://tests/test.elf")
	assert_true(s.is_allowed_resource("res://tests/test.elf"), "Resource should be allowed")

	exceptions = s.get_exceptions()
	var inst = s.vmcall("access_an_invalid_child_resource", "res://tests/test.elf")
	# The function should *NOT* have thrown an exception, as we allowed the resource
	assert_eq(s.get_exceptions(), exceptions)
	assert_eq(inst, "ELFScript", "the allowed resource loaded and answered the call")

	s.vmcall("access_an_invalid_child_resource", "res://other.tscn")
	# The function should have thrown an exception, as we didn't allow the resource
	assert_engine_error("Resource path is not allowed: res://other.tscn")
	assert_engine_error("Exception: Resource path is not allowed: res://other.tscn")
	assert_eq(s.get_exceptions(), exceptions + 1)

	# disable_restrictions
	assert_true(s.has_function("disable_restrictions"), "disable_restrictions should exist")

	s.restrictions = true
	s.vmcall("disable_restrictions")
	# The function should have denied disabling restrictions, as it is forbidden
	# to disable restrictions from within the sandbox
	assert_engine_error("Banned method called: disable_restrictions")
	assert_engine_error("Exception: Banned method called: disable_restrictions")
	assert_eq(s.restrictions, true)

	s.queue_free()


func test_allowed_objects_survive_load_buffer():
	# Which objects the host is willing to expose has nothing to do with which program is
	# loaded, but a full reset used to clear the allowed-objects list along with the
	# machine, quietly turning the sandbox back into an unrestricted one.
	var elf_bytes := FileAccess.get_file_as_bytes("res://tests/tests.elf")
	assert_false(elf_bytes.is_empty(), "tests.elf should be readable as bytes")

	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)

	var n = Node.new()
	n.name = "Kept"
	s.add_allowed_object(n)
	assert_true(s.is_allowed_object(n), "the Node was just allowed")
	assert_false(s.is_allowed_object(s), "a non-empty list makes everything else denied")

	s.load_buffer(elf_bytes)
	assert_true(s.is_allowed_object(n), "load_buffer() must keep the allowed-objects list")
	assert_false(s.is_allowed_object(s), "load_buffer() must not leave the sandbox unrestricted")

	n.queue_free()
	s.queue_free()


func test_allowed_refcounted_is_kept_alive():
	# An allowed object is named by its ObjectID, and an id outlives the object it names.
	# A RefCounted has to be held by the list as well: nothing else here owns it, and once
	# freed its address is free to be handed to whatever is allocated next.
	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)

	# An unrelated entry, so that the list never empties out -- an empty list means the
	# sandbox is unrestricted, and then everything is allowed again.
	var other := Node.new()
	s.add_allowed_object(other)

	var res := RefCounted.new()
	var id := res.get_instance_id()
	s.add_allowed_object(res)
	res = null

	assert_true(is_instance_id_valid(id), "an allowed RefCounted must not be freed")
	var kept = instance_from_id(id)
	assert_true(s.is_allowed_object(kept), "losing the caller's reference does not lose the entry")

	s.remove_allowed_object(kept)
	assert_false(s.is_allowed_object(kept), "the entry is gone after removing it")
	kept = null
	assert_false(is_instance_id_valid(id), "removing the entry releases the reference")

	other.queue_free()

	s.queue_free()


func test_object_handle_kept_across_calls():
	# The guest may store an Object handle and use it in a later call, where nothing scopes
	# it any more. An empty allowed-objects list means "unrestricted", which says nothing
	# about an address the guest is holding, so that has to be refused until the host has
	# actually named the object.
	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)
	assert_true(s.has_function("store_object"), "store_object should exist")

	var n = Node.new()
	n.name = "Remembered"
	s.vmcall("store_object", n)

	var exceptions = s.get_exceptions()
	s.vmcall("use_stored_object")
	assert_engine_error("Object is not scoped")
	assert_engine_error("Exception: Object is not scoped")
	assert_eq(s.get_exceptions(), exceptions + 1)

	# Now say so explicitly, and the same handle resolves.
	s.add_allowed_object(n)
	assert_eq(s.vmcall("use_stored_object"), "Node", "an allowed object resolves from a stored handle")
	assert_eq(s.get_exceptions(), exceptions + 1)

	n.queue_free()
	s.queue_free()


func test_a_grant_during_a_vm_call_takes_effect():
	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)
	assert_true(s.has_function("granted_mid_call"), "granted_mid_call should exist")

	var anchor = Node.new()
	s.add_allowed_object(anchor)

	var granted = Node.new()
	granted.name = "Granted"
	var exceptions = s.get_exceptions()
	var grant := func():
		s.add_allowed_object(granted)
		return granted
	assert_eq(s.vmcall("granted_mid_call", grant), "Node", "a mid-call grant crosses back to the guest")
	assert_eq(s.get_exceptions(), exceptions)

	assert_eq(s.vmcall("use_stored_object"), "Node", "the granted object resolves in a later call")
	assert_eq(s.get_exceptions(), exceptions)

	granted.queue_free()
	anchor.queue_free()
	s.queue_free()


func test_without_the_grant_the_object_does_not_cross():
	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)

	var anchor = Node.new()
	s.add_allowed_object(anchor)

	var ungranted = Node.new()
	ungranted.name = "Ungranted"
	var exceptions = s.get_exceptions()
	s.vmcall("granted_mid_call", func(): return ungranted)
	assert_engine_error("Exception: GuestVariant::create(): Object is not allowed")
	assert_true(s.get_exceptions() > exceptions, "an ungranted object must not cross")

	ungranted.queue_free()
	anchor.queue_free()
	s.queue_free()


func test_a_grant_from_inside_the_allow_callback():
	var s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)

	var n = Node.new()
	n.name = "Node"
	s.add_child(n)
	s.set_object_allowed_callback(func(sandbox, obj):
		sandbox.add_allowed_object(obj)
		return true)

	var exceptions = s.get_exceptions()
	s.vmcall("access_a_parent", n)
	assert_eq(s.get_exceptions(), exceptions, "granting from inside the allow callback is safe")

	s.set_object_allowed_callback(Callable())
	s.vmcall("access_a_parent", n)
	assert_eq(s.get_exceptions(), exceptions, "the promoted entry outlives the callback")

	s.queue_free()
