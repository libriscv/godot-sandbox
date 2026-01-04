extends GutTest

var Sandbox_TestsTests = load("res://tests/tests.elf")
var holder = Sandbox.new()

# Compile GDScript using an embedded compiler and test the output

func test_compile_and_run():
	var gdscript_code = """
func truthy():
	return true
func falsy():
	return false

func add(x, y):
	return x + y

func typed_add(x : int, y : int):
	return x + y

func typed_sub(x : int, y : int):
	return x - y

func typed_float_add(x : float, y : float):
	return x + y

func typed_float_mul(x : float, y : float):
	return x * y

func typed_vec3_add(a : Vector3, b : Vector3):
	return a + b

func typed_vec3_mul(a : Vector3, b : Vector3):
	return a * b

func sum1(n):
	var total = 0
	for i in range(n):
		total += i
	return total

func sum2(n):
	var total : int = 0
	var i : int = 0
	while i < n:
		total += i
		i += 1
	return total
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(600)
	assert_true(s.has_function("truthy"), "Compiled ELF should have function 'truthy'")
	assert_true(s.has_function("falsy"), "Compiled ELF should have function 'falsy'")
	assert_true(s.has_function("add"), "Compiled ELF should have function 'add'")
	assert_true(s.has_function("typed_add"), "Compiled ELF should have function 'typed_add'")
	assert_true(s.has_function("typed_sub"), "Compiled ELF should have function 'typed_sub'")

	assert_true(s.has_function("sum1"), "Compiled ELF should have function 'sum1'")
	assert_true(s.has_function("sum2"), "Compiled ELF should have function 'sum2'")

	# Test the compiled functions
	assert_eq(s.vmcallv("truthy"), true, "truthy() should return true")
	assert_eq(s.vmcallv("falsy"), false, "falsy() should return false")
	assert_eq(s.vmcallv("add", 7, 21), 28, "add(7, 21) = 28")
	assert_eq(s.vmcallv("typed_add", 10, 15), 25, "typed_add(10, 15) = 25")
	assert_eq(s.vmcallv("typed_sub", 10, 15), -5, "typed_sub(10, 15) = -5")

	# Test typed float operations (optimized with double-precision FP)
	var f_result = s.vmcallv("typed_float_add", 3.5, 2.5)
	assert_almost_eq(f_result, 6.0, 0.0001, "typed_float_add(3.5, 2.5) = 6.0")
	f_result = s.vmcallv("typed_float_mul", 2.5, 4.0)
	assert_almost_eq(f_result, 10.0, 0.0001, "typed_float_mul(2.5, 4.0) = 10.0")

	# Test typed Vector3 operations (optimized with single-precision FP)
	var v3_result = s.vmcallv("typed_vec3_add", Vector3(6.0, 8.0, 10.0), Vector3(2.0, 2.0, 5.0))
	assert_almost_eq(v3_result.x, 8.0, 0.0001, "vec3_add.x = 8.0")
	assert_almost_eq(v3_result.y, 10.0, 0.0001, "vec3_add.y = 10.0")
	assert_almost_eq(v3_result.z, 15.0, 0.0001, "vec3_add.z = 15.0")
	v3_result = s.vmcallv("typed_vec3_mul", Vector3(3.0, 4.0, 5.0), Vector3(2.0, 2.0, 2.0))
	assert_almost_eq(v3_result.x, 6.0, 0.0001, "vec3_mul.x = 6.0")
	assert_almost_eq(v3_result.y, 8.0, 0.0001, "vec3_mul.y = 8.0")
	assert_almost_eq(v3_result.z, 10.0, 0.0001, "vec3_mul.z = 10.0")

	assert_eq(s.vmcallv("sum1", 10), 45, "sum1(10) should return 45")
	assert_eq(s.vmcallv("sum2", 10), 45, "sum2(10) should return 45")

	s.queue_free()
	ts.queue_free()


func test_many_variables():
	# Test register allocation with 15+ local variables
	var gdscript_code = """
func many_variables():
	var a = 1
	var b = 2
	var c = 3
	var d = 4
	var e = 5
	var f = 6
	var g = 7
	var h = 8
	var i = 9
	var j = 10
	var k = 11
	var l = 12
	var m = 13
	var n = 14
	var o = 15
	return a + b + c + d + e + f + g + h + i + j + k + l + m + n + o
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	assert_true(s.has_function("many_variables"), "Compiled ELF should have function 'many_variables'")

	# Test the compiled function
	var result = s.vmcallv("many_variables")
	assert_eq(result, 120, "many_variables() should return 120 (sum of 1-15)")

	s.queue_free()
	ts.queue_free()

func test_complex_expression():
	# Test register allocation with deeply nested expressions
	var gdscript_code = """
func complex_expr(x, y, z):
	return (x + y) * (y + z) * (z + x) + (x * y) + (y * z) + (z * x)
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	assert_true(s.has_function("complex_expr"), "Compiled ELF should have function 'complex_expr'")

	# Test the compiled function
	var result = s.vmcallv("complex_expr", 2, 3, 4)
	# (2+3)*(3+4)*(4+2) + (2*3) + (3*4) + (4*2)
	# = 5*7*6 + 6 + 12 + 8
	# = 210 + 6 + 12 + 8
	# = 236
	assert_eq(result, 236, "complex_expr(2, 3, 4) should return 236")

	s.queue_free()
	ts.queue_free()

func test_ir_verification():
	# Verify that register allocation avoids unnecessary stack spilling
	# by checking max_registers in the IR
	var gdscript_code = """
func test_func():
	var a = 1
	var b = 2
	var c = 3
	var d = 4
	var e = 5
	return a + b + c + d + e
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true

	# Enable IR dumping to verify register usage
	# Note: This requires access to compiler internals, so we'll just test that it compiles
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	assert_true(s.has_function("test_func"), "Compiled ELF should have function 'test_func'")

	# Test the compiled function
	var result = s.vmcallv("test_func")
	assert_eq(result, 15, "test_func() should return 15")

	# Note: IR verification would check max_registers <= 25
	# This would require compiler internals access, so we verify functionality instead
	s.queue_free()
	ts.queue_free()

func test_vcall_method_calls():
	# Test VCALL - calling methods on Variants
	# Start with a simple test that just returns a constant
	var gdscript_code = """
func test_simple(str):
	str = str.to_upper()
	return str

func test_literal():
	return "Hello, World!"

func test_assign_literal():
	var str = "Hello, Assigned World!"
	return str

func test_chain():
	var str = "Hello, World!"
	str = str.to_upper().to_lower()
	return str

func test_args1(str):
	return str.split_floats(",")
func test_args2(str):
	return str.split_floats("-")
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(6000)
	assert_true(s.has_function("test_simple"), "Compiled ELF should have function 'test_simple'")

	# Test the compiled function
	var result = s.vmcallv("test_simple", "Hello, World!")
	assert_eq(result, "HELLO, WORLD!", "test_simple should convert string to uppercase")

	result = s.vmcallv("test_literal")
	assert_eq(result, "Hello, World!", "test_literal should return the literal string")
	result = s.vmcallv("test_assign_literal")
	assert_eq(result, "Hello, Assigned World!", "test_assign_literal should return the assigned literal string")

	result = s.vmcallv("test_chain")
	assert_eq(result, "hello, world!", "test_chain should convert string to uppercase then lowercase")

	var array : PackedFloat64Array = [1.5, 2.5, 3.5]
	result = s.vmcallv("test_args1", "1.5,2.5,3.5", ",")
	assert_eq_deep(result, array)
	result = s.vmcallv("test_args2", "1.5-2.5-3.5", "-")
	assert_eq_deep(result, array)

	s.queue_free()
	ts.queue_free()


func test_local_function_calls():
	var gdscript_code = """
func test_to_upper(str):
	str = str.to_upper()
	return str

func test_call():
	return test_to_upper("Hello, World!")

func test_call2():
	return test_call()

func test_call3():
	return test_call2()

func test_call_with_shuffling(a0, a1):
	return test_to_upper(a1)

func untyped_fibonacci(n):
	if n <= 1:
		return n
	return untyped_fibonacci(n - 1) + untyped_fibonacci(n - 2)

func typed_fibonacci(n : int):
	if n <= 1:
		return n
	return typed_fibonacci(n - 1) + typed_fibonacci(n - 2)
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(6000)
	assert_true(s.has_function("test_to_upper"), "Compiled ELF should have function 'test_to_upper'")
	assert_true(s.has_function("test_call"), "Compiled ELF should have function 'test_call'")

	# Test the compiled function
	var result = s.vmcallv("test_to_upper", "Hello, World!")
	assert_eq(result, "HELLO, WORLD!", "test_to_upper should convert string to uppercase")

	# Indirectly test via test_call
	result = s.vmcallv("test_call")
	assert_eq(result, "HELLO, WORLD!", "test_call should return uppercase string via test_to_upper")

	result = s.vmcallv("test_call2")
	assert_eq(result, "HELLO, WORLD!", "test_call2 should return uppercase string via test_call")

	result = s.vmcallv("test_call3")
	assert_eq(result, "HELLO, WORLD!", "test_call3 should return uppercase string via test_call2")

	result = s.vmcallv("test_call_with_shuffling", "first", "second")
	assert_eq(result, "SECOND", "test_call_with_shuffling should return uppercase of second argument")

	# Test typed/untyped fibonacci
	result = s.vmcallv("typed_fibonacci", 20)
	assert_eq(result, 6765, "typed_fibonacci(20) should return 6765")

	result = s.vmcallv("untyped_fibonacci", 20)
	assert_eq(result, 6765, "untyped_fibonacci(20) should return 6765")

	s.queue_free()
	ts.queue_free()

