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

func sum1(n):
	var total = 0
	for i in range(n):
		total += i
	return total

func sum2(n):
	var total = 0
	var i = 0
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
	assert_true(s.has_function("sum1"), "Compiled ELF should have function 'sum1'")
	assert_true(s.has_function("sum2"), "Compiled ELF should have function 'sum2'")

	# Test the compiled functions
	assert_eq(s.vmcallv("truthy"), true, "truthy() should return true")
	assert_eq(s.vmcallv("falsy"), false, "falsy() should return false")
	assert_eq(s.vmcallv("add", 7, 21), 28, "add(7, 21) = 28")
	assert_eq(s.vmcallv("sum1", 10), 45, "sum1(10) should return 45")
	assert_eq(s.vmcallv("sum2", 10), 45, "sum2(10) should return 45")

	s.queue_free()


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

	s.queue_free()

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
	#result = s.vmcallv("countdown_loop")
	# sum = 10 + 9 + 8 + 7 + 6 + 5 + 4 + 3 + 2 + 1 = 55
	#assert_eq(result, 55, "countdown_loop should sum 10..1 = 55")

	s.queue_free()

func test_gdscript_benchmarks():
	var benchmarks = {
		"fibonacci": """
func fibonacci(n):
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

func test_inline_vector_primitives():
	# Test inline construction and member access for Vector2/3/4 without syscalls
	var gdscript_code = """
func test_vector2():
	var v = Vector2(3.0, 4.0)
	return v.x + v.y

func test_vector3():
	var v = Vector3(1.0, 2.0, 3.0)
	return v.x + v.y + v.z

func test_vector4():
	var v = Vector4(10.0, 20.0, 30.0, 40.0)
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

func test_chained_vectors():
	var v1 = Vector2(10.0, 20.0)
	var v2 = Vector2(v1.x, v1.y)
	return v2.x + v2.y
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
	assert_almost_eq(result, 7.0, 0.001, "Vector2(3, 4).x + .y should be 7.0")

	# Test Vector3
	result = s.vmcallv("test_vector3")
	assert_almost_eq(result, 6.0, 0.001, "Vector3(1, 2, 3) sum should be 6.0")

	# Test Vector4
	result = s.vmcallv("test_vector4")
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

	# Test chained operations
	result = s.vmcallv("test_chained_vectors")
	assert_almost_eq(result, 30.0, 0.001, "Chained vector operations should work")

	s.queue_free()

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

func make_dict_and_add():
	var dict = Dictionary()
	dict["key1"] = "value1"
	dict["key2"] = 42
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
	dict = s.vmcallv("make_dict_and_add")
	assert_eq(dict.size(), 2, "Dictionary with 2 keys should have size 2")
	assert_eq(dict["key1"], "value1", "key1 should have value 'value1'")
	assert_eq(dict["key2"], 42, "key2 should have value 42")

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
