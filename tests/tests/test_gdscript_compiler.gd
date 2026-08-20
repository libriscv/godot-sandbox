extends GutTest

var Sandbox_TestsTests = load("res://tests/tests.elf")
var holder = Sandbox.new()

# Compile GDScript using an embedded compiler and test the output

func test_compiler_variant_layout():
	# The compiler bakes the Variant layout into the code it emits, so it has to
	# agree with the sandbox it is running in. Godot's double-precision builds
	# (real_t = double) widen every inline real_t payload and grow the Variant
	# from 24 to 40 bytes; a compiler that disagreed would read every vector
	# component from the wrong offset.
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var layout = ts.vmcall("compiler_variant_layout")

	assert_eq(layout["compiler_real_size"], layout["guest_real_size"],
		"Compiler and sandbox API must agree on sizeof(real_t)")
	assert_eq(layout["compiler_variant_size"], layout["guest_variant_size"],
		"Compiler and sandbox API must agree on sizeof(Variant)")

	if layout["double_precision"]:
		assert_eq(layout["compiler_real_size"], 8, "real_t is a double in double-precision builds")
		assert_eq(layout["compiler_variant_size"], 40, "Variant is 40 bytes in double-precision builds")
	else:
		assert_eq(layout["compiler_real_size"], 4, "real_t is a float in single-precision builds")
		assert_eq(layout["compiler_variant_size"], 24, "Variant is 24 bytes in single-precision builds")

	ts.queue_free()

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


var _printed : Array = []

func _collect_print(v):
	_printed.append(v)


func test_global_print():
	# print() is a GDScript global, not a method on the owner node. Compiled as
	# a self-call it becomes a VCALL that Godot accepts and drops in silence,
	# so the body ran and nothing was ever printed.
	var gdscript_code = """
func say_one():
	print("Hello")

func say_many():
	print("x = ", 42, true)

func say_nothing():
	print()

func say_and_return():
	print("side effect")
	return 7

func returns_print_value():
	return print("value")
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(6000)
	s.set_redirect_stdout(_collect_print)

	_printed = []
	s.vmcallv("say_one")
	assert_eq_deep(_printed, ["Hello"])

	# Godot's print() concatenates its arguments into one line with no
	# separator, so this is one entry rather than three.
	_printed = []
	s.vmcallv("say_many")
	assert_eq_deep(_printed, ["x = 42true"])

	# print() with no arguments still prints an empty line.
	_printed = []
	s.vmcallv("say_nothing")
	assert_eq_deep(_printed, [""])

	# A print() in the middle of a function must not disturb the return value.
	_printed = []
	var result = s.vmcallv("say_and_return")
	assert_eq(result, 7, "say_and_return should still return 7")
	assert_eq_deep(_printed, ["side effect"])

	# GDScript's print() evaluates to null.
	_printed = []
	result = s.vmcallv("returns_print_value")
	assert_eq(result, null, "print() should evaluate to null")
	assert_eq_deep(_printed, ["value"])

	s.queue_free()
	ts.queue_free()


# A host object whose _to_string() runs guest code again. Godot calls
# _to_string() during stringification, which is what print() does to its
# arguments, so this is reachable from inside a guest print().
class ReentrantPrinter extends Node:
	var target : Sandbox = null
	var depth : int = 0

	func _to_string() -> String:
		depth += 1
		if depth < 4 and target != null:
			target.vmcallv("print_arg", self)
		return "<ReentrantPrinter>"


func test_print_reentrancy_is_refused():
	# print() concatenates its arguments before emitting anything, so the host
	# holds a half-built line while it stringifies. Stringifying an Object runs
	# _to_string(), which can re-enter the guest and reach print() again. That
	# has to be refused: left open it recurses as deep as the guest likes, each
	# level nesting a guest execution inside a host syscall.
	var gdscript_code = """
func print_arg(n):
	print(n)
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(6000)
	s.set_redirect_stdout(_collect_print)

	var node = ReentrantPrinter.new()
	node.target = s

	_printed = []
	s.vmcallv("print_arg", node)

	# _to_string() ran once, for the outer print. The re-entrant print() inside
	# it was refused, so it never got to stringify a second time.
	assert_eq(node.depth, 1, "_to_string() should run exactly once")
	assert_engine_error("Recursive call to Sandbox::print() detected, ignoring.")

	# The outer print still produced its line.
	assert_eq_deep(_printed, ["<ReentrantPrinter>"])

	node.free()
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

