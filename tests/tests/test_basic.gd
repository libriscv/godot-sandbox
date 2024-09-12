extends GutTest

func test_instantiation():
	# Create a new sandbox
	var s = Sandbox.new()
	# Set the test program
	s.set_program(Sandbox_TestsTests)

	# Verify all basic types can be ping-ponged
	var nil : Variant
	assert_eq(s.vmcall("test_ping_pong", nil), nil) # Nil
	assert_eq(s.vmcall("test_ping_pong", true), true) # Bool
	assert_eq(s.vmcall("test_ping_pong", false), false) # Bool
	assert_eq(s.vmcall("test_ping_pong", 1234), 1234) # Int
	assert_eq(s.vmcall("test_ping_pong", -1234), -1234) # Int
	assert_eq(s.vmcall("test_ping_pong", 9876.0), 9876.0) # Float
	assert_eq(s.vmcall("test_ping_pong", "9876.0"), "9876.0") # String
	assert_eq(s.vmcall("test_ping_pong", Vector2(1, 2)), Vector2(1, 2)) # Vector2
	assert_eq(s.vmcall("test_ping_pong", Vector2i(1, 2)), Vector2i(1, 2)) # Vector2i
	assert_eq(s.vmcall("test_ping_pong", Vector3(1, 2, 3)), Vector3(1, 2, 3)) # Vector3
	assert_eq(s.vmcall("test_ping_pong", Vector3i(1, 2, 3)), Vector3i(1, 2, 3)) # Vector3i
	assert_eq(s.vmcall("test_ping_pong", Vector4(1, 2, 3, 4)), Vector4(1, 2, 3, 4)) # Vector4
	assert_eq(s.vmcall("test_ping_pong", Vector4i(1, 2, 3, 4)), Vector4i(1, 2, 3, 4)) # Vector4i
	assert_eq(s.vmcall("test_ping_pong", Color(1, 2, 3, 4)), Color(1, 2, 3, 4)) # Color
	assert_eq(s.vmcall("test_ping_pong", Rect2(Vector2(1, 2), Vector2(3, 4))), Rect2(Vector2(1, 2), Vector2(3, 4))) # Rect2
	#assert_eq(s.vmcall("test_ping_pong", Transform2D(Vector2(1, 2), Vector2(3, 4), Vector2(5, 6))), Transform2D(Vector2(1, 2), Vector2(3, 4), Vector2(5, 6))) # Transform2D
	#assert_eq(s.vmcall("test_ping_pong", AABB(Vector3(1, 2, 3), Vector3(4, 5, 6))), AABB(Vector3(1, 2, 3), Vector3(4, 5, 6))) # AABB
	#assert_eq(s.vmcall("test_ping_pong", Plane(Vector3(1, 2, 3), 4)), Plane(Vector3(1, 2, 3), 4)) # Plane
	#assert_eq(s.vmcall("test_ping_pong", Quaternion(1, 2, 3, 4)), Quaternion(1, 2, 3, 4)) # Quat
	#assert_eq(s.vmcall("test_ping_pong", Basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9))), Basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9))) # Basis
	#assert_eq(s.vmcall("test_ping_pong", RID()), RID()) # RID
	assert_eq(s.vmcall("test_ping_pong", NodePath("Node")), NodePath("Node")) # NodePath

	# Array, Dictionary and String as references
	var a_pp : Array
	assert_same(s.vmcall("test_ping_pong", a_pp), a_pp)
	var d_pp : Dictionary
	assert_same(s.vmcall("test_ping_pong", d_pp), d_pp)
	var s_pp : String = "12345"
	assert_same(s.vmcall("test_ping_pong", s_pp), s_pp)
	assert_eq(s_pp, "12345")
	# Packed arrays
	var pba_pp : PackedByteArray = [1, 2, 3, 4]
	assert_same(s.vmcall("test_ping_pong", pba_pp), pba_pp)
	var pfa32_pp : PackedFloat32Array = [1.0, 2.0, 3.0, 4.0]
	assert_same(s.vmcall("test_ping_pong", pfa32_pp), pfa32_pp)
	var pfa64_pp : PackedFloat64Array = [1.0, 2.0, 3.0, 4.0]
	assert_same(s.vmcall("test_ping_pong", pfa64_pp), pfa64_pp)

	# Callables
	var cb : Callable = Callable(callable_function)
	assert_same(s.vmcall("test_ping_pong", cb), cb)

	# Verify that a basic function that returns a String works
	assert_eq(s.vmcall("public_function"), "Hello from the other side")

	# Verify that the sandbox can receive a Dictionary and return an element
	var d : Dictionary
	d["1"] = Dictionary()
	d["1"]["2"] = "3"

	assert_eq(s.vmcall("test_dictionary", d), d["1"])

func callable_function():
	return