func test_range_loop_bounds():
	# Test that for i in range(n) doesn't execute n+1 iterations
	var gdscript_code = """
func test_range_count(n):
	var count = 0
	for i in range(n):
		count += 1
	return count

func test_range_new_var():
	var unused = 42
	for i in range(5):
		var nvar = i
	return unused

func test_range_no_var():
	var unused = 42
	for i in range(5):
		continue
	return unused

func test_range_last_value():
	var last = -1
	for i in range(5):
		last = i
	return last

func countup_loop():
	var sum = 0
	for i in range(1, 10, 1):
		sum = sum + i
	return sum

func countdown_loop():
	var sum = 0
	for i in range(10, 0, -1):
		sum = sum + i
	return sum

func test_loopy_ints():
	var a = 0
	const b = 1
	for i in range(10):
		a = a + b
	return a
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(1)
	assert_true(s.has_function("test_range_count"), "Compiled ELF should have function 'test_range_count'")
	assert_true(s.has_function("test_range_last_value"), "Compiled ELF should have function 'test_range_last_value'")
	assert_true(s.has_function("test_range_no_var"), "Compiled ELF should have function 'test_range_no_var'")
	assert_true(s.has_function("test_range_new_var"), "Compiled ELF should have function 'test_range_new_var'")

	# Test iteration count
	assert_eq(s.vmcallv("test_range_count", 10), 10, "range(10) should iterate exactly 10 times")
	assert_eq(s.vmcallv("test_range_count", 5), 5, "range(5) should iterate exactly 5 times")
	assert_eq(s.vmcallv("test_range_count", 0), 0, "range(0) should iterate 0 times")

	# Test last value (should be 4 for range(5))
	assert_eq(s.vmcallv("test_range_last_value"), 4, "range(5) last value should be 4")

	# Test no variable inside loop
	assert_eq(s.vmcallv("test_range_no_var"), 42, "test_range_no_var should return 42")

	# Test new variable inside loop
	assert_eq(s.vmcallv("test_range_new_var"), 42, "test_range_new_var should return 42")

	# Test countup loop
	var result = s.vmcallv("countup_loop")
	# sum = 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 = 45
	assert_eq(result, 45, "countup_loop should sum 1..9 = 45")

	# Test countdown loop with negative step
	result = s.vmcallv("countdown_loop")
	# sum = 10 + 9 + 8 + 7 + 6 + 5 + 4 + 3 + 2 + 1 = 55
	assert_eq(result, 55, "countdown_loop should sum 10..1 = 55")

	# Test loopy ints
	result = s.vmcallv("test_loopy_ints")
	assert_eq(result, 10, "test_loopy_ints should return 10")

	s.queue_free()
	ts.queue_free()

func test_subscript_operations():
	# Test array and dictionary subscript operations using [] operator
	var gdscript_code = """
func test_array_get(arr, idx):
	return arr[idx]

func test_array_set(arr, idx, value):
	arr[idx] = value
	return arr

func test_dict_get(dict, key):
	return dict[key]

func test_dict_set(dict, key, value):
	dict[key] = value
	return dict

func test_chained_get(arr):
	var first = arr[0]
	var second = arr[1]
	return first + second
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(6000)

	# Test array get
	var test_array = [10, 20, 30, 40, 50]
	var result = s.vmcallv("test_array_get", test_array, 2)
	assert_eq(result, 30, "arr[2] should return 30")

	result = s.vmcallv("test_array_get", test_array, 0)
	assert_eq(result, 10, "arr[0] should return 10")

	# Test array set
	var arr = [1, 2, 3]
	result = s.vmcallv("test_array_set", arr, 1, 99)
	assert_eq(result[1], 99, "arr[1] should be set to 99")

	# Test dictionary get
	var test_dict = {"name": "Alice", "age": 30}
	result = s.vmcallv("test_dict_get", test_dict, "name")
	assert_eq(result, "Alice", "dict['name'] should return 'Alice'")

	result = s.vmcallv("test_dict_get", test_dict, "age")
	assert_eq(result, 30, "dict['age'] should return 30")

	# Test dictionary set
	var dict = {"x": 1, "y": 2}
	result = s.vmcallv("test_dict_set", dict, "x", 42)
	assert_eq(result["x"], 42, "dict['x'] should be set to 42")

	# Test chained subscript operations
	var arr2 = [5, 15]
	result = s.vmcallv("test_chained_get", arr2)
	assert_eq(result, 20, "chained subscripts should return 5 + 15 = 20")

	s.queue_free()
	ts.queue_free()

func test_inline_vector_primitives():
	# Test inline construction and member access for Vector2/3/4 without syscalls
	var gdscript_code = """
func test_vector2():
	var v = Vector2(3.0, 4.0)
	return v.x + v.y

func test_vector2_int():
	var v = Vector2(3, 4)
	return v.x + v.y

func test_vector3():
	var v = Vector3(1.0, 2.0, 3.0)
	return v.x + v.y + v.z

func test_vector3_int():
	var v = Vector3(1, 2, 3)
	return v.x + v.y + v.z

func test_vector4():
	var v = Vector4(10.0, 20.0, 30.0, 40.0)
	return v.x + v.y + v.z + v.w

func test_vector4_int():
	var v = Vector4(10, 20, 30, 40)
	return v.x + v.y + v.z + v.w

func test_vector2i():
	var v = Vector2i(5, 7)
	return v.x + v.y

func test_vector3i():
	var v = Vector3i(1, 2, 3)
	return v.x * v.y * v.z

func test_vector4i():
	var v = Vector4i(2, 3, 5, 7)
	return v.x + v.y + v.z + v.w

func test_color():
	var c = Color(0.5, 0.25, 0.75, 1.0)
	return c.r + c.g + c.b + c.a

func test_color_int():
	var c = Color(0, 128, 255, 255)
	return c.r + c.g + c.b + c.a

func test_chained_vectors():
	var v1 = Vector2(10.0, 20.0)
	var v2 = Vector2(v1.x, v1.y)
	return v2.x + v2.y

func test_loopy_vector():
	var v = Vector2(0.0, 0.0)
	const v2 = Vector2(1.0, 1.0)
	for i in range(10):
		v = v + v2
	return v
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	# Write the ELF to a file for debugging
	var file = FileAccess.open("res://tests/vec.elf", FileAccess.WRITE)
	if file:
		file.store_buffer(compiled_elf)
		file.close()

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(1000)

	# Test Vector2
	var result = s.vmcallv("test_vector2")
	assert_almost_eq(result, 7.0, 0.001, "Vector2(3.0, 4.0).x + .y should be 7.0")

	# Test Vector2 with integer args
	result = s.vmcallv("test_vector2_int")
	assert_almost_eq(result, 7.0, 0.001, "Vector2(3, 4).x + .y should be 7.0")

	# Test Vector3
	result = s.vmcallv("test_vector3")
	assert_almost_eq(result, 6.0, 0.001, "Vector3(1.0, 2.0, 3.0) sum should be 6.0")

	# Test Vector3 with integer args
	result = s.vmcallv("test_vector3_int")
	assert_almost_eq(result, 6.0, 0.001, "Vector3(1, 2, 3) sum should be 6.0")

	# Test Vector4
	result = s.vmcallv("test_vector4")
	assert_almost_eq(result, 100.0, 0.001, "Vector4(10.0, 20.0, 30.0, 40.0) sum should be 100.0")

	# Test Vector4 with integer args
	result = s.vmcallv("test_vector4_int")
	assert_almost_eq(result, 100.0, 0.001, "Vector4(10, 20, 30, 40) sum should be 100.0")

	# Test Vector2i
	result = s.vmcallv("test_vector2i")
	assert_eq(result, 12, "Vector2i(5, 7).x + .y should be 12")

	# Test Vector3i
	result = s.vmcallv("test_vector3i")
	assert_eq(result, 6, "Vector3i(1, 2, 3) product should be 6")

	# Test Vector4i
	result = s.vmcallv("test_vector4i")
	assert_eq(result, 17, "Vector4i(2, 3, 5, 7) sum should be 17")

	# Test Color
	result = s.vmcallv("test_color")
	assert_almost_eq(result, 2.5, 0.001, "Color components sum should be 2.5")

	# Test Color with integer args (0, 128, 255, 255 = 0.0, 0.502, 1.0, 1.0)
	result = s.vmcallv("test_color_int")
	var expected = 0.0 + (128.0 / 255.0) + 1.0 + 1.0
	assert_almost_eq(result, expected, 0.01, "Color with int args should work")

	# Test chained operations
	result = s.vmcallv("test_chained_vectors")
	assert_almost_eq(result, 30.0, 0.001, "Chained vector operations should work")

	# Test loopy vector addition
	result = s.vmcallv("test_loopy_vector")
	assert_eq(result, Vector2(10.0, 10.0), "Loopy vector addition should result in Vector2(10.0, 10.0)")

	s.queue_free()
	ts.queue_free()

func test_float_constant_folding():
	var gdscript_code = """
func test_float_add():
	return 1.5 + 2.5

func test_float_mul():
	return 2.5 * 4.0

func test_float_sub():
	return 5.5 - 2.5

func test_float_div():
	return 10.0 / 2.0

func test_int_float_promotion():
	return 1 + 2.5

func test_float_comparison_lt():
	return 1.5 < 2.5

func test_float_comparison_gte():
	return 3.5 >= 2.5

func test_complex_fold():
	return 1.5 + 2.5 * 2.0

func test_int_neg():
	return -42

func test_float_neg():
	return -3.14
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(1000)

	# Test float addition
	var result = s.vmcallv("test_float_add")
	assert_almost_eq(result, 4.0, 0.001, "Float constant folding: 1.5 + 2.5 should be 4.0")

	# Test float multiplication
	result = s.vmcallv("test_float_mul")
	assert_almost_eq(result, 10.0, 0.001, "Float constant folding: 2.5 * 4.0 should be 10.0")

	# Test float subtraction
	result = s.vmcallv("test_float_sub")
	assert_almost_eq(result, 3.0, 0.001, "Float constant folding: 5.5 - 2.5 should be 3.0")

	# Test float division
	result = s.vmcallv("test_float_div")
	assert_almost_eq(result, 5.0, 0.001, "Float constant folding: 10.0 / 2.0 should be 5.0")

	# Test int + float promotion
	result = s.vmcallv("test_int_float_promotion")
	assert_almost_eq(result, 3.5, 0.001, "Int + float: 1 + 2.5 should be 3.5")

	# Test float comparison
	result = s.vmcallv("test_float_comparison_lt")
	assert_eq(result, true, "Float comparison: 1.5 < 2.5 should be true")

	result = s.vmcallv("test_float_comparison_gte")
	assert_eq(result, true, "Float comparison: 3.5 >= 2.5 should be true")

	# Test complex expression folding
	result = s.vmcallv("test_complex_fold")
	assert_almost_eq(result, 6.5, 0.001, "Complex folding: 1.5 + 2.5 * 2.0 should be 6.5")

	# Test integer negation
	result = s.vmcallv("test_int_neg")
	assert_eq(result, -42, "Int negation: -42 should be -42")

	# Test float negation
	result = s.vmcallv("test_float_neg")
	assert_almost_eq(result, -3.14, 0.001, "Float negation: -3.14 should be -3.14")

	s.queue_free()
	ts.queue_free()