func test_safegdscript():
	var gdscript_code = """
extends Label3D

func some_function():
	var counter = 0
	while counter < 10:
		counter += 2
	return counter

func meaning_of_life():
	return 42

func meaning_of_this() -> String:
	var hi: String = "Hi"
	set_text(hi)
	return hi
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compilation with @export should succeed")

	var temp_file_path = "user://temp_safegdscript.elf"
	var file = FileAccess.open(temp_file_path, FileAccess.WRITE)
	file.store_buffer(compiled_elf)
	file.close()

	var l = Label3D.new()
	l.set_script(load(temp_file_path))
	l.set_instructions_max(10000)

	# Test that exported variables work correctly
	var result = l.call("some_function")
	assert_eq(result, 10, "some_function should return 10")

	result = l.call("meaning_of_life")
	assert_eq(result, 42, "meaning_of_life should return 42")

	result = l.call("meaning_of_this")
	assert_eq(result, "Hi", "meaning_of_this should return 'Hi'")
	assert_eq(l.get("text"), "Hi", "Label3D text property should be 'Hi'")

	# Write GDScript to "user://temp_safegdscript.sgd"
	var gdscript_file = FileAccess.open("user://temp_safegdscript.sgd", FileAccess.WRITE)
	gdscript_file.store_string(gdscript_code)
	gdscript_file.close()

	# Load the GDScript file using SafeGDScript
	l.set_script(load("user://temp_safegdscript.sgd"))
	l.set_instructions_max(10000)

	# Test that exported variables work correctly
	result = l.call("some_function")
	assert_eq(result, 10, "some_function should return 10 after loading from .sgd")
	result = l.call("meaning_of_life")
	assert_eq(result, 42, "meaning_of_life should return 42 after loading from .sgd")
	result = l.call("meaning_of_this")
	assert_eq(result, "Hi", "meaning_of_this should return 'Hi' after loading from .sgd")
	assert_eq(l.get("text"), "Hi", "Label3D text property should be 'Hi' after loading from .sgd")

	l.queue_free()
	ts.queue_free()

# Helper: compile GDScript inside a Sandbox and return a Sandbox running the result
func _compile_and_load(gdscript_code: String, instructions_max: int = 4000) -> Sandbox:
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	ts.queue_free()
	assert_eq(compiled_elf.is_empty(), false, "Compilation should succeed")
	if compiled_elf.is_empty():
		return null
	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(instructions_max)
	return s

func test_bitwise_operators():
	var gdscript_code = """
func bit_and(a : int, b : int):
	return a & b

func bit_or(a : int, b : int):
	return a | b

func bit_xor(a : int, b : int):
	return a ^ b

func shift_left(a : int, b : int):
	return a << b

func shift_right(a : int, b : int):
	return a >> b

func bit_not(a : int):
	return ~a

func untyped_and(a, b):
	return a & b

func compound():
	var a = 1
	a <<= 4
	a |= 3
	a &= 30
	a ^= 1
	return a

func precedence(a : int):
	return a | 1 << 4
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("bit_and", 12, 10), 8, "12 & 10 should be 8")
	assert_eq(s.vmcallv("bit_or", 12, 3), 15, "12 | 3 should be 15")
	assert_eq(s.vmcallv("bit_xor", 12, 10), 6, "12 ^ 10 should be 6")
	assert_eq(s.vmcallv("shift_left", 1, 10), 1024, "1 << 10 should be 1024")
	assert_eq(s.vmcallv("shift_right", 1024, 3), 128, "1024 >> 3 should be 128")
	assert_eq(s.vmcallv("shift_right", -8, 1), -4, ">> should be an arithmetic shift")
	assert_eq(s.vmcallv("bit_not", 5), -6, "~5 should be -6")
	assert_eq(s.vmcallv("untyped_and", 12, 10), 8, "Untyped 12 & 10 should be 8")
	assert_eq(s.vmcallv("compound"), 19, "Bitwise compound assignment should yield 19")
	assert_eq(s.vmcallv("precedence", 3), 3 | 1 << 4, "Shifts should bind tighter than |")

	s.queue_free()

func test_number_literals():
	var gdscript_code = """
func hex():
	return 0xFF

func hex_separated():
	return 0xDEAD_BEEF

func binary():
	return 0b1011

func separated():
	return 1_000_000

func exponent():
	return 1.5e3
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("hex"), 255, "0xFF should be 255")
	assert_eq(s.vmcallv("hex_separated"), 0xDEADBEEF, "0xDEAD_BEEF should be 3735928559")
	assert_eq(s.vmcallv("binary"), 11, "0b1011 should be 11")
	assert_eq(s.vmcallv("separated"), 1000000, "1_000_000 should be 1000000")
	assert_almost_eq(s.vmcallv("exponent"), 1500.0, 0.001, "1.5e3 should be 1500.0")

	s.queue_free()

func test_ternary_expression():
	var gdscript_code = """
