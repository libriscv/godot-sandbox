extends GutTest

var callable_was_called = false

func test_instantiation():
	# Create a new sandbox
	var s = Sandbox.new()
	# Set the test program
	s.set_program(Sandbox_TestsTests)

	# Verify some basic stats
	assert_eq(s.get_calls_made(), 0)
	assert_eq(s.get_exceptions(), 0)
	assert_eq(s.get_timeouts(), 0)

	# Verify execution timeout
	s.set_instructions_max(10)
	s.vmcall("test_infinite_loop")

	# Verify that the execution timeout counter is increased
	assert_eq(s.get_timeouts(), 1)
	assert_eq(s.get_exceptions(), 1)

	# Verify that the sandbox can be reset
	s.free()
	s = Sandbox.new()
	s.set_program(Sandbox_TestsTests)

	# Verify that the counters are reset
	assert_eq(s.get_timeouts(), 0)
	assert_eq(s.get_exceptions(), 0)

	# Verify that too many VM call recursion levels are prevented
	s.vmcall("test_recursive_calls", s)

	assert_eq(s.get_timeouts(), 0)
	assert_eq(s.get_exceptions(), 1)

	# Verify that the sandbox program can be set to another program
	s.set_program(Sandbox_TestsTests)
	assert_eq(s.has_function("test_ping_pong"), true)

	s.queue_free()

	# Create a new sandbox from a buffer
	var buffer : PackedByteArray
	for i in 10:
		var s2 = Sandbox.FromBuffer(buffer)
		assert_false(s2.has_program_loaded(), "Program loaded")
		s2.free()
	for i in 10:
		var s2 = Sandbox.FromProgram(Sandbox_TestsTests)
		s2.free()

func test_binary_translation():
	# Create a new sandbox
	var s = Sandbox.new()
	# Set the test program
	s.set_program(Sandbox_TestsTests)

	var str : String = s.emit_binary_translation()
	assert_false(str.is_empty(), "Binary translation is not empty")
	#print(str)

	s.queue_free()