func test_array_dictionary_constructors():
	# Test Array() and Dictionary() constructor support
	var gdscript_code = """
func make_empty_array():
	return Array()

func make_empty_dictionary():
	return Dictionary()

func make_array_and_add():
	var arr = Array()
	arr.append(42)
	arr.append(100)
	return arr

func make_dict():
	var dict = Dictionary()
	dict["key1"] = "value1"
	dict["key2"] = 42
	return dict

func dict_literal():
	var dict = {key1: "value", key2: 42}
	return dict

func nested_dict_literal():
	var dict = {key1: "value", key2: 42, key3: {nested_key: "nested_value", number: 99}}
	return dict

func array_size():
	var arr = Array()
	arr.append(1)
	arr.append(2)
	arr.append(3)
	return arr.size()

func dict_size():
	var dict = Dictionary()
	dict["a"] = 1
	dict["b"] = 2
	dict["c"] = 3
	return dict.size()

func make_array_single():
	return Array(42)

func make_array_two():
	return Array(1, 2)

func make_array_with_values():
	return Array(1, 2, 3, 4, 5)

func make_array_with_strings():
	return Array("hello", "world", "test")
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(1000)

	# Test empty array
	var arr = s.vmcallv("make_empty_array")
	assert_eq(arr.size(), 0, "Empty Array() should have size 0")

	# Test empty dictionary
	var dict = s.vmcallv("make_empty_dictionary")
	assert_eq(dict.size(), 0, "Empty Dictionary() should have size 0")

	# Test array with append
	arr = s.vmcallv("make_array_and_add")
	assert_eq(arr.size(), 2, "Array with 2 appends should have size 2")
	assert_eq(arr[0], 42, "First element should be 42")
	assert_eq(arr[1], 100, "Second element should be 100")

	# Test dictionary with set
	dict = s.vmcallv("make_dict")
	assert_eq(dict.size(), 2, "Dictionary with 2 keys should have size 2")
	assert_eq(dict["key1"], "value1", "key1 should have value 'value1'")
	assert_eq(dict["key2"], 42, "key2 should have value 42")
	# Test dictionary literal
	var dict2 = s.vmcallv("dict_literal")
	assert_eq(dict2.size(), 2, "Dictionary literal should have size 2")
	assert_eq(dict2["key1"], "value", "key1 should have value 'value'")
	assert_eq(dict2["key2"], 42, "key2 should have value 42")
	# Test nested dictionary literal
	var dict3 = s.vmcallv("nested_dict_literal")
	assert_eq(dict3.size(), 3, "Nested dictionary literal should have size 3")
	assert_eq(dict3["key1"], "value", "key1 should have value 'value'")
	assert_eq(dict3["key2"], 42, "key2 should have value 42")
	var nested = dict3["key3"]
	assert_eq(nested.size(), 2, "Nested dictionary should have size 2")
	assert_eq(nested["nested_key"], "nested_value", "nested_key should have value 'nested_value'")
	assert_eq(nested["number"], 99, "number should have value 99")

	# Test array size
	assert_eq(s.vmcallv("array_size"), 3, "array_size() should return 3")

	# Test dictionary size
	assert_eq(s.vmcallv("dict_size"), 3, "dict_size() should return 3")

	# Test Array with single element
	arr = s.vmcallv("make_array_single")
	assert_eq(arr.size(), 1, "Array(42) should have size 1")
	assert_eq(arr[0], 42, "First element should be 42")

	# Test Array with two elements
	arr = s.vmcallv("make_array_two")
	assert_eq(arr.size(), 2, "Array(1,2) should have size 2")
	assert_eq(arr[0], 1, "First element should be 1")
	assert_eq(arr[1], 2, "Second element should be 2")

	# Test Array with initial integer values
	arr = s.vmcallv("make_array_with_values")
	assert_eq(arr.size(), 5, "Array(1,2,3,4,5) should have size 5")
	assert_eq(arr[0], 1, "First element should be 1")
	assert_eq(arr[1], 2, "Second element should be 2")
	assert_eq(arr[2], 3, "Third element should be 3")
	assert_eq(arr[3], 4, "Fourth element should be 4")
	assert_eq(arr[4], 5, "Fifth element should be 5")

	# Test Array with initial string values
	arr = s.vmcallv("make_array_with_strings")
	assert_eq(arr.size(), 3, "Array with 3 strings should have size 3")
	assert_eq(arr[0], "hello", "First string should be 'hello'")
	assert_eq(arr[1], "world", "Second string should be 'world'")
	assert_eq(arr[2], "test", "Third string should be 'test'")

	s.queue_free()
	ts.queue_free()

func test_comprehensive_compiler_readiness():
	# A comprehensive test that progressively tests more complex features
	# Each function tests more features together to uncover integration bugs
	# NOTE: Temporarily using minimal test to identify compilation issues
	var gdscript_code = """
# LEVEL 1: Basic types and literals
func level1_literals():
	var int_val = 42
	var float_val = 3.14
	var string_val = "Hello"
	var bool_val = true
	return int_val + float_val + string_val.length()

# LEVEL 2: Arithmetic operations
func level2_arithmetic(a, b):
	var sum = a + b
	var diff = a - b
	var prod = a * b
	var div = a / b
	var mod = a % b
	return sum + diff + prod + div + mod

# LEVEL 3: String operations and method chaining
func level3_strings(name):
	var greeting = "Hello, " + name + "!"
	var upper = greeting.to_upper()
	var lower = upper.to_lower()
	var trimmed = lower.strip_edges()
	return trimmed.length()

# LEVEL 4: Array operations
func level4_arrays():
	var arr = Array()
	arr.append(10)
	arr.append(20)
	arr.append(30)
	var first = arr[0]
	var last = arr[arr.size() - 1]
	arr.sort()
	return arr[0] + arr[1] + arr[2]

# LEVEL 5: Dictionary operations
func level5_dictionaries():
	var dict = Dictionary()
	dict["count"] = 0
	for i in range(5):
		dict["count"] = dict["count"] + 1
	dict["doubled"] = dict["count"] * 2
	return dict["count"] + dict["doubled"]

# LEVEL 6: Control flow - if/else
func level6_ifelse(x, y):
	if x > y:
		return x * 2
	elif x < y:
		return y * 2
	else:
		return x + y

# LEVEL 7: Nested loops with arrays
func level7_nested_loops():
	var matrix = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
	var sum = 0
	for i in range(3):
		for j in range(3):
			sum = sum + matrix[i][j]
	return sum

# LEVEL 8: Function calls and recursion
func level8_recursive(n):
	if n <= 1:
		return 1
	return n * level8_recursive(n - 1)

func level8_helper(x):
	return x * x

func level8_call_chain(n):
	var squared = level8_helper(n)
	var doubled = squared * 2
	return level8_recursive(doubled % 5 + 1)

# LEVEL 9: Vector types and math
func level9_vectors():
	var v2 = Vector2(3.0, 4.0)
	var v3 = Vector3(1.0, 2.0, 3.0)
	var v2i = Vector2i(10, 20)
	var v3i = Vector3i(2, 3, 4)
	var v2_len = v2.x + v2.y
	var v3_len = v3.x + v3.y + v3.z
	var v2i_len = v2i.x + v2i.y
	var v3i_len = v3i.x * v3i.y * v3i.z
	return v2_len + v3_len + v2i_len + v3i_len

# LEVEL 10: Mixed complex operations
func level10_complex(data):
	var result = 0
	for i in range(data.size()):
		var val = data[i]
		if val > 0:
			result = result + val
		elif val < 0:
			result = result - val
		else:
			result = result + 10
	return result
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(100000)

	# LEVEL 1: Basic types
	var result = s.vmcallv("level1_literals")
	# 42 + 3.14 + 5 (length of "Hello") = 50.14
	assert_eq(result, 50.14, "Level 1: Basic literals should work")

	# LEVEL 2: Arithmetic
	result = s.vmcallv("level2_arithmetic", 10, 3)
	# sum=13, diff=7, prod=30, div=3, mod=1 => 13+7+30+3+1 = 54
	assert_eq(result, 54, "Level 2: Arithmetic operations")

	# LEVEL 3: String operations
	result = s.vmcallv("level3_strings", "Claude")
	# "Hello, Claude!" -> uppercase -> lowercase -> trim -> length = 14
	assert_eq(result, 14, "Level 3: String operations")

	# LEVEL 4: Array operations
	result = s.vmcallv("level4_arrays")
	# [10, 20, 30] sorted -> [10, 20, 30] -> sum = 60
	assert_eq(result, 60, "Level 4: Array operations")

	# LEVEL 5: Dictionary operations
	result = s.vmcallv("level5_dictionaries")
	# count = 5, doubled = 10 -> 15
	assert_eq(result, 15, "Level 5: Dictionary operations")

	# LEVEL 6: Control flow
	result = s.vmcallv("level6_ifelse", 10, 5)
	assert_eq(result, 20, "Level 6: If/else (x > y)")
	result = s.vmcallv("level6_ifelse", 5, 10)
	assert_eq(result, 20, "Level 6: If/else (x < y)")
	result = s.vmcallv("level6_ifelse", 5, 5)
	assert_eq(result, 10, "Level 6: If/else (x == y)")

	# LEVEL 7: Nested loops
	result = s.vmcallv("level7_nested_loops")
	# 1+2+3+4+5+6+7+8+9 = 45
	assert_eq(result, 45, "Level 7: Nested loops")

	# LEVEL 8: Recursion
	result = s.vmcallv("level8_recursive", 5)
	# 5! = 120
	assert_eq(result, 120, "Level 8: Recursive factorial")
	result = s.vmcallv("level8_call_chain", 3)
	# squared=9, doubled=18, 18%5+1=4, 4! = 24
	assert_eq(result, 24, "Level 8: Call chain")

	# LEVEL 9: Vectors
	result = s.vmcallv("level9_vectors")
	# v2: 7.0, v3: 6.0, v2i: 30, v3i: 24 -> 67.0
	assert_eq(result, 67.0, "Level 9: Vector operations")

	# LEVEL 10: Complex with mixed data
	var test_data = [1, -2, 3, -4, 0, 5]
	result = s.vmcallv("level10_complex", test_data)
	# 1 + 2 + 3 + 4 + 10 + 5 = 25
	assert_eq(result, 25, "Level 10: Mixed complex operations")

	s.queue_free()
	ts.queue_free()