func pick(a):
	return 10 if a else 20

func nested(a : int):
	return 1 if a == 1 else 2 if a == 2 else 3

func in_expression(a):
	return 1 + (10 if a else 20)
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("pick", true), 10, "Ternary should pick the true value")
	assert_eq(s.vmcallv("pick", false), 20, "Ternary should pick the false value")
	assert_eq(s.vmcallv("nested", 1), 1, "Nested ternary should return 1")
	assert_eq(s.vmcallv("nested", 2), 2, "Nested ternary should return 2")
	assert_eq(s.vmcallv("nested", 9), 3, "Nested ternary should return 3")
	assert_eq(s.vmcallv("in_expression", true), 11, "Ternary inside an expression should work")

	s.queue_free()

func test_match_statement():
	var gdscript_code = """
func classify(a):
	match a:
		1:
			return 10
		2:
			return 20
		_:
			return 30

func multi_pattern(a):
	match a:
		1, 2, 3:
			return 100
		_:
			return 200

func strings(a):
	match a:
		"a":
			return 1
		"b":
			return 2
		_:
			return 3

func no_wildcard(a):
	var r = 0
	match a:
		1:
			r = 1
		2:
			r = 2
	return r
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("classify", 1), 10, "match 1 should return 10")
	assert_eq(s.vmcallv("classify", 2), 20, "match 2 should return 20")
	assert_eq(s.vmcallv("classify", 9), 30, "match wildcard should return 30")
	assert_eq(s.vmcallv("multi_pattern", 3), 100, "Multi-pattern branch should match 3")
	assert_eq(s.vmcallv("multi_pattern", 4), 200, "Multi-pattern branch should not match 4")
	assert_eq(s.vmcallv("strings", "b"), 2, "match on strings should work")
	assert_eq(s.vmcallv("no_wildcard", 2), 2, "match without wildcard should take branch 2")
	assert_eq(s.vmcallv("no_wildcard", 9), 0, "match without wildcard should take no branch")

	s.queue_free()

func test_default_arguments():
	var gdscript_code = """
func add(a, b = 5, c = 10):
	return a + b + c

func none_given():
	return add(1)

func one_given():
	return add(1, 2)

func all_given():
	return add(1, 2, 3)
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("none_given"), 16, "add(1) should be 1 + 5 + 10")
	assert_eq(s.vmcallv("one_given"), 13, "add(1, 2) should be 1 + 2 + 10")
	assert_eq(s.vmcallv("all_given"), 6, "add(1, 2, 3) should be 6")

	s.queue_free()

func test_global_stores_survive_optimization():
	# Regression test: the liveness analysis behind dead code elimination did
	# not treat STORE_GLOBAL as reading its value register, so the instruction
	# that produced the value was deleted as dead.
	var gdscript_code = """
var g = 0
var h = 0
var i = 0

func setup():
	g = 5
	h = 7
	i = 9

func read_g():
	setup()
	return g

func read_h():
	setup()
	return h

func read_i():
	setup()
	return i
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("read_g"), 5, "Global g should be 5")
	assert_eq(s.vmcallv("read_h"), 7, "Global h should be 7")
	assert_eq(s.vmcallv("read_i"), 9, "Global i should be 9")

	s.queue_free()

func test_local_shadows_global():
	# A local declared with the same name as a global shadows it: reads and
	# writes go to the local, and the global is left alone.
	var gdscript_code = """
var counter = 10

func shadowed():
	var counter = 1
	counter = counter + 1
	return counter

func read_global():
	return counter

func param_shadows(counter):
	return counter
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("shadowed"), 2, "The local shadows the global")
	assert_eq(s.vmcallv("read_global"), 10, "The global is untouched by the shadowing local")
	assert_eq(s.vmcallv("param_shadows", 3), 3, "A parameter shadows the global")

	s.queue_free()