func test_types():
	# Create a new sandbox
	var s = Sandbox.new()
	# Set the test program
	s.set_program(Sandbox_TestsTests)

	# Verify public functions
	assert_eq(s.has_function("test_ping_pong"), true)
	assert_eq(s.has_function("test_bool"), true)
	assert_eq(s.has_function("test_int"), true)
	assert_eq(s.has_function("test_float"), true)
	assert_eq(s.has_function("test_string"), true)
	assert_eq(s.has_function("test_object"), true)
	assert_eq(s.has_function("test_nodepath"), true)
	assert_eq(s.has_function("test_vec2"), true)
	assert_eq(s.has_function("test_vec2i"), true)
	assert_eq(s.has_function("test_vec3"), true)
	assert_eq(s.has_function("test_vec3i"), true)
	assert_eq(s.has_function("test_vec4"), true)
	assert_eq(s.has_function("test_vec4i"), true)
	assert_eq(s.has_function("test_color"), true)
	assert_eq(s.has_function("test_basis"), true)
	assert_eq(s.has_function("test_transform2d"), true)
	assert_eq(s.has_function("test_transform3d"), true)
	assert_eq(s.has_function("test_array"), true)
	assert_eq(s.has_function("test_dict"), true)

	# Verify all basic types can be ping-ponged
	var nil : Variant
	assert_eq(s.vmcall("test_ping_pong", nil), nil) # Nil
	assert_eq(s.vmcall("test_bool", true), true) # Bool
	assert_eq(s.vmcall("test_bool", false), false) # Bool
	assert_eq(s.vmcall("test_int", 1234), 1234) # Int
	assert_eq(s.vmcall("test_int", -1234), -1234) # Int
	assert_eq(s.vmcall("test_float", 9876.0), 9876.0) # Float
	assert_same(s.vmcall("test_string", "9876.0"), "9876.0") # String
	assert_eq(s.vmcall("test_nodepath", NodePath("Node")), NodePath("Node")) # NodePath
	assert_eq(s.vmcall("test_vec2", Vector2(1, 2)), Vector2(1, 2)) # Vector2
	assert_eq(s.vmcall("test_vec2i", Vector2i(1, 2)), Vector2i(1, 2)) # Vector2i
	assert_eq(s.vmcall("test_vec3", Vector3(1, 2, 3)), Vector3(1, 2, 3)) # Vector3
	assert_eq(s.vmcall("test_vec3i", Vector3i(1, 2, 3)), Vector3i(1, 2, 3)) # Vector3i
	assert_eq(s.vmcall("test_vec4", Vector4(1, 2, 3, 4)), Vector4(1, 2, 3, 4)) # Vector4
	assert_eq(s.vmcall("test_vec4i", Vector4i(1, 2, 3, 4)), Vector4i(1, 2, 3, 4)) # Vector4i
	assert_eq(s.vmcall("test_color", Color(1, 2, 3, 4)), Color(1, 2, 3, 4)) # Color
	assert_eq(s.vmcall("test_ping_pong", Rect2(Vector2(1, 2), Vector2(3, 4))), Rect2(Vector2(1, 2), Vector2(3, 4))) # Rect2
	assert_eq(s.vmcall("test_ping_pong", Rect2i(Vector2i(1, 2), Vector2i(3, 4))), Rect2i(Vector2i(1, 2), Vector2i(3, 4))) # Rect2i
	assert_eq(s.vmcall("test_transform2d", Transform2D(Vector2(1, 2), Vector2(3, 4), Vector2(5, 6))), Transform2D(Vector2(1, 2), Vector2(3, 4), Vector2(5, 6))) # Transform2D
	assert_eq(s.vmcall("test_transform3d", Transform3D(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9), Vector3(10, 11, 12))), Transform3D(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9), Vector3(10, 11, 12))) # Transform3D
	assert_eq(s.vmcall("test_ping_pong", AABB(Vector3(1, 2, 3), Vector3(4, 5, 6))), AABB(Vector3(1, 2, 3), Vector3(4, 5, 6))) # AABB
	assert_eq(s.vmcall("test_ping_pong", Plane(Vector3(1, 2, 3), 4)), Plane(Vector3(1, 2, 3), 4)) # Plane
	assert_eq(s.vmcall("test_ping_pong", Quaternion(1, 2, 3, 4)), Quaternion(1, 2, 3, 4)) # Quat
	assert_eq(s.vmcall("test_basis", Basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9))), Basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9))) # Basis
	assert_eq(s.vmcall("test_ping_pong", RID()), RID()) # RID

	# Nodes
	var n : Node = Node.new()
	n.name = "Node"
	assert_eq(s.vmcall("test_object", n), n)
	n.queue_free()

	var n2d : Node2D = Node2D.new()
	n2d.name = "Node2D"
	assert_eq(s.vmcall("test_object", n2d), n2d)
	n2d.queue_free()

	var n3d : Node3D = Node3D.new()
	n3d.name = "Node3D"
	assert_eq(s.vmcall("test_object", n3d), n3d)
	n3d.queue_free()

	# Array, Dictionary and String as references
	var a_pp : Array
	assert_same(s.vmcall("test_array", a_pp), a_pp)
	var d_pp : Dictionary
	assert_same(s.vmcall("test_dict", d_pp), d_pp)
	var s_pp : String = "12345"
	assert_same(s.vmcall("test_string", s_pp), s_pp)
	assert_eq(s_pp, "12345")
	# Packed arrays
	var pba_pp : PackedByteArray = [1, 2, 3, 4]
	assert_same(s.vmcall("test_pa_u8", pba_pp), pba_pp)
	var pfa32_pp : PackedFloat32Array = [1.0, 2.0, 3.0, 4.0]
	assert_same(s.vmcall("test_pa_f32", pfa32_pp), pfa32_pp)
	var pfa64_pp : PackedFloat64Array = [1.0, 2.0, 3.0, 4.0]
	assert_same(s.vmcall("test_pa_f64", pfa64_pp), pfa64_pp)
	var pia32_pp : PackedInt32Array = [1, 2, 3, 4]
	assert_same(s.vmcall("test_pa_i32", pia32_pp), pia32_pp)
	var pia64_pp : PackedInt64Array = [1, 2, 3, 4]
	assert_same(s.vmcall("test_pa_i64", pia64_pp), pia64_pp)
	var pa_vec2_pp : PackedVector2Array = [Vector2(1, 1), Vector2(2, 2), Vector2(3, 3)]
	assert_same(s.vmcall("test_pa_vec2", pa_vec2_pp), pa_vec2_pp)
	var pa_vec3_pp : PackedVector3Array = [Vector3(1, 1, 1), Vector3(2, 2, 2), Vector3(3, 3, 3)]
	assert_same(s.vmcall("test_pa_vec3", pa_vec3_pp), pa_vec3_pp)
	var pa_color_pp : PackedColorArray = [Color(0, 0, 0, 0), Color(1, 1, 1, 1)]
	assert_same(s.vmcall("test_pa_color", pa_color_pp), pa_color_pp)
	var pa_string_pp : PackedStringArray = ["Hello", "from", "the", "other", "side"]
	assert_same(s.vmcall("test_pa_string", pa_string_pp), pa_string_pp)
	# Packed arrays created in the guest
	assert_eq(s.vmcall("test_create_pa_u8"), PackedByteArray([1, 2, 3, 4]))
	assert_eq(s.vmcall("test_create_pa_f32"), PackedFloat32Array([1, 2, 3, 4]))
	assert_eq(s.vmcall("test_create_pa_f64"), PackedFloat64Array([1, 2, 3, 4]))
	assert_eq(s.vmcall("test_create_pa_i32"), PackedInt32Array([1, 2, 3, 4]))
	assert_eq(s.vmcall("test_create_pa_i64"), PackedInt64Array([1, 2, 3, 4]))
	assert_eq(s.vmcall("test_create_pa_vec2"), PackedVector2Array([Vector2(1, 1), Vector2(2, 2), Vector2(3, 3)]))
	assert_eq(s.vmcall("test_create_pa_vec3"), PackedVector3Array([Vector3(1, 1, 1), Vector3(2, 2, 2), Vector3(3, 3, 3)]))
	assert_eq(s.vmcall("test_create_pa_color"), PackedColorArray([Color(0, 0, 0, 0), Color(1, 1, 1, 1)]))
	assert_eq(s.vmcall("test_create_pa_string"), PackedStringArray(["Hello", "from", "the", "other", "side"]))

	# Callables
	var cb : Callable = Callable(callable_function)
	s.vmcall("test_callable", Callable(callable_callee))
	assert_eq(callable_was_called, true, "Callable was called")
	cb = s.vmcall("test_create_callable")
	# 1 + 2 + stoi("3") + 1 + 2 + stoi("3") = 12
	assert_eq(cb.call(1, 2, "3"), 12, "Callable() invocation evaluates correctly")

	# Verify that a basic function that returns a String works
	assert_eq(s.has_function("public_function"), true)
	assert_eq(s.vmcall("public_function"), "Hello from the other side")

	# Verify that the sandbox can receive a Dictionary and return an element
	var d : Dictionary
	d["1"] = Dictionary()
	d["1"]["2"] = "3"

	assert_eq(s.has_function("test_sub_dictionary"), true)
	assert_eq(s.vmcall("test_sub_dictionary", d), d["1"])

	s.queue_free()