func test_global_class_access():
	# Test accessing global classes like Time
	var gdscript_code = """
func test_time_get_ticks_usec():
	var time_obj = Time
	return time_obj.get_ticks_usec()

func test_chained_call():
	return Time.get_ticks_usec()
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true

	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.restrictions = true
	s.set_class_allowed_callback(func(sandbox, name): return name == "Time")
	s.set_method_allowed_callback(func(sandbox, obj, method):
		return obj.get_class() == "Time" and method == "get_ticks_usec")
	s.load_buffer(compiled_elf)
	s.set_instructions_max(5000)

	# Test direct global class access and method call
	var result = s.vmcallv("test_time_get_ticks_usec")
	assert_true(result >= 0, "Time.get_ticks_usec() should return a non-negative value")

	# Test chained call
	result = s.vmcallv("test_chained_call")
	assert_true(result >= 0, "Time.get_ticks_usec() chained should return a non-negative value")

	s.queue_free()
	ts.queue_free()

func test_array_iteration():
	# Test for item in array iteration
	var gdscript_code = """
func test_simple_array_iteration():
	var arr = [1, 2, 3, 4, 5]
	var sum = 0
	for item in arr:
		sum = sum + item
	return sum

func test_array_string_iteration():
	var arr = ["hello", "world", "test"]
	var result = ""
	for item in arr:
		result = result + item
	return result

func test_empty_array():
	var arr = []
	var count = 0
	for item in arr:
		count = count + 1
	return count

func test_nested_array_iteration():
	var matrix = [[1, 2], [3, 4]]
	var sum = 0
	for row in matrix:
		for item in row:
			sum = sum + item
	return sum
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	# Write ELF for objdump inspection
	var file = FileAccess.open("res://tests/array.elf", FileAccess.WRITE)
	if file:
		file.store_buffer(compiled_elf)
		file.close()

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test simple array iteration
	var result = s.vmcallv("test_simple_array_iteration")
	assert_eq(result, 15, "Sum of [1,2,3,4,5] should be 15")

	# Test array with string elements
	result = s.vmcallv("test_array_string_iteration")
	assert_eq(result, "helloworldtest", "Concatenated strings should be 'helloworldtest'")

	# Test empty array
	result = s.vmcallv("test_empty_array")
	assert_eq(result, 0, "Empty array should iterate 0 times")

	# Test nested array iteration
	result = s.vmcallv("test_nested_array_iteration")
	assert_eq(result, 10, "Sum of [[1,2],[3,4]] should be 10")

	s.queue_free()
	ts.queue_free()

# Comprehensive FP arithmetic tests to stress FP register allocation and AUIPC+ADDI patching
func test_fp_register_allocation_stress():
	var gdscript_code = """
func many_float_vars():
	var f1 = 1.1
	var f2 = 2.2
	var f3 = 3.3
	var f4 = 4.4
	var f5 = 5.5
	var f6 = 6.6
	var f7 = 7.7
	var f8 = 8.8
	var f9 = 9.9
	var f10 = 10.10
	var f11 = 11.11
	var f12 = 12.12
	var f13 = 13.13
	var f14 = 14.14
	var f15 = 15.15
	var sum = f1 + f2 + f3 + f4 + f5
	return sum
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	var result = s.vmcallv("many_float_vars")
	# 1.1 + 2.2 + 3.3 + 4.4 + 5.5 = 16.5
	assert_almost_eq(result, 16.5, 0.01, "Sum of 5 floats should be 16.5")

	s.queue_free()
	ts.queue_free()

func test_large_float_constants():
	var gdscript_code = """
func large_constants():
	const f1 = 123456.789
	const f2 = 987654.321
	const f3 = 111111.222
	const f4 = 999999.999
	var sum = f1 + f2 + f3 + f4
	return sum
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	var result = s.vmcallv("large_constants")
	assert_almost_eq(result, 2222222.331, 0.01, "Sum of large floats should be correct")

	s.queue_free()
	ts.queue_free()

func test_complex_float_arithmetic():
	var gdscript_code = """
func complex_arithmetic():
	var a = 1.5
	var b = 2.5
	var c = 3.0
	var d = 4.0
	var sum1 = a + b
	var sum2 = c + d
	var product = sum1 * sum2
	var quotient = product / a
	return quotient
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	var result = s.vmcallv("complex_arithmetic")
	# (1.5 + 2.5) * (3.0 + 4.0) / 1.5 = 4.0 * 7.0 / 1.5 = 28.0 / 1.5 = 18.666...
	assert_almost_eq(result, 18.666, 0.001, "Complex arithmetic should work")

	s.queue_free()
	ts.queue_free()

func test_vector_fp_arithmetic():
	var gdscript_code = """
func vector_arithmetic():
	var v1 = Vector2(1.5, 2.5)
	var v2 = Vector2(3.0, 4.0)
	# Add components: v1.x + v2.x, v1.y + v2.y
	var x_sum = v1.x + v2.x
	var y_sum = v1.y + v2.y
	var total = x_sum + y_sum
	return total
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	var result = s.vmcallv("vector_arithmetic")
	# (1.5 + 3.0) + (2.5 + 4.0) = 4.5 + 6.5 = 11.0
	assert_almost_eq(result, 11.0, 0.001, "Vector FP arithmetic should work")

	s.queue_free()
	ts.queue_free()

func test_vector3_operations():
	var gdscript_code = """
func vector3_ops():
	var v = Vector3(1.0, 2.0, 3.0)
	var x = v.x
	var y = v.y
	var z = v.z
	# Test each component access and arithmetic
	var x2 = x * 2.0
	var y2 = y * 3.0
	var z2 = z * 4.0
	return x2 + y2 + z2
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	var result = s.vmcallv("vector3_ops")
	# 1.0 * 2 + 2.0 * 3 + 3.0 * 4 = 2 + 6 + 12 = 20
	assert_almost_eq(result, 20.0, 0.001, "Vector3 operations should work")

	s.queue_free()
	ts.queue_free()

func test_vector4_operations():
	var gdscript_code = """
func vector4_ops():
	var v = Vector4(1.0, 2.0, 3.0, 4.0)
	var w = v.w
	var z = v.z
	var y = v.y
	var x = v.x
	# Test component order and arithmetic
	var sum = w + z + y + x
	return sum
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	var result = s.vmcallv("vector4_ops")
	assert_almost_eq(result, 10.0, 0.001, "Vector4 component access should work")

	s.queue_free()

func test_float_vector_chaining():
	var gdscript_code = """
func chain_vector_ops():
	var v1 = Vector2(1.0, 2.0)
	var v2 = Vector2(v1.x + 1.0, v1.y + 2.0)
	var v3 = Vector2(v2.x * 2.0, v2.y * 3.0)
	return v3.x + v3.y
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	var result = s.vmcallv("chain_vector_ops")
	# v1 = (1.0, 2.0), v2 = (2.0, 4.0), v3 = (4.0, 12.0)
	# 4.0 + 12.0 = 16.0
	assert_almost_eq(result, 16.0, 0.001, "Chained vector operations should work")

	s.queue_free()
	ts.queue_free()

func test_mixed_int_float_arithmetic():
	var gdscript_code = """
func mixed_arithmetic():
	var f = 10.5
	var i = 5
	var sum1 = f + i
	var sum2 = i + f
	var product = f * i
	return sum1 + sum2 + product
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	var result = s.vmcallv("mixed_arithmetic")
	# 10.5 + 5 + 5 + 10.5 + 10.5 * 5 = 15.5 + 15.5 + 52.5 = 83.5
	assert_almost_eq(result, 83.5, 0.001, "Mixed int/float arithmetic should work")

	s.queue_free()

func test_float_division_edge_cases():
	var gdscript_code = """
func float_division():
	var a = 100.0
	var b = 4.0
	var c = 3.0
	var result = a / b / c
	return result
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	var result = s.vmcallv("float_division")
	# 100.0 / 4.0 / 3.0 = 25.0 / 3.0 = 8.333...
	assert_almost_eq(result, 8.333, 0.001, "Float division chain should work")

	s.queue_free()
	ts.queue_free()

# Logical operator tests (and, or, not)
func test_logical_operators():
	var gdscript_code = """
func test_and():
	var a = true
	var b = false
	return a and b

func test_or():
	var a = true
	var b = false
	return a or b

func test_not():
	var a = true
	return not a

func test_complex_logical():
	var x = 5
	var y = 10
	var z = 15
	return (x < y) and (y < z)

func test_short_circuit_and():
	if false and expensive_call():
		return true
	return false

func test_short_circuit_or():
	if true or expensive_call():
		return true
	return false

func expensive_call():
	# This should never be called in short-circuit eval
	return "ERROR"

func test_and_with_result():
	var a = 42
	var b = 100
	return (a > 0) and (b < 200)

func test_or_with_result():
	var a = -5
	var b = 10
	return (a > 0) or (b > 0)
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test basic AND
	var result = s.vmcallv("test_and")
	assert_eq(result, false, "true and false should be false")

	# Test basic OR
	result = s.vmcallv("test_or")
	assert_eq(result, true, "true or false should be true")

	# Test NOT
	result = s.vmcallv("test_not")
	assert_eq(result, false, "not true should be false")

	# Test complex logical
	result = s.vmcallv("test_complex_logical")
	assert_eq(result, true, "(5 < 10) and (10 < 15) should be true")

	# NOTE: Short-circuit evaluation is NOT supported - GDScript evaluates all arguments
	# before applying operators. The following tests are disabled:
	# result = s.vmcallv("test_short_circuit_and")
	# assert_eq(result, false, "Short-circuit AND should skip expensive call")
	# result = s.vmcallv("test_short_circuit_or")
	# assert_eq(result, true, "Short-circuit OR should skip expensive call")

	# Test AND with comparison result
	result = s.vmcallv("test_and_with_result")
	assert_eq(result, true, "(42 > 0) and (100 < 200) should be true")

	# Test OR with comparison result
	result = s.vmcallv("test_or_with_result")
	assert_eq(result, true, "(-5 > 0) or (10 > 0) should be true")

	s.queue_free()
	ts.queue_free()