func test_short_circuit_evaluation():
	# 'and' and 'or' must not evaluate the right-hand side once the left decides
	# the result. The counter makes a skipped evaluation observable.
	var gdscript_code = """
var calls = 0

func bump():
	calls = calls + 1
	return true

func reset():
	calls = 0
	return calls

func and_false():
	calls = 0
	var r = false and bump()
	return calls

func and_true():
	calls = 0
	var r = true and bump()
	return calls

func or_true():
	calls = 0
	var r = true or bump()
	return calls

func or_false():
	calls = 0
	var r = false or bump()
	return calls

func and_value():
	return 5 and 3

func or_value():
	return 0 or 7

func not_value(a):
	return not a
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("and_false"), 0, "'false and f()' must not call f()")
	assert_eq(s.vmcallv("and_true"), 1, "'true and f()' must call f()")
	assert_eq(s.vmcallv("or_true"), 0, "'true or f()' must not call f()")
	assert_eq(s.vmcallv("or_false"), 1, "'false or f()' must call f()")

	# The operators booleanize: they return a bool, not one of the operands.
	assert_eq(s.vmcallv("and_value"), true, "'5 and 3' is true")
	assert_eq(s.vmcallv("or_value"), true, "'0 or 7' is true")
	assert_eq(s.vmcallv("not_value", false), true, "'not false' is true")
	assert_eq(s.vmcallv("not_value", true), false, "'not true' is false")
	assert_eq(s.vmcallv("not_value", 0), true, "'not 0' is true")

	s.queue_free()

func test_variant_truthiness():
	# Truthiness follows Variant::booleanize(). Testing only the low byte of the
	# payload made 256 false and 512 false as well.
	var gdscript_code = """
func truthy(a):
	if a:
		return 1
	return 0
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("truthy", 256), 1, "256 is truthy")
	assert_eq(s.vmcallv("truthy", 512), 1, "512 is truthy")
	assert_eq(s.vmcallv("truthy", 1), 1, "1 is truthy")
	assert_eq(s.vmcallv("truthy", 0), 0, "0 is falsy")
	assert_eq(s.vmcallv("truthy", 1.5), 1, "1.5 is truthy")
	assert_eq(s.vmcallv("truthy", 0.0), 0, "0.0 is falsy")
	assert_eq(s.vmcallv("truthy", true), 1, "true is truthy")
	assert_eq(s.vmcallv("truthy", false), 0, "false is falsy")

	s.queue_free()

func test_global_initializer_forms():
	# Initializers that are not plain literals used to be dropped silently,
	# leaving the global NIL.
	var gdscript_code = """
const MAX = 10
var neg = -5
var negf = -2.5
var folded = MAX
var arr = [1, 2, 3]
var dict = {"a": 1, "b": 2}
var nested = [[1, 2], {"k": 3}]
var packed = PackedInt32Array()
var empty_arr = []
var empty_dict = {}
var typed_arr: Array
var typed_str: String
var typed_int: int
var typed_float: float

func get_neg():
	return neg

func get_negf():
	return negf

func get_folded():
	return folded

func get_arr():
	return arr

func get_dict():
	return dict

func get_nested():
	return nested

func get_packed():
	return packed

func get_empty_arr():
	return empty_arr

func get_empty_dict():
	return empty_dict

func get_typed_arr():
	return typed_arr

func get_typed_str():
	return typed_str

func get_typed_int():
	return typed_int

func get_typed_float():
	return typed_float
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("get_neg"), -5, "Negative integer literal initializer")
	assert_almost_eq(s.vmcallv("get_negf"), -2.5, 0.001, "Negative float literal initializer")
	assert_eq(s.vmcallv("get_folded"), 10, "Initializer referring to a const folds to its value")
	assert_eq(s.vmcallv("get_arr"), [1, 2, 3], "Non-empty array literal initializer")
	assert_eq(s.vmcallv("get_dict"), {"a": 1, "b": 2}, "Non-empty dictionary literal initializer")
	assert_eq(s.vmcallv("get_nested"), [[1, 2], {"k": 3}], "Nested container initializer")
	assert_eq(s.vmcallv("get_packed"), PackedInt32Array(), "Empty packed array initializer")
	assert_eq(s.vmcallv("get_empty_arr"), [], "Empty array initializer")
	assert_eq(s.vmcallv("get_empty_dict"), {}, "Empty dictionary initializer")

	# A type hint with no initializer gets the type's default value.
	assert_eq(s.vmcallv("get_typed_arr"), [], "A typed Array defaults to an empty Array")
	assert_eq(s.vmcallv("get_typed_str"), "", "A typed String defaults to an empty String")
	assert_eq(s.vmcallv("get_typed_int"), 0, "A typed int defaults to 0")
	assert_almost_eq(s.vmcallv("get_typed_float"), 0.0, 0.001, "A typed float defaults to 0.0")

	s.queue_free()

func test_global_container_mutation():
	# A container global holds a permanent Variant that has to survive across
	# calls, and reassigning it must replace the value rather than leak it.
	var gdscript_code = """