func test_vmcallv():
	# Create a new sandbox
	var s = Sandbox.new()
	# Set the test program
	s.set_program(Sandbox_TestsTests)
	assert_eq(s.has_function("test_ping_pong"), true)

	# Verify that the vmcallv function works
	# vmcallv always uses Variants for both arguments and the return value
	assert_same(s.vmcallv("test_ping_pong", null), null)
	assert_same(s.vmcallv("test_ping_pong", true), true)
	assert_same(s.vmcallv("test_ping_pong", false), false)
	assert_same(s.vmcallv("test_ping_pong", 1234), 1234)
	assert_same(s.vmcallv("test_ping_pong", -1234), -1234)
	assert_same(s.vmcallv("test_ping_pong", 9876.0), 9876.0)
	assert_same(s.vmcallv("test_ping_pong", "9876.0"), "9876.0")
	assert_eq(s.vmcallv("test_ping_pong", NodePath("Node")), NodePath("Node"))
	assert_same(s.vmcallv("test_ping_pong", Vector2(1, 2)), Vector2(1, 2))
	assert_same(s.vmcallv("test_ping_pong", Vector2i(1, 2)), Vector2i(1, 2))
	assert_same(s.vmcallv("test_ping_pong", Vector3(1, 2, 3)), Vector3(1, 2, 3))
	assert_same(s.vmcallv("test_ping_pong", Vector3i(1, 2, 3)), Vector3i(1, 2, 3))
	assert_same(s.vmcallv("test_ping_pong", Vector4(1, 2, 3, 4)), Vector4(1, 2, 3, 4))
	assert_same(s.vmcallv("test_ping_pong", Vector4i(1, 2, 3, 4)), Vector4i(1, 2, 3, 4))
	assert_same(s.vmcallv("test_ping_pong", Color(1, 2, 3, 4)), Color(1, 2, 3, 4))
	assert_same(s.vmcallv("test_ping_pong", Rect2(Vector2(1, 2), Vector2(3, 4))), Rect2(Vector2(1, 2), Vector2(3, 4))) # Rect2
	assert_same(s.vmcallv("test_ping_pong", Rect2i(Vector2i(1, 2), Vector2i(3, 4))), Rect2i(Vector2i(1, 2), Vector2i(3, 4))) # Rect2i
	assert_same(s.vmcallv("test_ping_pong", Transform2D(Vector2(1, 2), Vector2(3, 4), Vector2(5, 6))), Transform2D(Vector2(1, 2), Vector2(3, 4), Vector2(5, 6))) # Transform2D
	assert_same(s.vmcallv("test_ping_pong", AABB(Vector3(1, 2, 3), Vector3(4, 5, 6))), AABB(Vector3(1, 2, 3), Vector3(4, 5, 6))) # AABB
	assert_same(s.vmcallv("test_ping_pong", Plane(Vector3(1, 2, 3), 4)), Plane(Vector3(1, 2, 3), 4)) # Plane
	assert_same(s.vmcallv("test_ping_pong", Quaternion(1, 2, 3, 4)), Quaternion(1, 2, 3, 4)) # Quat
	assert_same(s.vmcallv("test_ping_pong", Basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9))), Basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9))) # Basis
	assert_same(s.vmcallv("test_ping_pong", RID()), RID()) # RID
	var cb : Callable = Callable(callable_function)
	assert_same(s.vmcallv("test_ping_pong", cb), cb, "Returned Callable was same")

	# Nodes
	var n : Node = Node.new()
	n.name = "Node"
	assert_same(s.vmcallv("test_ping_pong", n), n)
	n.queue_free()

	var n2d : Node2D = Node2D.new()
	n2d.name = "Node2D"
	assert_same(s.vmcallv("test_ping_pong", n2d), n2d)
	n2d.queue_free()

	var n3d : Node3D = Node3D.new()
	n3d.name = "Node3D"
	assert_same(s.vmcallv("test_ping_pong", n3d), n3d)
	n3d.queue_free()

	# Packed arrays
	var pba_pp : PackedByteArray = [1, 2, 3, 4]
	assert_same(s.vmcallv("test_ping_pong", pba_pp), pba_pp)
	var pfa32_pp : PackedFloat32Array = [1.0, 2.0, 3.0, 4.0]
	assert_same(s.vmcallv("test_ping_pong", pfa32_pp), pfa32_pp)
	var pfa64_pp : PackedFloat64Array = [1.0, 2.0, 3.0, 4.0]
	assert_same(s.vmcallv("test_ping_pong", pfa64_pp), pfa64_pp)
	var pia32_pp : PackedInt32Array = [1, 2, 3, 4]
	assert_same(s.vmcallv("test_ping_pong", pia32_pp), pia32_pp)
	var pia64_pp : PackedInt64Array = [1, 2, 3, 4]
	assert_same(s.vmcallv("test_ping_pong", pia64_pp), pia64_pp)
	var pa_vec2_pp : PackedVector2Array = [Vector2(0, 0), Vector2(1, 1), Vector2(2, 2)]
	assert_same(s.vmcallv("test_ping_pong", pa_vec2_pp), pa_vec2_pp)
	var pa_vec3_pp : PackedVector3Array = [Vector3(0, 0, 0), Vector3(1, 1, 1), Vector3(2, 2, 2)]
	assert_same(s.vmcallv("test_ping_pong", pa_vec3_pp), pa_vec3_pp)
	var pca_pp : PackedColorArray = [Color(0, 0, 0, 0), Color(1, 1, 1, 1)]
	assert_same(s.vmcallv("test_ping_pong", pca_pp), pca_pp)
	var pa_string_pp : PackedStringArray = ["Hello", "from", "the", "other", "side"]
	assert_same(s.vmcallv("test_ping_pong", pa_string_pp), pa_string_pp)

	s.queue_free()