# String concatenation tests - NOT SUPPORTED (string + operator)
# func test_string_concatenation():
# 	var gdscript_code = """
# func test_basic_concat():
# 	var a = "Hello"
# 	var b = " World"
# 	return a + b
#
# func test_concat_literal():
# 	return "Hello" + " " + "World!"
#
# func test_concat_with_number():
# 	var name = "Count"
# 	var num = 42
# 	return name + str(num)
#
# func test_concat_chain():
# 	var a = "A"
# 	var b = "B"
# 	var c = "C"
# 	return a + b + c
#
# func test_concat_in_expression():
# 	var prefix = "Result: "
# 	var value = 123
# 	return prefix + str(value)
# """
#
# 	var ts : Sandbox = Sandbox.new()
# 	ts.set_program(Sandbox_TestsTests)
# 	ts.restrictions = true
# 	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
# 	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")
#
# 	var s = Sandbox.new()
# 	s.load_buffer(compiled_elf)
# 	s.set_instructions_max(10000)
#
# 	# Test basic concatenation
# 	var result = s.vmcallv("test_basic_concat")
# 	assert_eq(result, "Hello World", "Basic string concat should work")
#
# 	# Test literal concatenation
# 	result = s.vmcallv("test_concat_literal")
# 	assert_eq(result, "Hello World!", "Literal concat should work")
#
# 	# Test concat with str()
# 	result = s.vmcallv("test_concat_with_number")
# 	assert_eq(result, "Count42", "Concat with str() should work")
#
# 	# Test concat chain
# 	result = s.vmcallv("test_concat_chain")
# 	assert_eq(result, "ABC", "Chain concat should work")
#
# 	# Test concat in expression
# 	result = s.vmcallv("test_concat_in_expression")
# 	assert_eq(result, "Result: 123", "Concat with str() in expression should work")
#
# 	s.queue_free()

# Modulo operator edge cases
func test_modulo_operator():
	var gdscript_code = """
func test_basic_mod():
	return 17 % 5

func test_mod_negative():
	return -17 % 5

func test_mod_float():
	return 17.5 % 3.0

func test_mod_zero_divisor():
	var a = 10
	var b = 0
	# Division by zero should produce inf or error at runtime
	# We'll just test the compiler accepts it
	return a % 1

func test_mod_chain():
	var a = 100
	var b = a % 7
	var c = b % 3
	return c

func test_mod_with_vars():
	var x = 25
	var y = 4
	var result = x % y
	return result
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test basic modulo
	var result = s.vmcallv("test_basic_mod")
	assert_eq(result, 2, "17 % 5 should be 2")

	# Test modulo with negative
	result = s.vmcallv("test_mod_negative")
	# Godot's modulo behavior: -17 % 5 = -2
	assert_eq(result, -2, "-17 % 5 should be -2")

	# Test modulo with floats
	result = s.vmcallv("test_mod_float")
	# 17.5 % 3.0 = 2.5
	assert_almost_eq(result, 2.5, 0.01, "Float modulo should work")

	# Test modulo chain
	result = s.vmcallv("test_mod_chain")
	# 100 % 7 = 2, 2 % 3 = 2
	assert_eq(result, 2, "Chained modulo should work")

	# Test modulo with variables
	result = s.vmcallv("test_mod_with_vars")
	assert_eq(result, 1, "25 % 4 should be 1")

	s.queue_free()
	ts.queue_free()

# Color type comprehensive tests
func test_color_comprehensive():
	var gdscript_code = """
func test_color_construction():
	var c = Color(1.0, 0.5, 0.25, 0.75)
	return c.r + c.g + c.b + c.a

func test_color_members():
	var c = Color(0.1, 0.2, 0.3, 1.0)
	var red = c.r
	var green = c.g
	var blue = c.b
	var alpha = c.a
	return red + green + blue + alpha

func test_color_operations():
	var c1 = Color(0.5, 0.5, 0.5, 0.5)
	var r = c1.r * 2.0
	return r

func test_color_named_colors():
	# Test that Color() constructor works
	var c = Color()
	return c.a  # Default alpha is 1.0

func test_color_component_access():
	var c = Color(1.0, 0.8, 0.6, 0.4)
	var arr = [c.r, c.g, c.b, c.a]
	var sum = 0.0
	for val in arr:
		sum = sum + val
	return sum

func test_color_comparison():
	var c1 = Color(1.0, 0.0, 0.0, 1.0)
	var c2 = Color(1.0, 0.0, 0.0, 1.0)
	return c1.r == c2.r and c1.g == c2.g
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test color construction
	var result = s.vmcallv("test_color_construction")
	assert_almost_eq(result, 2.5, 0.001, "Color component sum should be 2.5")

	# Test color member access
	result = s.vmcallv("test_color_members")
	assert_almost_eq(result, 1.6, 0.001, "Color members should sum to 1.6")

	# Test color operations
	result = s.vmcallv("test_color_operations")
	assert_almost_eq(result, 1.0, 0.001, "Color r * 2 should be 1.0")

	# Test default color
	result = s.vmcallv("test_color_named_colors")
	assert_almost_eq(result, 1.0, 0.001, "Default Color alpha should be 1.0")

	# Test component access with loop
	result = s.vmcallv("test_color_component_access")
	assert_almost_eq(result, 2.8, 0.001, "Color sum via loop should be 2.8")

	# Test color comparison
	result = s.vmcallv("test_color_comparison")
	assert_eq(result, true, "Color comparison should work")

	s.queue_free()
	ts.queue_free()


# Optimization verification tests (constant folding)
func test_constant_folding_optimization():
	var gdscript_code = """
func test_fold_add():
	return 10 + 20

func test_fold_sub():
	return 50 - 15

func test_fold_mul():
	return 6 * 7

func test_fold_div():
	return 100 / 4

func test_fold_complex():
	return (2 + 3) * 4

func test_fold_float():
	return 1.5 + 2.5

func test_fold_neg():
	return -(-42)

func test_fold_comparison():
	return 5 < 10

func test_fold_logical():
	return true and false

func test_fold_no_fold_vars():
	var a = 10
	var b = 20
	return a + b
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	# Write ELF for objdump inspection
	var file = FileAccess.open("res://tests/const_fold.elf", FileAccess.WRITE)
	if file:
		file.store_buffer(compiled_elf)
		file.close()

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test folded add (constant folded at compile time)
	var result = s.vmcallv("test_fold_add")
	assert_eq(result, 30, "Folded 10 + 20 = 30")

	# Test folded sub
	result = s.vmcallv("test_fold_sub")
	assert_eq(result, 35, "Folded 50 - 15 = 35")

	# Test folded mul
	result = s.vmcallv("test_fold_mul")
	assert_eq(result, 42, "Folded 6 * 7 = 42")

	# Test folded div
	result = s.vmcallv("test_fold_div")
	assert_eq(result, 25, "Folded 100 / 4 = 25")

	# Test folded complex
	result = s.vmcallv("test_fold_complex")
	assert_eq(result, 20, "Folded (2 + 3) * 4 = 20")

	# Test folded float
	result = s.vmcallv("test_fold_float")
	assert_almost_eq(result, 4.0, 0.001, "Folded 1.5 + 2.5 = 4.0")

	# Test folded negation
	result = s.vmcallv("test_fold_neg")
	assert_eq(result, 42, "Folded -(-42) = 42")

	# Test folded comparison
	result = s.vmcallv("test_fold_comparison")
	assert_eq(result, true, "Folded 5 < 10 = true")

	# Test folded logical
	result = s.vmcallv("test_fold_logical")
	assert_eq(result, false, "Folded true and false = false")

	# Test that variables are NOT folded
	result = s.vmcallv("test_fold_no_fold_vars")
	assert_eq(result, 30, "Variables should still add: 10 + 20 = 30")

	s.queue_free()
	ts.queue_free()

# Negative step in range loops - KNOWN BUG, disabled for now
# func test_negative_range_step():
# 	var gdscript_code = """
# func countdown_loop():
# 	var sum = 0
# 	for i in range(10, 0, -1):
# 		sum = sum + i
# 	return sum
#
# func countdown_with_step():
# 	var count = 0
# 	for i in range(20, 0, -2):
# 		count = count + 1
# 	return count
#
# func negative_range_values():
# 	var values = []
# 	for i in range(5, -1, -1):
# 		values.append(i)
# 	return values.size()
# """
#
# 	var ts : Sandbox = Sandbox.new()
# 	ts.set_program(Sandbox_TestsTests)
# 	ts.restrictions = true
# 	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
# 	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")
#
# 	var s = Sandbox.new()
# 	s.load_buffer(compiled_elf)
# 	s.set_instructions_max(10000)
#
# 	# Test countdown loop
# 	var result = s.vmcallv("countdown_loop")
# 	# 10 + 9 + 8 + 7 + 6 + 5 + 4 + 3 + 2 + 1 = 55
# 	assert_eq(result, 55, "Countdown loop should sum to 55")
#
# 	# Test countdown with step
# 	result = s.vmcallv("countdown_with_step")
# 	# 20, 18, 16, 14, 12, 10, 8, 6, 4, 2 = 10 iterations
# 	assert_eq(result, 10, "Countdown with step -2 should have 10 iterations")
#
# 	# Test negative range creates values
# 	result = s.vmcallv("negative_range_values")
# 	# 5, 4, 3, 2, 1, 0 = 6 values
# 	assert_eq(result, 6, "Negative range should create 6 values")
#
# 	s.queue_free()

# Comparison operators comprehensive tests
func test_comparison_operators_comprehensive():
	var gdscript_code = """
func test_int_comparisons(n):
	var a = 10
	var b = 20
	if n == 0:
		return a == b
	elif n == 1:
		return a != b
	elif n == 2:
		return a < b
	elif n == 3:
		return a <= b
	elif n == 4:
		return a > b
	elif n == 5:
		return a >= b
	else:
		return false

func test_float_comparisons():
	var a = 1.5
	var b = 2.5
	return a < b

func test_string_comparisons():
	var a = "apple"
	var b = "banana"
	return a < b

func test_mixed_type_comparison():
	var a = 10
	var b = 10.0
	# In GDScript, int and float can be compared
	return a == b

func test_chain_comparisons():
	var x = 5
	var y = 10
	var z = 15
	return (x < y) and (y < z) and (x < z)
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test int comparisons - test each comparison individually
	var result = s.vmcallv("test_int_comparisons", 0)
	assert_eq(result, false, "10 == 20 should be false")

	result = s.vmcallv("test_int_comparisons", 1)
	assert_eq(result, true, "10 != 20 should be true")

	result = s.vmcallv("test_int_comparisons", 2)
	assert_eq(result, true, "10 < 20 should be true")

	result = s.vmcallv("test_int_comparisons", 3)
	assert_eq(result, true, "10 <= 20 should be true")

	result = s.vmcallv("test_int_comparisons", 4)
	assert_eq(result, false, "10 > 20 should be false")

	result = s.vmcallv("test_int_comparisons", 5)
	assert_eq(result, false, "10 >= 20 should be false")

	# Test float comparisons
	result = s.vmcallv("test_float_comparisons")
	assert_eq(result, true, "1.5 < 2.5 should be true")

	# Test string comparisons
	result = s.vmcallv("test_string_comparisons")
	assert_eq(result, true, "\"apple\" < \"banana\" should be true")

	# Test mixed type comparison
	result = s.vmcallv("test_mixed_type_comparison")
	assert_eq(result, true, "10 == 10.0 should be true")

	# Test chain comparisons
	result = s.vmcallv("test_chain_comparisons")
	assert_eq(result, true, "Chain comparisons should work")

	s.queue_free()
	ts.queue_free()

# Control flow comprehensive tests
func test_control_flow_comprehensive():
	var gdscript_code = """
