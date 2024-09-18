extends GutTest

func test_instantiation():
	# Create a new sandbox
	var s = Sandbox.new()
	# Set the test program
	s.set_program(Sandbox_TestsTests)

	# Verify some basic stats
	assert_eq(s.get_calls_made(), 0)
	assert_eq(s.get_budget_overruns(), 0)

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
	assert_eq(s.has_function("test_nodepath"), true)
	assert_eq(s.has_function("test_vec2"), true)
	assert_eq(s.has_function("test_vec2i"), true)
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
	# Packed arrays created in the guest
	assert_eq(s.vmcall("test_create_pa_u8"), PackedByteArray([1, 2, 3, 4]))
	assert_eq(s.vmcall("test_create_pa_f32"), PackedFloat32Array([1, 2, 3, 4]))
	assert_eq(s.vmcall("test_create_pa_f64"), PackedFloat64Array([1, 2, 3, 4]))

	# Callables
	var cb : Callable = Callable(callable_function)
	assert_same(s.vmcall("test_ping_pong", cb), cb)

	# Verify that a basic function that returns a String works
	assert_eq(s.has_function("public_function"), true)
	assert_eq(s.vmcall("public_function"), "Hello from the other side")

	# Verify that the sandbox can receive a Dictionary and return an element
	var d : Dictionary
	d["1"] = Dictionary()
	d["1"]["2"] = "3"

	assert_eq(s.has_function("test_sub_dictionary"), true)
	assert_eq(s.vmcall("test_sub_dictionary", d), d["1"])

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
	#assert_same(s.vmcallv("test_ping_pong", Rect2(Vector2(1, 2), Vector2(3, 4))), Rect2(Vector2(1, 2), Vector2(3, 4)))

	# Packed arrays
	var pba_pp : PackedByteArray = [1, 2, 3, 4]
	assert_same(s.vmcallv("test_ping_pong", pba_pp), pba_pp)
	var pfa32_pp : PackedFloat32Array = [1.0, 2.0, 3.0, 4.0]
	assert_same(s.vmcallv("test_ping_pong", pfa32_pp), pfa32_pp)
	var pfa64_pp : PackedFloat64Array = [1.0, 2.0, 3.0, 4.0]
	assert_same(s.vmcallv("test_ping_pong", pfa64_pp), pfa64_pp)


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


func test_timers():
	# Create a new sandbox
	var s = Sandbox.new()
	# Set the test program
	s.set_program(Sandbox_TestsTests)
	assert_eq(s.has_function("test_timers"), true)
	assert_eq(s.has_function("verify_timers"), true)

	# Create a timer and verify that it works
	var timer = s.vmcall("test_timers")
	assert_typeof(timer, TYPE_OBJECT)
	await get_tree().create_timer(0.25).timeout
	assert_eq(s.get_global_exceptions(), 0)
	#assert_true(s.vmcall("verify_timers"), "Timers did not work")


func test_exceptions():
	# Create a new sandbox
	var s = Sandbox.new()
	# Set the test program
	s.set_program(Sandbox_TestsTests)

	# Verify that an exception is thrown
	assert_eq(s.has_function("test_exception"), true)
	assert_eq(s.get_global_exceptions(), 0)
	s.vmcall("test_exception")
	assert_eq(s.get_global_exceptions(), 1)

func test_math():
	# Create a new sandbox
	var s = Sandbox.new()
	# Set the test program
	s.set_program(Sandbox_TestsTests)

	# 64-bit FP math
	assert_eq(s.vmcall("test_math_sin", 0.0), 0.0)
	assert_eq(s.vmcall("test_math_cos", 0.0), 1.0)
	assert_eq(s.vmcall("test_math_tan", 0.0), 0.0)

	assert_eq(s.vmcall("test_math_asin", 0.0), 0.0)
	assert_eq(s.vmcall("test_math_acos", 1.0), 0.0)
	assert_eq(s.vmcall("test_math_atan", 0.0), 0.0)
	assert_eq(s.vmcall("test_math_atan2", 0.0, 1.0), 0.0)

	assert_eq(s.vmcall("test_math_pow", 2.0, 3.0), 8.0)

	# 32-bit FP math
	assert_eq(s.vmcall("test_math_sinf", 0.0), 0.0)

	assert_eq(s.vmcall("test_math_lerp",       0.0, 1.0, 0.5), 0.5)
	assert_eq(s.vmcall("test_math_smoothstep", 0.0, 1.0, 0.5), 0.5)


func callable_function():
	return