func test_objects():
	# Create a new sandbox
	var s = Sandbox.new()
	# Set the test program
	s.set_program(Sandbox_TestsTests)
	assert_eq(s.has_function("test_object"), true)

	# Pass a node to the sandbox
	var n = Node.new()
	n.name = "Node"
	assert_same(s.vmcall("test_object", n), n)

	var n2d = Node2D.new()
	n2d.name = "Node2D"
	assert_same(s.vmcall("test_object", n2d), n2d)

	var n3d = Node3D.new()
	n3d.name = "Node3D"
	assert_same(s.vmcall("test_object", n3d), n3d)
	s.queue_free()


func test_timers():
	# Create a new sandbox
	var s = Sandbox.new()
	var current_exceptions = s.get_global_exceptions()
	# Set the test program
	s.set_program(Sandbox_TestsTests)
	assert_eq(s.has_function("test_timers"), true)
	assert_eq(s.has_function("verify_timers"), true)

	# Create a timer and verify that it works
	var timer = s.vmcall("test_timers")
	assert_typeof(timer, TYPE_OBJECT)
	await get_tree().create_timer(0.25).timeout
	assert_eq(s.get_global_exceptions(), current_exceptions)
	#assert_true(s.vmcall("verify_timers"), "Timers did not work")
	s.queue_free()