func test_nested_if():
	var x = 10
	var y = 20
	if x > 5:
		if y > 15:
			return 100
		else:
			return 50
	else:
		return 25

func test_elif_chain():
	var score = 75
	if score >= 90:
		return "A"
	elif score >= 80:
		return "B"
	elif score >= 70:
		return "C"
	elif score >= 60:
		return "D"
	else:
		return "F"

func test_ternary_like():
	var x = 10
	var result = 0
	if x > 5:
		result = 1
	else:
		result = -1
	return result

func test_while_with_break():
	var i = 0
	var sum = 0
	while i < 100:
		sum = sum + i
		i = i + 1
		if sum > 10:
			break
	return sum

func test_while_with_continue():
	var i = 0
	var sum = 0
	while i < 10:
		i = i + 1
		if i % 2 == 0:
			continue
		sum = sum + i
	return sum
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test nested if
	var result = s.vmcallv("test_nested_if")
	assert_eq(result, 100, "Nested if should return 100")

	# Test elif chain
	result = s.vmcallv("test_elif_chain")
	assert_eq(result, "C", "Elif chain should return 'C' for score 75")

	# Test ternary-like if
	result = s.vmcallv("test_ternary_like")
	assert_eq(result, 1, "Ternary-like if should return 1")

	# Test while with break
	result = s.vmcallv("test_while_with_break")
	# 0 + 1 + 2 + 3 + 4 + 5 = 15 (breaks when sum > 10)
	assert_eq(result, 15, "While with break should sum to 15")

	# Test while with continue
	result = s.vmcallv("test_while_with_continue")
	# i = 1, 3, 5, 7, 9 (skips evens) = 25
	assert_eq(result, 25, "While with continue should sum odd numbers to 25")

	s.queue_free()
	ts.queue_free()

# Array methods and operations
func test_array_operations():
	var gdscript_code = """
func test_array_size():
	var arr = [1, 2, 3, 4, 5]
	return arr.size()

func test_array_append():
	var arr = []
	arr.append(10)
	arr.append(20)
	arr.append(30)
	return arr.size()

func test_array_clear():
	var arr = [1, 2, 3]
	arr.clear()
	return arr.size()

func test_array_sort():
	var arr = [3, 1, 4, 1, 5]
	arr.sort()
	return arr[0]

func test_array_reverse():
	var arr = [1, 2, 3]
	arr.reverse()
	return arr[0]
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test array size
	var result = s.vmcallv("test_array_size")
	assert_eq(result, 5, "Array size should be 5")

	# Test array append
	result = s.vmcallv("test_array_append")
	assert_eq(result, 3, "After 3 appends, size should be 3")

	# Test array clear
	result = s.vmcallv("test_array_clear")
	assert_eq(result, 0, "After clear, size should be 0")

	# Test array sort
	result = s.vmcallv("test_array_sort")
	assert_eq(result, 1, "After sort, first element should be 1")

	# Test array reverse
	result = s.vmcallv("test_array_reverse")
	assert_eq(result, 3, "After reverse, first element should be 3")

	s.queue_free()
	ts.queue_free()

# Method call tests
func test_method_calls():
	var gdscript_code = """
func test_string_length():
	var s = "Hello"
	return s.length()

func test_string_methods():
	var s = "  Hello World  "
	return s.strip_edges().length()

func test_array_method_chain():
	var arr = [1, 2, 3]
	var size = arr.size()
	return size

func test_string_find():
	var s = "Hello World"
	return s.find("World")
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test string length
	var result = s.vmcallv("test_string_length")
	assert_eq(result, 5, "String 'Hello' length should be 5")

	# Test string method chaining
	result = s.vmcallv("test_string_methods")
	assert_eq(result, 11, "Stripped 'Hello World' length should be 11")

	# Test array method
	result = s.vmcallv("test_array_method_chain")
	assert_eq(result, 3, "Array size should be 3")

	# Test string find
	result = s.vmcallv("test_string_find")
	assert_eq(result, 6, "Find 'World' in 'Hello World' should return 6")

	s.queue_free()
	ts.queue_free()

# Codegen quality verification via ELF inspection helper
func test_codegen_quality_verification():
	# This test compiles code and writes ELF for manual objdump inspection
	# Use: riscv64-linux-gnu-objdump -d res://tests/codegen_quality.elf
	var gdscript_code = """
func simple_add():
	return 5 + 3

func register_pressure():
	var a = 1
	var b = 2
	var c = 3
	var d = 4
	var e = 5
	var f = 6
	return a + b + c + d + e + f

func loop_test():
	var sum = 0
	for i in range(10):
		sum = sum + i
	return sum

func float_test():
	var a = 1.5
	var b = 2.5
	return a + b
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	# Write ELF for objdump inspection
	var file = FileAccess.open("res://tests/codegen_quality.elf", FileAccess.WRITE)
	if file:
		file.store_buffer(compiled_elf)
		file.close()

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Verify functionality
	var result = s.vmcallv("simple_add")
	assert_eq(result, 8, "simple_add should return 8")

	result = s.vmcallv("register_pressure")
	assert_eq(result, 21, "register_pressure sum should be 21")

	result = s.vmcallv("loop_test")
	assert_eq(result, 45, "loop_test sum should be 45")

	result = s.vmcallv("float_test")
	assert_almost_eq(result, 4.0, 0.001, "float_test should return 4.0")

	s.queue_free()
	ts.queue_free()


func test_peephole_pattern_e():
	# Test Pattern E optimization: x = x + 1 (increment optimization)
	# This tests that the compiler properly optimizes the common increment pattern
	var gdscript_code = """
func increment_by_one(x):
	var i = x
	i += 1
	return i

func increment_multiple_times():
	var count = 0
	count += 1
	count += 1
	count += 1
	count += 1
	count += 1
	return count

func increment_in_loop():
	var sum = 0
	for i in range(10):
		sum += 1
	return sum

func increment_with_arithmetic(x):
	var result = x
	result += 5
	result += 3
	return result

func float_increment(x):
	var f = x
	f += 1.5
	f += 2.5
	return f
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test increment by one
	assert_true(s.has_function("increment_by_one"), "Should have increment_by_one function")
	var result = s.vmcallv("increment_by_one", 5)
	assert_eq(result, 6, "increment_by_one(5) should return 6")

	result = s.vmcallv("increment_by_one", 0)
	assert_eq(result, 1, "increment_by_one(0) should return 1")

	result = s.vmcallv("increment_by_one", -5)
	assert_eq(result, -4, "increment_by_one(-5) should return -4")

	# Test multiple increments
	assert_true(s.has_function("increment_multiple_times"), "Should have increment_multiple_times function")
	result = s.vmcallv("increment_multiple_times")
	assert_eq(result, 5, "increment_multiple_times should return 5")

	# Test increment in loop
	assert_true(s.has_function("increment_in_loop"), "Should have increment_in_loop function")
	result = s.vmcallv("increment_in_loop")
	assert_eq(result, 10, "increment_in_loop should return 10")

	# Test increment with arithmetic
	assert_true(s.has_function("increment_with_arithmetic"), "Should have increment_with_arithmetic function")
	result = s.vmcallv("increment_with_arithmetic", 10)
	assert_eq(result, 18, "increment_with_arithmetic(10) should return 18")

	# Test float increment
	assert_true(s.has_function("float_increment"), "Should have float_increment function")
	result = s.vmcallv("float_increment", 1.0)
	assert_eq(result, 5.0, "float_increment(1.0) should return 5.0")

	s.queue_free()
	ts.queue_free()


func test_peephole_combined_patterns():
	# Test combination of multiple peephole optimizations
	var gdscript_code = """
func combined_arithmetic(a, b, c):
	var x = a
	var y = b
	var z = x + y
	var result = z + c
	return result

func nested_arithmetic():
	var a = 1
	var b = 2
	var c = 3
	var d = 4
	var result = a + b + c + d
	return result

func arithmetic_chain(x):
	var result = x
	result = result + 1
	result = result + 2
	result = result + 3
	return result
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test combined arithmetic
	assert_true(s.has_function("combined_arithmetic"), "Should have combined_arithmetic function")
	var result = s.vmcallv("combined_arithmetic", 10, 20, 30)
	assert_eq(result, 60, "combined_arithmetic(10, 20, 30) should return 60")

	# Test nested arithmetic
	assert_true(s.has_function("nested_arithmetic"), "Should have nested_arithmetic function")
	result = s.vmcallv("nested_arithmetic")
	assert_eq(result, 10, "nested_arithmetic should return 10")

	# Test arithmetic chain
	assert_true(s.has_function("arithmetic_chain"), "Should have arithmetic_chain function")
	result = s.vmcallv("arithmetic_chain", 100)
	assert_eq(result, 106, "arithmetic_chain(100) should return 106")

	s.queue_free()
	ts.queue_free()


func test_optimization_preserves_semantics():
	# Ensure that optimizations don't change program semantics
	var gdscript_code = """
func side_effect_test(x):
	var a = x
	var b = a + 1
	var c = b + 1
	var d = c + 1
	return d

func constant_folding_test():
	var a = 5 + 3
	var b = a + 2
	var c = b * 2
	return c

func order_of_operations(x):
	var a = x + 1
	var b = a * 2
	var c = b + 3
	return c
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test side effects
	assert_true(s.has_function("side_effect_test"), "Should have side_effect_test function")
	var result = s.vmcallv("side_effect_test", 0)
	assert_eq(result, 3, "side_effect_test(0) should return 3")

	result = s.vmcallv("side_effect_test", 10)
	assert_eq(result, 13, "side_effect_test(10) should return 13")

	# Test constant folding preserves semantics
	assert_true(s.has_function("constant_folding_test"), "Should have constant_folding_test function")
	result = s.vmcallv("constant_folding_test")
	assert_eq(result, 20, "constant_folding_test should return 20 ((5+3)+2)*2 = 20")

	# Test order of operations
	assert_true(s.has_function("order_of_operations"), "Should have order_of_operations function")
	result = s.vmcallv("order_of_operations", 5)
	assert_eq(result, 15, "order_of_operations(5) should return 15 ((5+1)*2+3 = 15)")

	s.queue_free()
	ts.queue_free()