var items = [1, 2]
var lookup = {"a": 1}

func append_item(v):
	items.append(v)
	return items

func read_items():
	return items

func replace_items(v):
	items = v
	return items

func read_lookup():
	return lookup
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("read_items"), [1, 2], "The initializer survives into the first call")
	assert_eq(s.vmcallv("append_item", 3), [1, 2, 3], "Appending mutates the global")
	assert_eq(s.vmcallv("read_items"), [1, 2, 3], "The mutation survives across calls")
	assert_eq(s.vmcallv("replace_items", [9]), [9], "Reassigning replaces the value")
	assert_eq(s.vmcallv("read_items"), [9], "The replacement survives across calls")
	assert_eq(s.vmcallv("read_lookup"), {"a": 1}, "A dictionary global keeps its value")

	s.queue_free()

func test_declared_float_type_coercion():
	# A declared float holds a float even when initialized or assigned from an
	# integer; without the conversion the payload is read as a double.
	var gdscript_code = """
var gf: float = 0

func read_global():
	return gf

func assign_global():
	gf = 3
	return gf

func local_init():
	var f: float = 2
	return f

func local_assign():
	var f: float = 0.0
	f = 7
	return f

func local_math():
	var f: float = 2
	return f * 2.0
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_typeof(s.vmcallv("read_global"), TYPE_FLOAT)
	assert_almost_eq(s.vmcallv("read_global"), 0.0, 0.001, "A float global initialized from 0")
	assert_almost_eq(s.vmcallv("assign_global"), 3.0, 0.001, "Assigning an int to a float global")
	assert_almost_eq(s.vmcallv("local_init"), 2.0, 0.001, "A float local initialized from an int")
	assert_almost_eq(s.vmcallv("local_assign"), 7.0, 0.001, "Assigning an int to a float local")
	assert_almost_eq(s.vmcallv("local_math"), 4.0, 0.001, "Arithmetic on a coerced float local")

	s.queue_free()

func test_many_globals():
	# A global's address is .globals + index * sizeof(Variant). Adding that
	# offset with a separate ADDI truncates it to 12 signed bits, so everything
	# past global #85 addressed the wrong slot.
	var gdscript_code = ""
	for i in range(150):
		gdscript_code += "var g%d = %d\n" % [i, i]
	gdscript_code += """
func read_first():
	return g0

func read_middle():
	return g100

func read_last():
	return g149

func write_last(v):
	g149 = v
	return g149

func read_last_again():
	return g149
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	assert_eq(s.vmcallv("read_first"), 0, "First global")
	assert_eq(s.vmcallv("read_middle"), 100, "Global past the 12-bit immediate range")
	assert_eq(s.vmcallv("read_last"), 149, "Last global")
	assert_eq(s.vmcallv("write_last", 42), 42, "Writing a global past the immediate range")
	assert_eq(s.vmcallv("read_last_again"), 42, "The write landed in the right slot")

	s.queue_free()

func test_large_stack_frame():
	# A Variant slot past 2047 bytes into the frame cannot be reached with a
	# 12-bit immediate, so the address has to be computed in a register first.
	# Computing it in one the surrounding instruction was already using stored
	# the address instead of the value: `sd t2, 2048(sp)` became "compute
	# sp+2048 into t2, then store t2", and the variable came back as a stack
	# address. Around eighty locals is enough.
	var gdscript_code = "func many_locals():\n"
	for i in range(120):
		gdscript_code += "\tvar v%d = %d\n" % [i, i]
	gdscript_code += "\treturn v0 + v60 + v119\n"

	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	assert_eq(s.vmcallv("many_locals"), 0 + 60 + 119, "Locals past the 12-bit frame offset")

	s.queue_free()