func test_exceptions():
	# Create a new sandbox
	var s = Sandbox.new()
	var current_exceptions = s.get_global_exceptions()
	# Set the test program
	s.set_program(Sandbox_TestsTests)

	# Verify that an exception is thrown
	assert_eq(s.has_function("test_exception"), true)
	assert_eq(s.get_timeouts(), 0)
	assert_eq(s.get_exceptions(), 0)
	assert_eq(s.get_global_exceptions(), current_exceptions)
	s.vmcall("test_exception")
	assert_eq(s.get_timeouts(), 0)
	assert_eq(s.get_exceptions(), 1)
	assert_eq(s.get_global_exceptions(), current_exceptions + 1)
	s.queue_free()

func test_indirect_methods():
	# Create a new sandbox
	var s = Sandbox.new()
	# Set the test program
	s.set_program(Sandbox_TestsTests)

	# It's possible to call a method on any Variant, which is an easy
	# way to expand the API without having to add custom system functions
	# call_method() takes a Variant, a method name, and an array of arguments
	assert_eq(s.has_function("call_method"), true)

	# Call a method on a String
	var str : String = "Hello from the other side"
	var expected : PackedStringArray = ["Hello", "from", "the", "other", "side"]
	assert_eq(s.vmcallv("call_method", str, "split", [" "]), expected)

	# Calls that don't return a value
	var d : Dictionary
	d["1"] = "1"
	d["2"] = "2"
	d["3"] = "3"
	var d_expected : Dictionary
	# Clear the dictionary using voidcall "clear" on the dictionary
	s.vmcallv("voidcall_method", d, "clear", [])
	assert_eq_deep(d, d_expected)

	s.queue_free()


func test_static_storage():
	var s : Sandbox = Sandbox.new()
	s.set_program(Sandbox_TestsTests)

	# Test that static storage works
	assert_eq(s.has_function("test_static_storage"), true)

	# Call the function that uses static storage
	# It takes a key and a value, stores it in a
	# static Dictionary, and returns the dictionary
	assert_eq_deep(s.vmcallv("test_static_storage", "key", "value"), {"key": "value"})
	assert_eq_deep(s.vmcallv("test_static_storage", "key2", "value2"), {"key": "value", "key2": "value2"})

	# This function tries to create a Dictionary after initialization
	assert_eq(s.has_function("test_failing_static_storage"), true)
	# The first time it will initialize the Dictionary on first-use
	# So, no exception should be thrown
	var exceptions = s.get_exceptions()
	var result = s.vmcallv("test_failing_static_storage", "key", "value")
	assert_eq_deep(result, {"key": "value"})
	assert_eq(s.get_exceptions(), exceptions)

	# The second time it will not initialize the Dictionary
	# as it's stored in a static variable, which was not created
	# during the initialization of the sandbox
	# So, an exception should be thrown
	exceptions = s.get_exceptions()
	result = s.vmcallv("test_failing_static_storage", "key2", "value2")
	assert_eq(s.get_exceptions(), exceptions + 1)
	assert_eq(result, null)

	# This function creates a Dictionary after initialization but makes it permanent
	assert_eq(s.has_function("test_permanent_storage"), true)
	exceptions = s.get_exceptions()

	result = s.vmcallv("test_permanent_storage", "key", "value")
	assert_eq_deep(result, {"key": "value"})
	assert_eq(s.get_exceptions(), exceptions)
	# And again, the second time it will not create a new Dictionary
	# but since the old one was made permanent, it will return the stored value nevertheless
	result = s.vmcallv("test_permanent_storage", "key", "value")
	assert_eq_deep(result, {"key": "value"})
	assert_eq(s.get_exceptions(), exceptions)

	s.queue_free()

func callable_function():
	return

func callable_callee(a1, a2, a3):
	assert(a1 == 1)
	assert(a2 == 2)
	assert(a3 == "3")
	callable_was_called = true