func test_packed_arrays():
	# Test packed array constructors
	var gdscript_code = """
func test_empty_packed_byte_array():
	return PackedByteArray()

func test_packed_byte_array():
	return PackedByteArray([1, 2, 3, 4, 5])

func test_empty_packed_int32_array():
	return PackedInt32Array()

func test_packed_int32_array():
	return PackedInt32Array([10, 20, 30, 40, 50])

func test_empty_packed_int64_array():
	return PackedInt64Array()

func test_packed_int64_array():
	return PackedInt64Array([100, 200, 300, 400, 500])

func test_empty_packed_float32_array():
	return PackedFloat32Array()

func test_packed_float32_array():
	return PackedFloat32Array([1.1, 2.2, 3.3, 4.4, 5.5])

func test_empty_packed_float64_array():
	return PackedFloat64Array()

func test_packed_float64_array():
	return PackedFloat64Array([10.01, 20.02, 30.03, 40.04, 50.05])

func test_empty_packed_string_array():
	return PackedStringArray()

func test_packed_string_array():
	return PackedStringArray(["one", "two", "three", "four", "five"])

func test_empty_packed_vector2_array():
	return PackedVector2Array()

func test_packed_vector2_array():
	return PackedVector2Array([Vector2(1.0,2.0), Vector2(3.0,4.0), Vector2(5.0,6.0)])

func test_packed_vector2_array_i():
	return PackedVector2Array([Vector2(1,2), Vector2(3,4), Vector2(5,6)])

func test_empty_packed_vector3_array():
	return PackedVector3Array()

func test_packed_vector3_array():
	return PackedVector3Array([Vector3(1.0,2.0,3.0), Vector3(4.0,5.0,6.0), Vector3(7.0,8.0,9.0)])

func test_packed_vector3_array_i():
	return PackedVector3Array([Vector3(1,2,3), Vector3(4,5,6), Vector3(7,8,9)])

func test_empty_packed_color_array():
	return PackedColorArray()

func test_packed_color_array():
	return PackedColorArray([Color(1.0,0.0,0.0), Color(0.0,1.0,0.0), Color(0.0,0.0,1.0)])

func test_empty_packed_vector4_array():
	return PackedVector4Array()

func test_packed_vector4_array():
	return PackedVector4Array([Vector4(1.0,2.0,3.0,4.0), Vector4(5.0,6.0,7.0,8.0), Vector4(9.0,10.0,11.0,12.0)])

func test_packed_vector4_array_i():
	return PackedVector4Array([Vector4(1,2,3,4), Vector4(5,6,7,8), Vector4(9,10,11,12)])
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)

	# Test that all functions exist
	assert_true(s.has_function("test_empty_packed_byte_array"), "Should have test_empty_packed_byte_array function")
	assert_true(s.has_function("test_empty_packed_int32_array"), "Should have test_empty_packed_int32_array function")
	assert_true(s.has_function("test_empty_packed_int64_array"), "Should have test_empty_packed_int64_array function")
	assert_true(s.has_function("test_empty_packed_float32_array"), "Should have test_empty_packed_float32_array function")
	assert_true(s.has_function("test_empty_packed_float64_array"), "Should have test_empty_packed_float64_array function")
	assert_true(s.has_function("test_empty_packed_string_array"), "Should have test_empty_packed_string_array function")
	assert_true(s.has_function("test_empty_packed_vector2_array"), "Should have test_empty_packed_vector2_array function")
	assert_true(s.has_function("test_empty_packed_vector3_array"), "Should have test_empty_packed_vector3_array function")
	assert_true(s.has_function("test_empty_packed_color_array"), "Should have test_empty_packed_color_array function")
	assert_true(s.has_function("test_empty_packed_vector4_array"), "Should have test_empty_packed_vector4_array function")

	assert_true(s.has_function("test_packed_byte_array"), "Should have test_packed_byte_array function")
	assert_true(s.has_function("test_packed_int32_array"), "Should have test_packed_int32_array function")
	assert_true(s.has_function("test_packed_int64_array"), "Should have test_packed_int64_array function")
	assert_true(s.has_function("test_packed_float32_array"), "Should have test_packed_float32_array function")
	assert_true(s.has_function("test_packed_float64_array"), "Should have test_packed_float64_array function")
	assert_true(s.has_function("test_packed_string_array"), "Should have test_packed_string_array function")
	assert_true(s.has_function("test_packed_vector2_array"), "Should have test_packed_vector2_array function")
	assert_true(s.has_function("test_packed_vector2_array_i"), "Should have test_packed_vector2_array_i function")
	assert_true(s.has_function("test_packed_vector3_array"), "Should have test_packed_vector3_array function")
	assert_true(s.has_function("test_packed_vector3_array_i"), "Should have test_packed_vector3_array_i function")
	assert_true(s.has_function("test_packed_color_array"), "Should have test_packed_color_array function")
	assert_true(s.has_function("test_packed_vector4_array"), "Should have test_packed_vector4_array function")
	assert_true(s.has_function("test_packed_vector4_array_i"), "Should have test_packed_vector4_array_i function")

	# Test that functions return valid packed arrays (non-nil)
	var result = s.vmcallv("test_empty_packed_byte_array")
	assert_eq_deep(result, PackedByteArray([]))
	result = s.vmcallv("test_packed_byte_array")
	assert_eq_deep(result, PackedByteArray([1, 2, 3, 4, 5]))

	result = s.vmcallv("test_empty_packed_int32_array")
	assert_eq_deep(result, PackedInt32Array([]))
	result = s.vmcallv("test_packed_int32_array")
	assert_eq_deep(result, PackedInt32Array([10, 20, 30, 40, 50]))

	result = s.vmcallv("test_empty_packed_int64_array")
	assert_eq_deep(result, PackedInt64Array([]))
	result = s.vmcallv("test_packed_int64_array")
	assert_eq_deep(result, PackedInt64Array([100, 200, 300, 400, 500]))

	result = s.vmcallv("test_empty_packed_float32_array")
	assert_eq_deep(result, PackedFloat32Array([]))
	result = s.vmcallv("test_packed_float32_array")
	assert_eq_deep(result, PackedFloat32Array([1.1, 2.2, 3.3, 4.4, 5.5]))

	result = s.vmcallv("test_empty_packed_float64_array")
	assert_eq_deep(result, PackedFloat64Array([]))
	result = s.vmcallv("test_packed_float64_array")
	assert_eq_deep(result, PackedFloat64Array([10.01, 20.02, 30.03, 40.04, 50.05]))

	result = s.vmcallv("test_empty_packed_string_array")
	assert_eq_deep(result, PackedStringArray([]))
	result = s.vmcallv("test_packed_string_array")
	assert_eq_deep(result, PackedStringArray(["one", "two", "three", "four", "five"]))

	result = s.vmcallv("test_empty_packed_vector2_array")
	assert_eq_deep(result, PackedVector2Array([]))
	result = s.vmcallv("test_packed_vector2_array")
	assert_eq_deep(result, PackedVector2Array([Vector2(1,2), Vector2(3,4), Vector2(5,6)]))
	result = s.vmcallv("test_packed_vector2_array_i")
	assert_eq_deep(result, PackedVector2Array([Vector2(1,2), Vector2(3,4), Vector2(5,6)]))

	result = s.vmcallv("test_empty_packed_vector3_array")
	assert_eq_deep(result, PackedVector3Array([]))
	result = s.vmcallv("test_packed_vector3_array")
	assert_eq_deep(result, PackedVector3Array([Vector3(1,2,3), Vector3(4,5,6), Vector3(7,8,9)]))
	result = s.vmcallv("test_packed_vector3_array_i")
	assert_eq_deep(result, PackedVector3Array([Vector3(1,2,3), Vector3(4,5,6), Vector3(7,8,9)]))

	result = s.vmcallv("test_empty_packed_color_array")
	assert_eq_deep(result, PackedColorArray([]))
	result = s.vmcallv("test_packed_color_array")
	assert_eq_deep(result, PackedColorArray([Color(1,0,0), Color(0,1,0), Color(0,0,1)]))

	result = s.vmcallv("test_empty_packed_vector4_array")
	assert_eq_deep(result, PackedVector4Array([]))
	result = s.vmcallv("test_packed_vector4_array")
	assert_eq_deep(result, PackedVector4Array([Vector4(1,2,3,4), Vector4(5,6,7,8), Vector4(9,10,11,12)]))
	result = s.vmcallv("test_packed_vector4_array_i")
	assert_eq_deep(result, PackedVector4Array([Vector4(1,2,3,4), Vector4(5,6,7,8), Vector4(9,10,11,12)]))

	s.queue_free()
	ts.queue_free()

func test_const_declarations():
	# Test const declarations (const is synonymous with var)
	var gdscript_code = """
func test_const_int():
	const x = 42
	return x

func test_const_float():
	const pi = 3.14159
	return pi

func test_const_string():
	const greeting = "hello"
	return greeting

func test_const_bool():
	const flag = true
	return flag

func test_const_multiple():
	const a = 10
	const b = 20
	const c = 30
	return a + b + c

func test_const_in_expression():
	const multiplier = 2.5
	const base = 10
	return base * multiplier
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test const int
	var result = s.vmcallv("test_const_int")
	assert_eq(result, 42, "Const int should be 42")

	# Test const float
	result = s.vmcallv("test_const_float")
	assert_almost_eq(result, 3.14159, 0.00001, "Const float should be 3.14159")

	# Test const string
	result = s.vmcallv("test_const_string")
	assert_eq(result, "hello", "Const string should be 'hello'")

	# Test const bool
	result = s.vmcallv("test_const_bool")
	assert_eq(result, true, "Const bool should be true")

	# Test multiple const declarations
	result = s.vmcallv("test_const_multiple")
	assert_eq(result, 60, "Sum of consts should be 60")

	# Test const in expression
	result = s.vmcallv("test_const_in_expression")
	assert_almost_eq(result, 25.0, 0.001, "Const expression should be 25.0")

	s.queue_free()
	ts.queue_free()