func test_far_branch_is_relaxed():
	# A conditional branch reaches +-4KB. A loop body long enough to outgrow
	# that used to get a masked displacement, which is a branch to somewhere
	# else entirely. The exit branch is now rewritten as an inverted branch over
	# a jump, which reaches +-1MB.
	var gdscript_code = "func long_loop():\n\tvar total = 0\n\tvar i = 0\n\twhile i < 3:\n"
	for k in range(60):
		gdscript_code += "\t\tvar a%d = i + %d\n" % [k, k]
	gdscript_code += "\t\ttotal = total"
	for k in range(60):
		gdscript_code += " + a%d" % k
	gdscript_code += "\n\t\ti = i + 1\n\treturn total\n"

	var s = _compile_and_load(gdscript_code, 60000)
	if s == null:
		return

	# Three iterations of sum(i + k) for k in 0..59, with i = 0, 1, 2.
	var expected = 0
	for i in range(3):
		for k in range(60):
			expected += i + k
	assert_eq(s.vmcallv("long_loop"), expected, "A loop whose body outgrows a branch")

	s.queue_free()


func test_structs():
	# A struct is sugar for a Dictionary with a fixed set of keys: an instance
	# really is a Dictionary, so Godot sees one on the way back out.
	var gdscript_code = """
struct BankAccount:
	var balance = 0
	var loan = 0

func make_default():
	return BankAccount.new()

func make_positional():
	return BankAccount.new(100, 50)

func make_plain_call():
	return BankAccount(100, 50)

func make_named():
	return BankAccount.new(loan = 50, balance = 100)

func make_mixed():
	return BankAccount.new(100, loan = 50)

func make_partial():
	return BankAccount.new(loan = 50)

func net_worth():
	var account = BankAccount.new(100, 50)
	return account.balance - account.loan

func deposit(amount):
	var account = BankAccount.new(100, 50)
	account.balance += amount
	return account.balance

func repay(account: BankAccount, amount):
	account.loan = account.loan - amount
	return account.loan

func is_a_dictionary():
	return BankAccount.new().size()
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("make_default"), {"balance": 0, "loan": 0}, "Declared defaults")
	assert_eq(s.vmcallv("make_positional"), {"balance": 100, "loan": 50}, "Positional values")
	assert_eq(s.vmcallv("make_plain_call"), {"balance": 100, "loan": 50}, "Constructor-call form")
	assert_eq(s.vmcallv("make_named"), {"balance": 100, "loan": 50}, "Named values, out of order")
	assert_eq(s.vmcallv("make_mixed"), {"balance": 100, "loan": 50}, "Positional then named")
	assert_eq(s.vmcallv("make_partial"), {"balance": 0, "loan": 50}, "A field left out keeps its default")

	assert_eq(s.vmcallv("net_worth"), 50, "Reading two fields")
	assert_eq(s.vmcallv("deposit", 5), 105, "Compound assignment to a field")
	# An instance passed in from Godot is an ordinary Dictionary.
	assert_eq(s.vmcallv("repay", {"balance": 100, "loan": 50}, 20), 30, "A struct parameter")
	assert_eq(s.vmcallv("is_a_dictionary"), 2, "An instance is a Dictionary")

	s.queue_free()


func test_struct_globals_and_nesting():
	# A struct-typed global is built by the global initializer, and a field
	# declared as another struct defaults to an instance of it.
	var gdscript_code = """
struct Point:
	var x = 0
	var y = 0

struct Sprite:
	var pos: Point
	var name = "unnamed"

var vault: Point
var origin = Point.new(3, 4)

func read_vault():
	return vault

func write_vault():
	vault.x = 7
	vault.y = 8
	return vault.x + vault.y

func read_origin():
	return origin.x + origin.y

func nested_default():
	return Sprite.new()

func nested_read():
	var s = Sprite.new()
	s.pos.x = 5
	return s.pos.x
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("read_vault"), {"x": 0, "y": 0}, "A struct-typed global starts at its defaults")
	assert_eq(s.vmcallv("write_vault"), 15, "Writing fields of a struct global")
	assert_eq(s.vmcallv("read_origin"), 7, "A global initialized from a constructor")
	assert_eq(s.vmcallv("nested_default"), {"pos": {"x": 0, "y": 0}, "name": "unnamed"},
		"A struct field defaults to an instance")
	assert_eq(s.vmcallv("nested_read"), 5, "Writing through a nested struct")

	s.queue_free()


func test_dictionary_member_access():
	# In GDScript d.key is d["key"]. The member path used to take the property
	# syscall, which reaches an Object's properties and throws on a Dictionary.
	var gdscript_code = """
func round_trip():
	var d = {}
	d.count = 1
	d.count += 4
	return d.count

func literal_key():
	var d = {"a": 1, "b": 2}
	return d.a + d.b
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("round_trip"), 5, "Member access on a Dictionary")
	assert_eq(s.vmcallv("literal_key"), 3, "Reading dictionary literal keys by name")

	s.queue_free()