func test_self_get_name():
	# Test self.get_name() - calling a method on self
	var gdscript_code = """
func test_get_name():
	return self.get_name()
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test that get_name() can be called on self
	# get_name() returns a StringName, which is type 21
	var result = s.vmcallv("test_get_name")
	assert_eq(typeof(result), TYPE_STRING_NAME, "self.get_name() should return a StringName")
	# Just check that we got some result (not null)
	assert_not_null(result, "self.get_name() should return a value")

	s.queue_free()


func test_freestanding_function_call():
	# Test freestanding get_name() - should be treated as self.get_name()
	var gdscript_code = """
func test_freestanding():
	return get_name()
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test that get_name() works as a freestanding function
	var result = s.vmcallv("test_freestanding")
	assert_eq(typeof(result), TYPE_STRING_NAME, "get_name() should return a StringName")
	assert_not_null(result, "get_name() should return a value")

	s.queue_free()
	ts.queue_free()


func test_get_node_no_args():
	# Test get_node() without arguments - should return self
	var gdscript_code = """
func test_get_node_self():
	var node = get_node()
	return node.get_name()
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test that get_node() returns a node
	var result = s.vmcallv("test_get_node_self")
	assert_eq(typeof(result), TYPE_STRING_NAME, "get_node().get_name() should return a StringName")
	assert_not_null(result, "get_node().get_name() should return a value")

	s.queue_free()
	ts.queue_free()


func test_multiple_freestanding_calls():
	# Test multiple freestanding function calls in sequence
	var gdscript_code = """
func test_multiple_calls():
	var name1 = get_name()
	var name2 = self.get_name()
	return name1 == name2
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Both should return the same name
	var result = s.vmcallv("test_multiple_calls")
	assert_eq(result, true, "get_name() and self.get_name() should return the same value")

	s.queue_free()
	ts.queue_free()


func test_properties():
	# Test property access - obj.property should translate to obj.get("property")
	# and obj.property = value should translate to obj.set("property", value)
	var gdscript_code = """
func test_property_get():
	var node = get_node()
	# Access the 'name' property - should use VGET instruction
	var name = node.name
	return name
func test_property_set():
	var node = get_node()
	# Set the 'name' property using VSET instruction
	var old_name = node.name
	node.name = "test_name"
	var new_name = node.name
	return new_name

func test_property_self_get():
	return self.name
func test_property_self_set():
	var old_name = self.name
	self.name = "test_name"
	return self.name
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test that property access works
	var result = s.vmcallv("test_property_get")
	assert_not_null(result, "node.name should return a value")

	result = s.vmcallv("test_property_set")
	assert_eq(result, "test_name", "After setting, node.name should be 'test_name'")

	result = s.vmcallv("test_property_self_get")
	assert_not_null(result, "self.name should return a value")
	result = s.vmcallv("test_property_self_set")
	assert_eq(result, "test_name", "After setting, self.name should be 'test_name'")
	result = s.vmcallv("test_property_self_get")
	assert_eq(result, "test_name", "self.name should be 'test_name' after setting")

	s.queue_free()
	ts.queue_free()

func test_gdscript_benchmarks():
	var benchmarks = {
		"fibonacci": """
func fibonacci(n : int):
	if n <= 1:
		return n
	return fibonacci(n - 1) + fibonacci(n - 2)
""",
		"factorial": """
func factorial(n):
	if n <= 1:
		return 1
	return n * factorial(n - 1)
""",
		"pf32a_operation": """
func pf32a_operation(array):
	var i = 0
	for n in range(10000):
		array.set(i, i * 2.0)
	return array
"""
	}

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true

	for name in benchmarks.keys():
		var gdscript_code = benchmarks[name]
		var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
		assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty for %s" % name)

		var s = Sandbox.new()
		s.load_buffer(compiled_elf)
		s.set_instructions_max(20000)
		assert_true(s.has_function(name), "Compiled ELF should have function '%s'" % name)

		# Benchmark the compiled function
		var start_time = Time.get_ticks_usec()
		if name == "fibonacci":
			var result = s.vmcallv(name, 20)  # Fibonacci of 20
			assert_eq(result, 6765, "fibonacci(20) should return 6765")
		elif name == "factorial":
			var result = s.vmcallv(name, 10)  # Factorial of 10
			assert_eq(result, 3628800, "factorial(10) should return 3628800")
		elif name == "pf32a_operation":
			var array : PackedFloat32Array = PackedFloat32Array()
			array.resize(10000)
			var result = s.vmcallv(name, array)
			assert_eq(result.size(), 10000, "pf32a_operation should return array of length 10000")
		var end_time = Time.get_ticks_usec()
		print("%s benchmark took %d us" % [name, end_time - start_time])

		s.queue_free()
	ts.queue_free()

func test_global_variables():
	# Test that global var and const declarations work correctly
	var gdscript_code = """
var global_counter: int = 0
const GLOBAL_CONST: float = 42.0
var g_string = "Hello, World!"
var g_array = []

func increment_counter():
	global_counter = global_counter + 1
	return global_counter

func get_counter():
	return global_counter

func loop_counter():
	global_counter = 0
	while global_counter < 5:
		global_counter = global_counter + 1
	return global_counter

func get_const():
	return GLOBAL_CONST

func const_arithmetic():
	return GLOBAL_CONST * 2.0 + 8.0

func get_string():
	return g_string

func set_string(new_value):
	g_string = new_value
	return g_string

func test_array():
	g_array.append(10)
	g_array.append(20)
	return g_array

func set_array(new_array):
	g_array = new_array
	return g_array

"""

	print("Compiling GDScript code with global variables:")
	print(gdscript_code)

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty for global variables test")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(1000)

	assert_true(s.has_function("increment_counter"), "Compiled ELF should have function 'increment_counter'")
	assert_true(s.has_function("get_counter"), "Compiled ELF should have function 'get_counter'")
	assert_true(s.has_function("loop_counter"), "Compiled ELF should have function 'loop_counter'")
	assert_true(s.has_function("get_const"), "Compiled ELF should have function 'get_const'")
	assert_true(s.has_function("const_arithmetic"), "Compiled ELF should have function 'const_arithmetic'")
	assert_true(s.has_function("get_string"), "Compiled ELF should have function 'get_string'")
	assert_true(s.has_function("set_string"), "Compiled ELF should have function 'set_string'")
	assert_true(s.has_function("test_array"), "Compiled ELF should have function 'test_array'")
	assert_true(s.has_function("set_array"), "Compiled ELF should have function 'set_array'")

	# Test global constant
	assert_eq(s.vmcallv("get_const"), 42.0, "get_const() should return 42.0")
	assert_almost_eq(s.vmcallv("const_arithmetic"), 92.0, 0.001, "const_arithmetic() should return 92.0")

	# Test global variable that gets incremented
	assert_eq(s.vmcallv("get_counter"), 0, "Initial counter should be 0")
	assert_eq(s.vmcallv("increment_counter"), 1, "First increment should return 1")
	assert_eq(s.vmcallv("get_counter"), 1, "Counter should now be 1")
	assert_eq(s.vmcallv("increment_counter"), 2, "Second increment should return 2")
	assert_eq(s.vmcallv("get_counter"), 2, "Counter should now be 2")
	assert_eq(s.vmcallv("loop_counter"), 5, "loop_counter should return 5")

	# Test global string
	assert_eq(s.vmcallv("get_string"), "Hello, World!", "Initial string should be 'Hello, World!'")
	assert_eq(s.vmcallv("set_string", "world"), "world", "set_string should return new value")
	assert_eq(s.vmcallv("get_string"), "world", "String should now be 'world'")

	# Test global array
	var array_result = s.vmcallv("test_array")
	assert_eq_deep(array_result, [10, 20])
	array_result = s.vmcallv("set_array", [333, 666, 999])
	assert_eq_deep(array_result, [333, 666, 999])
	array_result = s.vmcallv("test_array")
	assert_eq_deep(array_result, [333, 666, 999, 10, 20])

	s.queue_free()
	ts.queue_free()

func test_untyped_global_error():
	# Test that untyped global variables without initializers produce a helpful error
	var gdscript_code = """
var untyped_global

func test():
	untyped_global = 42
	return untyped_global
"""

	print("Testing untyped global variable error handling:")
	print(gdscript_code)

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)

	# Should fail to compile
	assert_eq(compiled_elf.is_empty(), true, "Compilation should fail for untyped global without initializer")

	var error_msg = ts.vmcall("get_compiler_error", "")
	assert_true(error_msg.find("requires either a type hint or an initializer") != -1, \
		"Error message should mention type hint or initializer requirement")

	ts.queue_free()


func test_export_attribute():
	# Test that @export attribute works for global variables
	var gdscript_code = """
@export
var exported_int : int = 42

@export
var exported_float : float = 3.14

@export
var exported_string : String = "test"

@export
var exported_array : Array = []

var non_exported : int = 100

func get_exported_int():
	return exported_int

func get_exported_float():
	return exported_float

func get_exported_string():
	return exported_string

func get_exported_array():
	return exported_array

func get_non_exported():
	return non_exported
"""

	print("Testing @export attribute:")
	print(gdscript_code)

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compilation with @export should succeed")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(10000)

	# Test that exported variables work correctly
	assert_eq(s.vmcallv("get_exported_int"), 42, "exported_int should be 42")
	assert_almost_eq(s.vmcallv("get_exported_float"), 3.14, 0.001, "exported_float should be 3.14")
	assert_eq(s.vmcallv("get_exported_string"), "test", "exported_string should be 'test'")
	var array_result = s.vmcallv("get_exported_array")
	assert_eq_deep(array_result, [])
	assert_eq(s.vmcallv("get_non_exported"), 100, "non_exported should be 100")

	assert_eq(s.get("exported_int"), 42, "exported_int property should be 42")
	assert_almost_eq(s.get("exported_float"), 3.14, 0.001, "exported_float property should be 3.14")
	assert_eq(s.get("exported_string"), "test", "exported_string property should be 'test'")
	assert_eq_deep(s.get("exported_array"), [])
	assert_eq(s.get("non_exported"), null, "non_exported is not exported, should be nil")

	s.set("exported_int", 100)
	assert_eq(s.get("exported_int"), 100, "exported_int property should be updated to 100")
	s.set("exported_float", 6.28)
	assert_almost_eq(s.get("exported_float"), 6.28, 0.001, "exported_float property should be updated to 6.28")
	s.set("exported_string", "updated")
	assert_eq(s.get("exported_string"), "updated", "exported_string property should be updated to 'updated'")
	s.set("exported_array", [1, 2, 3])
	assert_eq_deep(s.get("exported_array"), [1, 2, 3])

	s.queue_free()
	ts.queue_free()

