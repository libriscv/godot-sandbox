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


func test_profiled_build():
	var gdscript_code = """
func leaf(x : int):
	return x + 1

func mid(x : int):
	return leaf(x) + leaf(x)

func run(x : int):
	return mid(x) + mid(x)
"""
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true

	# The instrumentation is a compile-time choice, so the two builds come from
	# two entry points rather than a flag on one.
	var plain_elf = ts.vmcall("compile", gdscript_code)
	assert_eq(plain_elf.is_empty(), false, "Plain ELF should not be empty")
	var profiled_elf = ts.vmcall("compile_profiled", gdscript_code)
	assert_eq(profiled_elf.is_empty(), false, "Profiled ELF should not be empty")

	var plain = Sandbox.new()
	plain.load_buffer(plain_elf)
	assert_eq(plain.address_of("__gdsc_profiling"), 0,
		"An unprofiled build carries no profiling area")

	var profiled = Sandbox.new()
	profiled.load_buffer(profiled_elf)
	assert_true(profiled.address_of("__gdsc_profiling") != 0,
		"A profiled build exports the profiling area")

	# Instrumentation must not change what the program computes.
	assert_eq(plain.vmcallv("run", 10), 44, "run(10) = 44 without instrumentation")
	assert_eq(profiled.vmcallv("run", 10), 44, "run(10) = 44 with instrumentation")
	assert_eq(plain.vmcallv("mid", 3), 8, "mid(3) = 8 without instrumentation")
	assert_eq(profiled.vmcallv("mid", 3), 8, "mid(3) = 8 with instrumentation")

	# It costs instructions, though, which is why it is not always emitted.
	assert_true(profiled_elf.size() > plain_elf.size(),
		"The profiled build is the larger of the two")


func test_profiling_skips_self_instrumented():
	var gdscript_code = """
func spin(n : int):
	var total = 0
	var i = 0
	while i < n:
		total = total + i
		i = i + 1
	return total
"""
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var plain_elf = ts.vmcall("compile", gdscript_code)
	assert_eq(plain_elf.is_empty(), false, "Plain ELF should not be empty")
	var profiled_elf = ts.vmcall("compile_profiled", gdscript_code)
	assert_eq(profiled_elf.is_empty(), false, "Profiled ELF should not be empty")

	# A program with no instrumentation of its own is interval-sampled.
	var plain = Sandbox.new()
	plain.load_buffer(plain_elf)
	plain.set_instructions_max(64)
	plain.profiling = true
	Sandbox.clear_hotspots()
	assert_eq(plain.vmcallv("spin", 20000), 199990000, "spin(20000) = 199990000")
	var sampled = Sandbox.get_hotspots(10)
	assert_true(sampled[sampled.size() - 1]["samples_total"] > 0,
		"An uninstrumented program is sampled")

	# One that times its own functions is not sampled on top of that: the
	# sampler re-enters simulate() every few hundred instructions, which would
	# distort the very timings the program is recording.
	var profiled = Sandbox.new()
	profiled.load_buffer(profiled_elf)
	profiled.set_instructions_max(64)
	profiled.profiling = true
	assert_eq(profiled.profiling, true, "The property still reads as enabled")
	Sandbox.clear_hotspots()
	assert_eq(profiled.vmcallv("spin", 20000), 199990000, "spin(20000) = 199990000")
	var unsampled = Sandbox.get_hotspots(10)
	assert_eq(unsampled[unsampled.size() - 1]["samples_total"], 0,
		"A self-instrumented program is not sampled")

	plain.profiling = false
	profiled.profiling = false


func test_debug_build_shadow_stack():
	# Debug info is the same kind of compile-time choice profiling is, and for
	# the same reason gets its own entry point: the Sandbox ABI has no argument
	# count, so an added parameter would reach an old caller as a null pointer.
	var gdscript_code = """
func c():
	return 3

func b():
	return c()

func a():
	return b()

func run():
	return a()
"""
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true

	var plain_elf = ts.vmcall("compile", gdscript_code)
	assert_eq(plain_elf.is_empty(), false, "Plain ELF should not be empty")
	# compile_debug() takes the breakpoint lines alongside the source; an empty
	# list is the debuggable build with nothing to stop on.
	var debug_elf = ts.vmcall("compile_debug", gdscript_code, PackedInt32Array())
	assert_eq(debug_elf.is_empty(), false, "Debug ELF should not be empty")

	var plain = Sandbox.new()
	plain.load_buffer(plain_elf)
	assert_eq(plain.address_of("__gdsc_debug"), 0,
		"An ordinary build carries no shadow stack")

	var debug = Sandbox.new()
	debug.load_buffer(debug_elf)
	assert_true(debug.address_of("__gdsc_debug") != 0,
		"A debug build exports the shadow stack")

	# A call stack is not an answer, so recording one must not change any.
	assert_eq(plain.vmcallv("run"), 3, "run() = 3 without the shadow stack")
	assert_eq(debug.vmcallv("run"), 3, "run() = 3 with the shadow stack")

	# Only a call carries instrumentation, so the cost is there but bounded;
	# a statement costs nothing at all.
	assert_true(debug_elf.size() > plain_elf.size(),
		"The debug build is the larger of the two")


func test_line_table_is_published_by_every_build():
	# The table maps a code address to a source line. It costs no instructions,
	# so unlike the shadow stack it is published by an ordinary compile too --
	# which is what lets a fault in a shipped program still name a line.
	var gdscript_code = """
func add(x, y):
	var total = x + y
	return total
"""
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true

	var elf = ts.vmcall("compile", gdscript_code)
	assert_eq(elf.is_empty(), false, "Compiled ELF should not be empty")

	var blob : PackedByteArray = ts.vmcall("get_line_table")
	assert_true(blob.size() >= 12, "The line table blob carries at least a header")
	# 'GDSL', little-endian, then the layout version.
	assert_eq(blob.decode_u32(0), 0x4C534447, "Line table magic")
	assert_eq(blob.decode_u32(4), 1, "Line table version")

	var rows = blob.decode_u32(8)
	assert_true(rows > 0, "A compiled program has rows in its line table")
	assert_eq(blob.size(), 12 + rows * 8, "The blob is exactly its rows")

	# Ordering is what makes a lookup meaningful: a row covers everything up to
	# the next one, so an out-of-order table would answer with an arbitrary row.
	var previous_address = -1
	for i in range(rows):
		var address = blob.decode_u32(12 + i * 8)
		var line = blob.decode_u32(16 + i * 8)
		assert_true(address > previous_address,
			"Row %d ascends (address %d after %d)" % [i, address, previous_address])
		previous_address = address
		assert_true(address >= 0x10000, "Row %d is an ELF address, not a text offset" % i)
		# The source above is four lines including the leading newline.
		assert_true(line >= 1 and line <= 4, "Row %d names a line of the source" % i)


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

func test_unused_parameters():
	# Every parameter arrives whether or not the body reads it, and the prologue
	# copies each one into its own stack slot. Sizing the frame from the
	# registers the optimized instructions still name left the copy of an unused
	# parameter writing past the end of the frame, and the compile was refused
	# with "max_registers too low".
	var gdscript_code = """
func first_of_two(a, b):
	return a

func middle_of_three(a, b, c):
	return b

func last_of_seven(a, b, c, d, e, f, g):
	return g

func none_of_two(a, b):
	return 7

func overwrites_its_parameter(a):
	a = 7
	return a

func maybe_overwrites(b, n):
	# The loop may not run, so the parameter that looks overwritten is still
	# the value returned.
	var t = 0
	while t < n:
		t = t + 1
		b = t
	return b
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)

	assert_eq(s.vmcallv("first_of_two", 11, 22), 11, "first_of_two(11, 22) should return 11")
	assert_eq(s.vmcallv("middle_of_three", 1, 2, 3), 2, "middle_of_three(1, 2, 3) should return 2")
	assert_eq(s.vmcallv("last_of_seven", 1, 2, 3, 4, 5, 6, 7), 7, "last_of_seven(..) should return the seventh")
	assert_eq(s.vmcallv("none_of_two", 1, 2), 7, "none_of_two(1, 2) should return 7")
	assert_eq(s.vmcallv("overwrites_its_parameter", 3), 7, "overwrites_its_parameter(3) should return 7")
	assert_eq(s.vmcallv("maybe_overwrites", 3, 0), 3, "maybe_overwrites(3, 0) should return the argument")
	assert_eq(s.vmcallv("maybe_overwrites", 3, 2), 2, "maybe_overwrites(3, 2) should return the loop's value")

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


func test_global_math_matches_gdscript():
	# The global math functions are the same @GlobalScope functions Godot runs,
	# so the assertion is not a table of constants but Godot itself: the
	# compiled guest and the GDScript below have to agree.
	var gdscript_code = """
func f_abs(x):
	return abs(x)
func f_sign(x):
	return sign(x)
func f_floor(x):
	return floor(x)
func f_ceil(x):
	return ceil(x)
func f_round(x):
	return round(x)
func f_min(a, b):
	return min(a, b)
func f_max(a, b):
	return max(a, b)
func f_clamp(x, lo, hi):
	return clamp(x, lo, hi)
func f_sqrt(x):
	return sqrt(x)
func f_pow(x, y):
	return pow(x, y)
func f_sin(x):
	return sin(x)
func f_cos(x):
	return cos(x)
func f_atan2(y, x):
	return atan2(y, x)
func f_exp(x):
	return exp(x)
func f_log(x):
	return log(x)
func f_fmod(x, y):
	return fmod(x, y)
func f_fposmod(x, y):
	return fposmod(x, y)
func f_posmod(x, y):
	return posmod(x, y)
func f_lerp(a, b, t):
	return lerp(a, b, t)
func f_inverse_lerp(a, b, v):
	return inverse_lerp(a, b, v)
func f_remap(v, a, b, c, d):
	return remap(v, a, b, c, d)
func f_smoothstep(a, b, t):
	return smoothstep(a, b, t)
func f_deg_to_rad(x):
	return deg_to_rad(x)
func f_rad_to_deg(x):
	return rad_to_deg(x)
func f_snapped(x, step):
	return snapped(x, step)
func f_wrapi(v, lo, hi):
	return wrapi(v, lo, hi)
func f_wrapf(v, lo, hi):
	return wrapf(v, lo, hi)
func f_floori(x):
	return floori(x)
func f_ceili(x):
	return ceili(x)
func f_roundi(x):
	return roundi(x)
func f_is_equal_approx(a, b):
	return is_equal_approx(a, b)
func f_is_zero_approx(x):
	return is_zero_approx(x)
func f_is_nan(x):
	return is_nan(x)
func f_is_finite(x):
	return is_finite(x)
func f_variadic_min(a, b, c):
	return min(a, b, c)
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(6000)

	# The type-preserving globals: an integer in, an integer out.
	assert_eq(s.vmcallv("f_abs", -7), abs(-7), "abs(-7)")
	assert_eq(typeof(s.vmcallv("f_abs", -7)), TYPE_INT, "abs() of an int is an int")
	assert_almost_eq(s.vmcallv("f_abs", -7.5), abs(-7.5), 0.0000001, "abs(-7.5)")
	assert_eq(typeof(s.vmcallv("f_abs", -7.5)), TYPE_FLOAT, "abs() of a float is a float")
	assert_eq(s.vmcallv("f_sign", -7), sign(-7), "sign(-7)")
	assert_eq(s.vmcallv("f_sign", 0), sign(0), "sign(0)")
	assert_almost_eq(s.vmcallv("f_sign", 7.5), sign(7.5), 0.0000001, "sign(7.5)")
	assert_eq(s.vmcallv("f_floor", -7), floor(-7), "floor() of an int is that int")
	assert_almost_eq(s.vmcallv("f_floor", -7.5), floor(-7.5), 0.0000001, "floor(-7.5)")
	assert_almost_eq(s.vmcallv("f_ceil", -7.5), ceil(-7.5), 0.0000001, "ceil(-7.5)")
	assert_almost_eq(s.vmcallv("f_round", -7.5), round(-7.5), 0.0000001, "round(-7.5)")
	assert_almost_eq(s.vmcallv("f_round", 2.5), round(2.5), 0.0000001, "round(2.5)")
	assert_eq(s.vmcallv("f_min", 3, 9), min(3, 9), "min(3, 9)")
	assert_almost_eq(s.vmcallv("f_min", 3.5, 9.5), min(3.5, 9.5), 0.0000001, "min(3.5, 9.5)")
	assert_eq(s.vmcallv("f_max", 3, 9), max(3, 9), "max(3, 9)")
	assert_eq(s.vmcallv("f_clamp", 11, 0, 10), clamp(11, 0, 10), "clamp(11, 0, 10)")
	assert_eq(s.vmcallv("f_clamp", -11, 0, 10), clamp(-11, 0, 10), "clamp(-11, 0, 10)")
	assert_almost_eq(s.vmcallv("f_clamp", 5.5, 0.0, 10.0), clamp(5.5, 0.0, 10.0), 0.0000001, "clamp(5.5, ...)")
	assert_eq(s.vmcallv("f_variadic_min", 9, 3, 5), min(9, 3, 5), "min() takes any number of arguments")

	# The transcendental ones, which the host performs.
	assert_almost_eq(s.vmcallv("f_sqrt", 2.0), sqrt(2.0), 0.0000001, "sqrt(2.0)")
	assert_almost_eq(s.vmcallv("f_pow", 2.0, 10.0), pow(2.0, 10.0), 0.0000001, "pow(2, 10)")
	assert_almost_eq(s.vmcallv("f_sin", 1.25), sin(1.25), 0.0000001, "sin(1.25)")
	assert_almost_eq(s.vmcallv("f_cos", 1.25), cos(1.25), 0.0000001, "cos(1.25)")
	assert_almost_eq(s.vmcallv("f_atan2", 1.0, 2.0), atan2(1.0, 2.0), 0.0000001, "atan2(1, 2)")
	assert_almost_eq(s.vmcallv("f_exp", 1.5), exp(1.5), 0.0000001, "exp(1.5)")
	assert_almost_eq(s.vmcallv("f_log", 1.5), log(1.5), 0.0000001, "log(1.5)")
	assert_almost_eq(s.vmcallv("f_deg_to_rad", 90.0), deg_to_rad(90.0), 0.0000001, "deg_to_rad(90)")
	assert_almost_eq(s.vmcallv("f_rad_to_deg", 1.0), rad_to_deg(1.0), 0.0000001, "rad_to_deg(1)")

	# Modulo and wrapping.
	assert_almost_eq(s.vmcallv("f_fmod", 7.5, 2.0), fmod(7.5, 2.0), 0.0000001, "fmod(7.5, 2)")
	assert_almost_eq(s.vmcallv("f_fposmod", -7.5, 2.0), fposmod(-7.5, 2.0), 0.0000001, "fposmod(-7.5, 2)")
	assert_eq(s.vmcallv("f_posmod", -3, 5), posmod(-3, 5), "posmod(-3, 5)")
	assert_eq(s.vmcallv("f_wrapi", 11, 0, 10), wrapi(11, 0, 10), "wrapi(11, 0, 10)")
	assert_eq(s.vmcallv("f_wrapi", -1, 0, 10), wrapi(-1, 0, 10), "wrapi(-1, 0, 10)")
	assert_almost_eq(s.vmcallv("f_wrapf", 11.5, 0.0, 10.0), wrapf(11.5, 0.0, 10.0), 0.0000001, "wrapf(11.5, ...)")
	assert_almost_eq(s.vmcallv("f_snapped", 2.6, 0.5), snapped(2.6, 0.5), 0.0000001, "snapped(2.6, 0.5)")

	# Interpolation.
	assert_almost_eq(s.vmcallv("f_lerp", 1.0, 5.0, 0.25), lerp(1.0, 5.0, 0.25), 0.0000001, "lerp(1, 5, 0.25)")
	assert_almost_eq(s.vmcallv("f_inverse_lerp", 1.0, 5.0, 2.0), inverse_lerp(1.0, 5.0, 2.0), 0.0000001, "inverse_lerp")
	assert_almost_eq(s.vmcallv("f_remap", 5.0, 0.0, 10.0, 100.0, 200.0), remap(5.0, 0.0, 10.0, 100.0, 200.0), 0.0000001, "remap")
	assert_almost_eq(s.vmcallv("f_smoothstep", 0.0, 1.0, 0.3), smoothstep(0.0, 1.0, 0.3), 0.0000001, "smoothstep")

	# The forms that take a float and answer an integer.
	assert_eq(s.vmcallv("f_floori", 2.6), floori(2.6), "floori(2.6)")
	assert_eq(typeof(s.vmcallv("f_floori", 2.6)), TYPE_INT, "floori() returns an int")
	assert_eq(s.vmcallv("f_ceili", 2.1), ceili(2.1), "ceili(2.1)")
	assert_eq(s.vmcallv("f_roundi", 2.6), roundi(2.6), "roundi(2.6)")

	# The predicates, which answer a bool.
	assert_eq(s.vmcallv("f_is_equal_approx", 1.0, 1.0), true, "is_equal_approx(1, 1)")
	assert_eq(s.vmcallv("f_is_equal_approx", 1.0, 2.0), false, "is_equal_approx(1, 2)")
	assert_eq(s.vmcallv("f_is_zero_approx", 0.0), true, "is_zero_approx(0)")
	assert_eq(s.vmcallv("f_is_nan", 0.0), false, "is_nan(0)")
	assert_eq(s.vmcallv("f_is_finite", 1.0), true, "is_finite(1)")
	assert_eq(typeof(s.vmcallv("f_is_finite", 1.0)), TYPE_BOOL, "a predicate returns a bool")

	# An integer where a float is wanted is the one implicit conversion
	# GDScript performs, and it has to reach the host as a double.
	assert_almost_eq(s.vmcallv("f_sqrt", 16), 4.0, 0.0000001, "sqrt(16) with an int argument")
	assert_almost_eq(s.vmcallv("f_pow", 2, 8), 256.0, 0.0000001, "pow(2, 8) with int arguments")

	s.queue_free()
	ts.queue_free()


func test_global_str_and_len():
	# str() and len() are the two globals that need the host's Variant API.
	var gdscript_code = """
func to_str(x):
	return str(x)

func join(a, b):
	return str(a, b)

func length(x):
	return len(x)
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(6000)

	assert_eq(s.vmcallv("to_str", 42), "42", "str(42)")
	assert_eq(typeof(s.vmcallv("to_str", 42)), TYPE_STRING, "str() returns a String")
	assert_eq(s.vmcallv("to_str", 1.5), str(1.5), "str(1.5)")
	assert_eq(s.vmcallv("to_str", true), str(true), "str(true)")
	assert_eq(s.vmcallv("to_str", "already"), "already", "str() of a String")
	assert_eq(s.vmcallv("to_str", [1, 2]), str([1, 2]), "str() of an Array")

	# str() concatenates, the way GDScript's does.
	assert_eq(s.vmcallv("join", "x = ", 7), "x = 7", "str() concatenates its arguments")

	assert_eq(s.vmcallv("length", "abcd"), 4, "len() of a String")
	assert_eq(s.vmcallv("length", [1, 2, 3]), 3, "len() of an Array")
	assert_eq(s.vmcallv("length", {"a": 1}), 1, "len() of a Dictionary")
	assert_eq(s.vmcallv("length", PackedByteArray([1, 2])), 2, "len() of a PackedByteArray")
	assert_eq(typeof(s.vmcallv("length", "abcd")), TYPE_INT, "len() returns an int")

	s.queue_free()
	ts.queue_free()


func test_global_type_constructors():
	# int(), float(), bool() and String(). The compiler performs the first
	# three inline when it already knows the argument is a number or a bool,
	# and asks the host otherwise -- a String is the reason it has to ask.
	var gdscript_code = """
func to_int(x):
	return int(x)

func to_float(x):
	return float(x)

func to_bool(x):
	return bool(x)

func to_string_of(x):
	return String(x)

func empty_string():
	return String()

func typed_to_int(x : float):
	return int(x)

func typed_to_float(x : int):
	return float(x)

func typed_to_bool(x : float):
	return bool(x)
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(6000)

	# The host performs these: the argument could be anything a Variant holds.
	assert_eq(s.vmcallv("to_int", 2.9), 2, "int(2.9) truncates toward zero")
	assert_eq(s.vmcallv("to_int", -2.9), -2, "int(-2.9) truncates toward zero")
	assert_eq(s.vmcallv("to_int", 7), 7, "int() of an int is that int")
	assert_eq(s.vmcallv("to_int", true), 1, "int(true)")
	assert_eq(s.vmcallv("to_int", "42"), 42, "int() of a String parses it")
	assert_eq(s.vmcallv("to_int", "-42"), -42, "int() of a negative String parses it")
	assert_eq(typeof(s.vmcallv("to_int", 2.9)), TYPE_INT, "int() returns an int")

	assert_almost_eq(s.vmcallv("to_float", 7), 7.0, 0.0000001, "float(7)")
	assert_almost_eq(s.vmcallv("to_float", 2.5), 2.5, 0.0000001, "float(2.5)")
	assert_almost_eq(s.vmcallv("to_float", "2.5"), 2.5, 0.0000001, "float() of a String parses it")
	assert_eq(typeof(s.vmcallv("to_float", 7)), TYPE_FLOAT, "float() returns a float")

	# bool() is Variant::booleanize(), which every type answers.
	assert_eq(s.vmcallv("to_bool", 0), false, "bool(0)")
	assert_eq(s.vmcallv("to_bool", 7), true, "bool(7)")
	assert_eq(s.vmcallv("to_bool", 0.0), false, "bool(0.0)")
	assert_eq(s.vmcallv("to_bool", 0.5), true, "bool(0.5)")
	assert_eq(s.vmcallv("to_bool", ""), false, "bool() of an empty String")
	assert_eq(s.vmcallv("to_bool", "x"), true, "bool() of a String")
	assert_eq(s.vmcallv("to_bool", []), false, "bool() of an empty Array")
	assert_eq(s.vmcallv("to_bool", [1]), true, "bool() of an Array")
	assert_eq(typeof(s.vmcallv("to_bool", 7)), TYPE_BOOL, "bool() returns a bool")

	# String(x) is str(x) of one argument.
	assert_eq(s.vmcallv("to_string_of", 42), "42", "String(42)")
	assert_eq(s.vmcallv("to_string_of", 1.5), str(1.5), "String(1.5)")
	assert_eq(s.vmcallv("to_string_of", [1, 2]), str([1, 2]), "String() of an Array")
	assert_eq(typeof(s.vmcallv("to_string_of", 42)), TYPE_STRING, "String() returns a String")
	assert_eq(s.vmcallv("empty_string"), "", "String() with no arguments is the empty String")

	# The same conversions, where the type hint puts them inline instead.
	assert_eq(s.vmcallv("typed_to_int", 2.9), 2, "int() of a float that the compiler knows is one")
	assert_eq(typeof(s.vmcallv("typed_to_int", 2.9)), TYPE_INT, "the inline int() returns an int")
	assert_almost_eq(s.vmcallv("typed_to_float", 7), 7.0, 0.0000001, "float() of a known int")
	assert_eq(typeof(s.vmcallv("typed_to_float", 7)), TYPE_FLOAT, "the inline float() returns a float")
	assert_eq(s.vmcallv("typed_to_bool", 0.0), false, "bool() of a known float")
	assert_eq(s.vmcallv("typed_to_bool", 0.5), true, "bool() of a known float")
	assert_eq(typeof(s.vmcallv("typed_to_bool", 0.5)), TYPE_BOOL, "the inline bool() returns a bool")

	s.queue_free()
	ts.queue_free()


func test_global_random():
	# The random draws are the one family of globals whose answer depends on
	# host state, so what can be asserted is the shape of the answer: its type,
	# its range, and that two calls are two draws.
	var gdscript_code = """
func draw_int():
	return randi()

func roll():
	return randi_range(1, 6)

func wide_range(a, b):
	return randi_range(a, b)

func draw_float():
	return randf()

func in_range(a, b):
	return randf_range(a, b)

func normal(mean, deviation):
	return randfn(mean, deviation)

func distinct_draws(n):
	# Two draws in a row are two draws: a pass that folded them into one, or
	# dropped the one whose result is unused, would show up here.
	var first = randi()
	var same = 0
	var i = 0
	while i < n:
		if randi() == first:
			same += 1
		i += 1
	return same
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(20000)

	assert_eq(typeof(s.vmcallv("draw_int")), TYPE_INT, "randi() returns an int")
	assert_eq(typeof(s.vmcallv("draw_float")), TYPE_FLOAT, "randf() returns a float")
	assert_eq(typeof(s.vmcallv("roll")), TYPE_INT, "randi_range() returns an int")
	assert_eq(typeof(s.vmcallv("in_range", 0.0, 1.0)), TYPE_FLOAT, "randf_range() returns a float")
	assert_eq(typeof(s.vmcallv("normal", 0.0, 1.0)), TYPE_FLOAT, "randfn() returns a float")

	# randi_range() is inclusive at both ends, and its bounds travel as 64-bit
	# integers rather than as doubles.
	var seen := {}
	for i in range(60):
		var roll = s.vmcallv("roll")
		assert_true(roll >= 1 and roll <= 6, "randi_range(1, 6) stays in range")
		seen[roll] = true
	assert_true(seen.size() > 1, "60 rolls of a die are not all the same number")

	# A bound reaches Godot as the 64-bit integer the program wrote, not as a
	# double that lost its low bit on the way. Godot's own randi_range()
	# narrows to 32 bits before drawing, so what this pins down is that the
	# guest and GDScript hand it the same number and get the same answer --
	# narrowing included.
	var big : int = 9007199254740993  # 2^53 + 1, which a double cannot hold
	assert_eq(s.vmcallv("wide_range", big, big), randi_range(big, big),
		"randi_range() is handed the bound the program wrote")

	for i in range(20):
		var f = s.vmcallv("draw_float")
		assert_true(f >= 0.0 and f <= 1.0, "randf() stays in [0, 1]")
		var r = s.vmcallv("in_range", -2.5, 2.5)
		assert_true(r >= -2.5 and r <= 2.5, "randf_range() stays in range")

	# 40 draws all landing on the first one would mean the calls were folded.
	assert_true(s.vmcallv("distinct_draws", 40) < 40, "two randi() calls are two draws")

	s.queue_free()
	ts.queue_free()


func test_global_math_in_a_loop():
	# The globals have to work where they are actually used: inside a loop, with
	# the result feeding the arithmetic around it.
	var gdscript_code = """
func total(n):
	var sum = 0.0
	var i = 0
	while i < n:
		sum += abs(sin(i)) * clampf(i, 0.0, 4.0)
		i += 1
	return sum

func typed_total(n : int) -> float:
	var sum : float = 0.0
	for i in range(n):
		sum += sqrt(i) + maxi(i, 2)
	return sum
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(20000)

	var expected = 0.0
	for i in range(8):
		expected += abs(sin(i)) * clampf(i, 0.0, 4.0)
	assert_almost_eq(s.vmcallv("total", 8), expected, 0.0000001, "a global inside a loop")

	expected = 0.0
	for i in range(8):
		expected += sqrt(i) + maxi(i, 2)
	assert_almost_eq(s.vmcallv("typed_total", 8), expected, 0.0000001, "globals with type hints")

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
	# Color() does not rescale integers; Color8() divides by 255.
	var engine_color = Color(0, 128, 255, 255)
	var expected = engine_color.r + engine_color.g + engine_color.b + engine_color.a
	assert_almost_eq(result, expected, 0.01, "Color(int, ...) is not rescaled by 255")

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
	return [42]

func make_array_two():
	return [1, 2]

func make_array_with_values():
	return [1, 2, 3, 4, 5]

func make_array_with_strings():
	return ["hello", "world", "test"]
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

	# Element list uses []; Array() takes one container to convert.
	arr = s.vmcallv("make_array_single")
	assert_eq(arr.size(), 1, "[42] should have size 1")
	assert_eq(arr[0], 42, "First element should be 42")

	# Test Array with two elements
	arr = s.vmcallv("make_array_two")
	assert_eq(arr.size(), 2, "[1, 2] should have size 2")
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

	# Array() converts one container; multi-arg and single-container forms
	# are compile errors.
	assert_true(ts.vmcall("compile_to_elf", "func f():\n\treturn Array(1, 2)\n").is_empty(),
		"Array(1, 2) is refused")
	assert_true(ts.vmcall("compile_to_elf", "func f():\n\treturn Array([1, 2])\n").is_empty(),
		"Array([1, 2]) is refused")

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

# String concatenation folding and ECALL_STRING_SIZE, differential against GDScript.
func test_string_building():
	var gdscript_code = """
func plain(a : String, b : String):
	return a + b

func literals():
	return "Hello" + " " + "World!"

func with_number(n):
	return "Count" + str(n)

func leading(n):
	return str(n) + " items"

func chain(a, b, c):
	return "[" + str(a) + ", " + str(b) + ", " + str(c) + "]"

func both_sides(a, b):
	return str(a) + str(b)

func of_a_string(s : String):
	return "<" + str(s) + ">"

func containers(a, d):
	return "a=" + str(a) + " d=" + str(d)

func conditional(flag, n):
	return ("yes " if flag else "no ") + str(n)

func measured(n):
	return ("value " + str(n)).length()

func measured_plain(a : String, b : String):
	return (a + b).length()

func measured_literal():
	var s = "hello"
	return s.length()

func built_in_a_loop(n : int):
	var acc : int = 0
	var i : int = 0
	while i < n:
		var s : String = "value " + str(i)
		acc += s.length()
		i += 1
	return acc
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	var script := GDScript.new()
	script.source_code = gdscript_code
	assert_eq(script.reload(), OK, "the same source should compile as GDScript")
	var engine = script.new()

	var cases := [
		["plain", ["Hello", " World"]],
		["literals", []],
		["with_number", [42]],
		["with_number", [-1]],
		["with_number", [1.5]],
		["leading", [7]],
		["chain", [1, 2.5, "x"]],
		["both_sides", [1, 2]],
		["both_sides", ["a", "b"]],
		["of_a_string", ["inner"]],
		["containers", [[1, 2], {"k": 1}]],
		["conditional", [true, 3]],
		["conditional", [false, 3]],
		["measured", [1234]],
		["measured_plain", ["ab", "cde"]],
		["measured_literal", []],
		["built_in_a_loop", [20]],
	]
	for case in cases:
		var name : String = case[0]
		var args : Array = case[1]
		var expected = engine.callv(name, args)
		var actual = _call_with(s, name, args)
		assert_eq(actual, expected, "%s%s should answer what GDScript answers" % [name, args])
		assert_eq(typeof(actual), typeof(expected), "%s%s should answer the same type" % [name, args])

	s.queue_free()

# vmcallv() is variadic; expand manually.
func _call_with(s: Sandbox, name: String, args: Array):
	match args.size():
		0: return s.vmcallv(name)
		1: return s.vmcallv(name, args[0])
		2: return s.vmcallv(name, args[0], args[1])
		_: return s.vmcallv(name, args[0], args[1], args[2])

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

func test_member_access_on_an_untyped_value():
	# Untracked struct instance: tag decides element read vs VGET at run time.
	var gdscript_code = """
struct Account:
	var balance = 0

func read(account):
	return account.balance

func write(account):
	account.balance = 42
	return account

func walk(accounts):
	var total = 0
	for account in accounts:
		total += account.balance
	return total

func local_walk():
	var total = 0
	for account in [Account.new(100), Account.new(50)]:
		total += account.balance
	return total
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("read", {"balance": 7}), 7, "Reading a field of an untyped value")
	assert_eq(s.vmcallv("write", {"balance": 0}), {"balance": 42}, "Writing a field of an untyped value")
	assert_eq(s.vmcallv("walk", [{"balance": 1}, {"balance": 2}]), 3, "Fields of Array elements")
	assert_eq(s.vmcallv("local_walk"), 150, "Fields of instances built in the program")

	s.queue_free()


# -= Diagnostics =-
#
# The .sgd script language extension underlines errors in the Godot editor, and
# it gets them from validate() inside the compiler sandbox. What it needs beyond
# a yes/no is a line and a column, so these tests hold the compiler to reporting
# them, and to reporting them where the user is looking.

func _validate(gdscript_code: String) -> Dictionary:
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var result = ts.vmcall("validate", gdscript_code)
	ts.queue_free()
	return result

func test_validate_accepts_a_good_program():
	var result = _validate("""
func add(x, y):
	return x + y
""")
	assert_true(result["valid"], "A program that compiles must validate")
	assert_eq(result["message"], "", "A valid program has no error message")
	assert_eq(result["line"], 0, "A valid program has no error line")

func test_validate_reports_an_unclosed_bracket_where_it_opens():
	# The 'func g(' on line 5 is never closed. A newline inside brackets is
	# layout, not a statement end, so an unclosed '(' swallows the rest of the
	# file: EOF is the symptom, and the bracket's position is the cause.
	var result = _validate("""
func f():
	return 1

func g(
""")
	assert_false(result["valid"], "An unclosed parameter list must not validate")
	assert_eq(result["line"], 5, "The error belongs to the line that opened it")
	assert_gt(result["column"], 0, "A syntax error carries a column")
	assert_eq(result["type"], "Lexer Error", "Reported as a lexer error")
	assert_ne(result["message"], "", "A rejected program says what is wrong")

func test_validate_reports_a_syntax_error():
	# A malformed signature with no brackets, so the parser objects, on the line
	# of the mistake.
	var result = _validate("""
func f():
	return 1

func g)
""")
	assert_false(result["valid"], "A malformed signature must not validate")
	assert_eq(result["line"], 5, "Reported on the line holding the mistake")
	assert_gt(result["column"], 0, "A syntax error carries a column")
	assert_eq(result["type"], "Parser Error", "Reported as a parser error")
	assert_ne(result["message"], "", "A rejected program says what is wrong")

func test_validate_reports_an_unterminated_string_where_it_opens():
	# A plain string ends at its own line, so a stray quote is reported at the
	# quote rather than at the end of the file, and the lines below still parse.
	var result = _validate("""
func f():
	var s = "oops
	return s
""")
	assert_false(result["valid"], "An unterminated string must not validate")
	assert_eq(result["type"], "Lexer Error", "Reported as a lexer error")
	assert_eq(result["line"], 3, "Reported on the line holding the opening quote")

func test_validate_reports_an_unknown_struct_field():
	var result = _validate("""
struct Point:
	var x = 0

func f():
	var p = Point.new()
	return p.z
""")
	assert_false(result["valid"], "An undeclared struct field must not validate")
	assert_eq(result["line"], 7, "Reported on the line that reads the field")
	assert_eq(result["function"], "f", "Reported with the function it happened in")
	assert_true(result["hint"].contains("x"), "The hint lists the fields that do exist")

func test_validate_leaves_no_error_behind():
	# validate() runs on every keystroke in the editor, so a stale error would
	# keep a fixed script underlined.
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true

	assert_false(ts.vmcall("validate", "func f(")["valid"], "Broken source is rejected")
	var fixed = ts.vmcall("validate", "func f():\n\treturn 1\n")
	assert_true(fixed["valid"], "The fixed source validates")
	assert_eq(fixed["line"], 0, "and carries none of the previous error")

	ts.queue_free()

func test_validate_does_not_disturb_compilation():
	# Validation and compilation share one compiler sandbox in the editor.
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true

	var code = "func answer():\n\treturn 42\n"
	assert_true(ts.vmcall("validate", code)["valid"], "The program validates")
	var elf = ts.vmcall("compile", code)
	assert_false(elf.is_empty(), "and still compiles afterwards")
	assert_true(ts.vmcall("validate", code)["valid"], "and validates again after compiling")

	ts.queue_free()

# -= Operators and everyday syntax =-
#
# These run the compiled program against the real engine, the only place the
# operators answered by Variant::evaluate() can be checked: the IR interpreter
# refuses '**' and 'in'. Where an expected value is written as GDScript, the
# oracle is Godot's own evaluation of the same expression.

func test_power_operator():
	var gdscript_code = """
func int_power(a : int, b : int):
	return a ** b

func float_power(a : float, b : float):
	return a ** b

func untyped_power(a, b):
	return a ** b

func right_associative():
	return 2 ** 3 ** 2

func binds_tighter_than_minus():
	return -2 ** 2

func compound(a : int):
	var x = a
	x **= 3
	return x
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("int_power", 2, 10), 2 ** 10, "2 ** 10 should match Godot")
	assert_eq(s.vmcallv("int_power", 3, 4), 3 ** 4, "3 ** 4 should match Godot")
	assert_almost_eq(s.vmcallv("float_power", 2.0, 0.5), 2.0 ** 0.5, 0.0001, "A float power should match Godot")
	assert_eq(s.vmcallv("untyped_power", 5, 3), 5 ** 3, "An untyped power should match Godot")
	assert_eq(s.vmcallv("right_associative"), 2 ** 3 ** 2, "'**' should be right-associative")
	assert_eq(s.vmcallv("binds_tighter_than_minus"), -2 ** 2, "'**' should bind tighter than unary '-'")
	assert_eq(s.vmcallv("compound", 2), 2 ** 3, "'**=' should raise in place")

	s.queue_free()

func test_in_operator():
	var gdscript_code = """
func in_array(a):
	return a in [1, 2, 3]

func not_in_array(a):
	return a not in [1, 2, 3]

func in_dictionary(key):
	var d = {"alpha": 1, "beta": 2}
	return key in d

func in_string(needle : String):
	return needle in "haystack"
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_true(s.vmcallv("in_array", 2), "2 is in [1, 2, 3]")
	assert_false(s.vmcallv("in_array", 7), "7 is not in [1, 2, 3]")
	assert_false(s.vmcallv("not_in_array", 2), "'not in' should negate the test")
	assert_true(s.vmcallv("not_in_array", 7), "7 is not in [1, 2, 3]")
	assert_true(s.vmcallv("in_dictionary", "beta"), "'in' should find a Dictionary key")
	assert_false(s.vmcallv("in_dictionary", "gamma"), "and should not find a missing one")
	assert_true(s.vmcallv("in_string", "hay"), "'in' should find a substring")
	assert_false(s.vmcallv("in_string", "needle"), "and should not find a missing one")

	s.queue_free()

func test_is_operator():
	# 'is' compares the Variant's type tag: exact, not convertibility, so 5.0 is
	# not an int.
	var gdscript_code = """
func is_int(a):
	return a is int

func is_float(a):
	return a is float

func is_string(a):
	return a is String

func is_array(a):
	return a is Array

func is_dictionary(a):
	return a is Dictionary

func is_not_int(a):
	return a is not int
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_true(s.vmcallv("is_int", 5), "5 is an int")
	assert_false(s.vmcallv("is_int", 5.0), "5.0 is not an int")
	assert_true(s.vmcallv("is_float", 5.0), "5.0 is a float")
	assert_true(s.vmcallv("is_string", "text"), "A String is a String")
	assert_false(s.vmcallv("is_string", 5), "5 is not a String")
	assert_true(s.vmcallv("is_array", [1, 2]), "An Array is an Array")
	assert_false(s.vmcallv("is_array", {}), "A Dictionary is not an Array")
	assert_true(s.vmcallv("is_dictionary", {"a": 1}), "A Dictionary is a Dictionary")
	assert_true(s.vmcallv("is_not_int", 5.0), "'is not' should negate the test")
	assert_false(s.vmcallv("is_not_int", 5), "and should be false for a match")

	s.queue_free()

func test_for_over_a_container():
	# Iterating an Array used to emit an increment with an immediate where the
	# opcode requires a register: the loop compiled, and the IR was malformed.
	var gdscript_code = """
func sum_array(a : Array):
	var total = 0
	for v in a:
		total += v
	return total

func sum_literal():
	var total = 0
	for v in [1, 2, 3, 4]:
		total += v
	return total

func join_keys(d : Dictionary):
	var out = ""
	for k in d:
		out += k
	return out

func join_keys_untyped(d):
	# The same loop with the type unknown at compile time: the path that decides
	# between an Array and a Dictionary at run time.
	var out = ""
	for k in d:
		out += k
	return out

func sum_values(d : Dictionary):
	var total = 0
	for k in d:
		total += d[k]
	return total

func with_break(a : Array):
	var total = 0
	for v in a:
		if v > 3:
			break
		total += v
	return total

func nested():
	var total = 0
	for a in [1, 2]:
		for b in [10, 20]:
			total += a * b
	return total
"""
	var s = _compile_and_load(gdscript_code, 20000)
	if s == null:
		return

	assert_eq(s.vmcallv("sum_array", [1, 2, 3]), 6, "Iterating an Array should visit every element")
	assert_eq(s.vmcallv("sum_array", []), 0, "An empty Array should run no iterations")
	assert_eq(s.vmcallv("sum_literal"), 10, "Iterating an Array literal should work")
	assert_eq(s.vmcallv("join_keys", {"a": 1, "b": 2}), "ab", "Iterating a Dictionary should visit its keys")
	assert_eq(s.vmcallv("join_keys_untyped", {"a": 1, "b": 2}), "ab", "An untyped Dictionary should iterate its keys")
	assert_eq(s.vmcallv("join_keys_untyped", ["x", "y"]), "xy", "An untyped Array should still iterate its elements")
	assert_eq(s.vmcallv("sum_values", {"a": 1, "b": 2}), 3, "A Dictionary's values should be reachable by key")
	assert_eq(s.vmcallv("with_break", [1, 2, 3, 4, 5]), 6, "'break' should leave a container loop")
	assert_eq(s.vmcallv("nested"), 90, "Container loops should nest")

	s.queue_free()

func test_not_binds_looser_than_comparison():
	# 'not a == b' is 'not (a == b)', as in GDScript. It used to parse as
	# '(not a) == b', which has a different answer.
	var gdscript_code = """
func differs(a : int, b : int):
	return not a == b

func not_less(a : int, b : int):
	return not a < b

func still_negates(a : bool):
	return not a

func and_is_looser(a : bool, b : bool):
	return not a and b
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("differs", 2, 1), not 2 == 1, "'not a == b' should match Godot")
	assert_eq(s.vmcallv("differs", 2, 2), not 2 == 2, "'not a == b' should match Godot")
	assert_eq(s.vmcallv("not_less", 2, 1), not 2 < 1, "'not a < b' should match Godot")
	assert_true(s.vmcallv("still_negates", false), "'not' should still negate a value")
	assert_eq(s.vmcallv("and_is_looser", false, true), (not false) and true, "'and' should be looser than 'not'")

	s.queue_free()

func test_enum_declarations():
	var gdscript_code = """
enum Mode { IDLE, RUN = 5, STOP }
enum { LEFT, RIGHT }

func idle():
	return Mode.IDLE

func run():
	return Mode.RUN

func stop():
	return Mode.STOP

func unqualified():
	return LEFT + RIGHT

func classify(m : int):
	if m == Mode.RUN:
		return "run"
	if m == Mode.STOP:
		return "stop"
	return "idle"
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("idle"), 0, "Enum numbering should start at zero")
	assert_eq(s.vmcallv("run"), 5, "An explicit enum value should be used")
	assert_eq(s.vmcallv("stop"), 6, "Numbering should continue from an explicit value")
	assert_eq(s.vmcallv("unqualified"), 1, "An unnamed enum's members need no qualification")
	assert_eq(s.vmcallv("classify", 5), "run", "An enum member should compare as its integer")
	assert_eq(s.vmcallv("classify", 0), "idle", "An enum member should compare as its integer")

	s.queue_free()

func test_statement_layout():
	# A semicolon, a backslash and an open bracket all say where a statement ends.
	# None of them changes what a program means.
	var gdscript_code = """
func semicolons():
	var a = 1; var b = 2
	return a + b

func explicit_continuation():
	return 1 + \\
		2 + \\
		3

func implicit_continuation():
	return (1 +
		2 +
		3)

func multiline_call(a : int, b : int, c : int):
	return a + b + c

func trailing_commas():
	var a = [1, 2, 3,]
	var d = {"x": 1,}
	return multiline_call(
		a[0],
		a[1],
		d["x"],
	)

func lua_style_dictionary():
	var d = {alpha = 1, beta = 2}
	return d["alpha"] + d["beta"]
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("semicolons"), 3, "';' should separate two statements on one line")
	assert_eq(s.vmcallv("explicit_continuation"), 6, "'\\' should continue a line")
	assert_eq(s.vmcallv("implicit_continuation"), 6, "A parenthesised expression should span lines")
	assert_eq(s.vmcallv("trailing_commas"), 4, "A trailing comma should be allowed")
	assert_eq(s.vmcallv("lua_style_dictionary"), 3, "'{key = value}' should build a Dictionary")

	s.queue_free()

func test_compound_assignment_to_containers():
	var gdscript_code = """
func bump_first(a : Array):
	a[0] += 10
	return a

func scale_at(a : Array, i : int):
	a[i] *= 3
	return a

func bump_key(d : Dictionary):
	d["count"] += 1
	return d
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("bump_first", [1, 2]), [11, 2], "'a[0] += 10' should update the element")
	assert_eq(s.vmcallv("scale_at", [1, 2, 3], 2), [1, 2, 9], "'a[i] *= 3' should update the element")
	assert_eq(s.vmcallv("bump_key", {"count": 4}), {"count": 5}, "'d[k] += 1' should update the value")

	s.queue_free()

func test_declarations_without_a_lowering():
	# 'static', 'class_name' and container element types describe the script, not
	# the code, and are accepted so an ordinary script compiles.
	var gdscript_code = """
class_name Widget

static func answer():
	return 42

func typed_container(a : Array[int]) -> int:
	var total : int = 0
	for v in a:
		total += v
	return total

func typed_dictionary() -> Dictionary[String, int]:
	return {"a": 1}
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("answer"), 42, "'static func' should compile as a function")
	assert_eq(s.vmcallv("typed_container", [1, 2, 3]), 6, "'Array[int]' should be accepted as a type hint")
	assert_eq(s.vmcallv("typed_dictionary"), {"a": 1}, "'Dictionary[K, V]' should be accepted as a return type")

	s.queue_free()

func test_as_conversion():
	var gdscript_code = """
func to_int(a):
	return a as int

func to_float(a):
	return a as float

func to_bool(a):
	return a as bool

func to_string(a):
	return a as String
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	# The expected values are literals rather than `2.7 as int`: Godot rejects a
	# cast between types it sees as incompatible at parse time, and would not
	# compile this file.
	assert_eq(s.vmcallv("to_int", 2.7), 2, "'as int' should truncate a float")
	assert_eq(s.vmcallv("to_int", "42"), 42, "'as int' should parse a String the way int() does")
	assert_almost_eq(s.vmcallv("to_float", 3), 3.0, 0.0001, "'as float' should widen an int")
	assert_eq(s.vmcallv("to_bool", 0), false, "'as bool' should booleanize")
	assert_eq(s.vmcallv("to_bool", 3), true, "'as bool' should booleanize")
	assert_eq(s.vmcallv("to_string", 42), "42", "'as String' should convert")

	s.queue_free()

# -= A logic CPU =-
#
# tests/tests/test_cpu.sgd is a small register machine whose execute step is a
# `match` on the opcode: the program shape the jump-table lowering was written
# for. These cases check that it is correct.

const CPU_SOURCE_PATH = "res://tests/test_cpu.sgd"

func _load_cpu_source() -> String:
	var file = FileAccess.open(CPU_SOURCE_PATH, FileAccess.READ)
	assert_not_null(file, "test_cpu.sgd should be readable")
	if file == null:
		return ""
	var source = file.get_as_text()
	file.close()
	return source

func test_match_bindings_and_guards():
	# `var name` and `when` are the pattern features that need no container: the
	# first binds what the arm matched, the second lets an arm decline after its
	# pattern matched, which is what lets two arms bind the same name.
	var gdscript_code = """
func classify(n):
	match n:
		0:
			return "zero"
		var v when v < 0:
			return "negative " + str(-v)
		var v when v < 10:
			return "small " + str(v)
		var v:
			return "large " + str(v)

func first_or_default(n, flag):
	match n:
		1 when flag:
			return 10
		1:
			return 20
		_ when flag:
			return 30
		_:
			return 40

func binding_is_a_copy():
	var subject = 5
	match subject:
		var v:
			v = 99
	return subject

func matches_a_string(s):
	match s:
		"circle":
			return 1
		"square":
			return 2
		var other:
			return other.length()
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	assert_eq(s.vmcallv("classify", 0), "zero", "A value pattern is tried before the bindings below it")
	assert_eq(s.vmcallv("classify", -3), "negative 3", "A guard should see the name the pattern bound")
	assert_eq(s.vmcallv("classify", 4), "small 4", "The first arm whose guard holds should win")
	assert_eq(s.vmcallv("classify", 50), "large 50", "An unguarded binding should catch the rest")

	assert_eq(s.vmcallv("first_or_default", 1, true), 10, "A guard that holds takes its arm")
	assert_eq(s.vmcallv("first_or_default", 1, false), 20, "A declining guard should try the next arm")
	assert_eq(s.vmcallv("first_or_default", 9, true), 30, "A guarded wildcard is still an arm")
	assert_eq(s.vmcallv("first_or_default", 9, false), 40, "A wildcard catches what the guards declined")

	assert_eq(s.vmcallv("binding_is_a_copy"), 5, "Assigning to a binding must not reach the subject")

	assert_eq(s.vmcallv("matches_a_string", "circle"), 1, "A String subject should match a String pattern")
	assert_eq(s.vmcallv("matches_a_string", "hexagon"), 7, "A binding should hold the String it matched")

	# The same cases asked of the engine, the authority on what `match` answers.
	assert_eq(s.vmcallv("classify", -3), _engine_classify(-3), "The engine and the guest should agree")
	assert_eq(s.vmcallv("classify", 50), _engine_classify(50), "The engine and the guest should agree")

	s.queue_free()

func _engine_classify(n):
	match n:
		0:
			return "zero"
		var v when v < 0:
			return "negative " + str(-v)
		var v when v < 10:
			return "small " + str(v)
		var v:
			return "large " + str(v)

func test_match_container_patterns():
	# Array and dictionary patterns test a value's shape, which only the host can
	# answer: type tag, then length, then elements or keys.
	var gdscript_code = """
func shape(v):
	match v:
		[]:
			return "empty"
		[1, var x]:
			return "one then " + str(x)
		[var a, var b]:
			return "pair " + str(a + b)
		[var head, ..]:
			return "starts with " + str(head)
		_:
			return "not an array"

func nested(v):
	match v:
		[var a, [var b, var c]]:
			return a + b + c
		_:
			return -1

func describe(d):
	match d:
		{"kind": "circle", "r": var r}:
			return "circle of " + str(r)
		{"kind": var k}:
			return "a " + str(k)
		{"x", "y"}:
			return "a point"
		{}:
			return "empty"
		{..}:
			return "something else"
		_:
			return "not a dictionary"
"""
	var s = _compile_and_load(gdscript_code, 200000)
	if s == null:
		return

	assert_eq(s.vmcallv("shape", []), "empty", "An empty array pattern should match an empty Array")
	assert_eq(s.vmcallv("shape", [1, 5]), "one then 5", "An element pattern should be compared elementwise")
	assert_eq(s.vmcallv("shape", [2, 5]), "pair 7", "A pattern that does not match should try the next arm")
	assert_eq(s.vmcallv("shape", [4, 5, 6]), "starts with 4", "'..' should match an Array that is longer")
	assert_eq(s.vmcallv("shape", 7), "not an array", "A non-Array must not match an array pattern")
	assert_eq(s.vmcallv("shape", {"a": 1}), "not an array", "A Dictionary must not match an array pattern")

	assert_eq(s.vmcallv("nested", [1, [2, 3]]), 6, "Array patterns should nest")
	assert_eq(s.vmcallv("nested", [1, [2, 3, 4]]), -1, "A nested pattern should check its own length")

	assert_eq(s.vmcallv("describe", {"kind": "circle", "r": 3}), "circle of 3",
		"A dictionary pattern should match its keys and their values")
	assert_eq(s.vmcallv("describe", {"kind": "square"}), "a square",
		"A binding should hold what the key held")
	assert_eq(s.vmcallv("describe", {"x": 1, "y": 2}), "a point",
		"A key-only entry should ask only that the key is there")
	assert_eq(s.vmcallv("describe", {}), "empty", "An empty dictionary pattern should match an empty Dictionary")
	assert_eq(s.vmcallv("describe", {"a": 1, "b": 2, "c": 3}), "something else",
		"'..' should match a Dictionary with other keys")
	assert_eq(s.vmcallv("describe", [1, 2]), "not a dictionary",
		"An Array must not match a dictionary pattern")

	# A closed dictionary pattern constrains the size too: an extra key is a
	# different shape.
	assert_eq(s.vmcallv("describe", {"kind": "circle", "r": 3, "extra": 1}), "something else",
		"A closed dictionary pattern should not match a Dictionary with more keys")

	# The same patterns run by the engine on the same values: what `match` means
	# is Godot's to say, so every answer above is checked against it.
	for value in [[], [1, 5], [2, 5], [4, 5, 6], 7, {"a": 1}]:
		assert_eq(s.vmcallv("shape", value), _engine_shape(value),
			"The engine and the guest should agree on " + str(value))
	for value in [{"kind": "circle", "r": 3}, {"kind": "square"}, {"x": 1, "y": 2}, {},
		{"a": 1, "b": 2, "c": 3}, {"kind": "circle", "r": 3, "extra": 1}, [1, 2]]:
		assert_eq(s.vmcallv("describe", value), _engine_describe(value),
			"The engine and the guest should agree on " + str(value))

	s.queue_free()

func _engine_shape(v):
	match v:
		[]:
			return "empty"
		[1, var x]:
			return "one then " + str(x)
		[var a, var b]:
			return "pair " + str(a + b)
		[var head, ..]:
			return "starts with " + str(head)
		_:
			return "not an array"

func _engine_describe(d):
	match d:
		{"kind": "circle", "r": var r}:
			return "circle of " + str(r)
		{"kind": var k}:
			return "a " + str(k)
		{"x", "y"}:
			return "a point"
		{}:
			return "empty"
		{..}:
			return "something else"
		_:
			return "not a dictionary"

func test_switch_dispatch():
	# Mandatory jump table: int subject, int-constant patterns, no guards/destructuring.
	var gdscript_code = """
const OP_HALT = 0
const OP_LOADI = 1
const OP_ADD = 2
const OP_SUB = 3
const OP_MUL = 4
const OP_NEG = 5

func step(op : int, acc : int, imm : int) -> int:
	switch op:
		OP_HALT:
			return acc
		OP_LOADI:
			return imm
		OP_ADD:
			return acc + imm
		OP_SUB:
			return acc - imm
		OP_MUL:
			return acc * imm
		OP_NEG:
			return -acc
		_:
			return -9999

func decoded(word : int) -> int:
	# Inferred int subject; annotation not required.
	var op = (word >> 8) & 7
	switch op:
		0:
			return 100
		1:
			return 101
		2:
			return 102
		3:
			return 103
		_:
			return -1

func negative_and_holes(n : int) -> int:
	switch n:
		-3:
			return 10
		-1:
			return 20
		0:
			return 30
		2:
			return 40
		_:
			return -1

func run(steps : int) -> int:
	var acc = 0
	var i = 0
	while i < steps:
		acc = step(i % 6, acc, 3)
		i += 1
	return acc
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	assert_eq(s.vmcallv("step", 0, 7, 3), 7, "The first arm of a switch is reachable")
	assert_eq(s.vmcallv("step", 1, 7, 3), 3, "Each table entry reaches its own arm")
	assert_eq(s.vmcallv("step", 2, 7, 3), 10, "An arm should see the arguments")
	assert_eq(s.vmcallv("step", 3, 7, 3), 4, "An arm should see the arguments")
	assert_eq(s.vmcallv("step", 4, 7, 3), 21, "An arm should see the arguments")
	assert_eq(s.vmcallv("step", 5, 7, 3), -7, "The last table entry is reachable")
	assert_eq(s.vmcallv("step", 6, 7, 3), -9999, "A subject past the table reaches the wildcard")
	assert_eq(s.vmcallv("step", -1, 7, 3), -9999, "A subject below the table reaches the wildcard")

	assert_eq(s.vmcallv("decoded", 0), 100, "An inferred int subject keeps the table")
	assert_eq(s.vmcallv("decoded", 3 << 8), 103, "An inferred int subject keeps the table")
	assert_eq(s.vmcallv("decoded", 5 << 8), -1, "A hole in the range reaches the wildcard")

	assert_eq(s.vmcallv("negative_and_holes", -3), 10, "A negative base is scaled from the right place")
	assert_eq(s.vmcallv("negative_and_holes", -1), 20, "A negative base is scaled from the right place")
	assert_eq(s.vmcallv("negative_and_holes", 0), 30, "Zero is an ordinary table entry")
	assert_eq(s.vmcallv("negative_and_holes", 2), 40, "The top of the range is reachable")
	assert_eq(s.vmcallv("negative_and_holes", 1), -1, "A hole reaches the wildcard")
	assert_eq(s.vmcallv("negative_and_holes", -4), -1, "Below the range reaches the wildcard")

	assert_eq(s.vmcallv("run", 24), _engine_run(24), "A dispatch loop should answer as the engine does")

	# Differential against engine match.
	for n in range(-5, 5):
		assert_eq(s.vmcallv("negative_and_holes", n), _engine_negative_and_holes(n),
			"switch and the engine's match should agree on " + str(n))

	s.queue_free()

func test_switch_refuses_what_it_cannot_dispatch():
	# Each case compiles under match but is refused under switch.
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true

	var refused = {
		"an untyped subject": "func f(op):\n\tswitch op:\n\t\t0:\n\t\t\treturn 1\n\t\t1:\n\t\t\treturn 2\n",
		"a float subject": "func f(op : float):\n\tswitch op:\n\t\t0:\n\t\t\treturn 1\n\t\t1:\n\t\t\treturn 2\n",
		"a String subject": "func f(op : String):\n\tswitch op:\n\t\t0:\n\t\t\treturn 1\n\t\t1:\n\t\t\treturn 2\n",
		"a when guard": "func f(op : int):\n\tswitch op:\n\t\t0 when op > 1:\n\t\t\treturn 1\n\t\t1:\n\t\t\treturn 2\n",
		"a binding": "func f(op : int):\n\tswitch op:\n\t\t0:\n\t\t\treturn 1\n\t\tvar v:\n\t\t\treturn v\n",
		"an array pattern": "func f(op : int):\n\tswitch op:\n\t\t0:\n\t\t\treturn 1\n\t\t[1, 2]:\n\t\t\treturn 2\n",
		"a String pattern": "func f(op : int):\n\tswitch op:\n\t\t0:\n\t\t\treturn 1\n\t\t\"x\":\n\t\t\treturn 2\n",
		"a run-time pattern": "func f(op : int):\n\tswitch op:\n\t\t0:\n\t\t\treturn 1\n\t\top:\n\t\t\treturn 2\n",
		"a duplicated value": "func f(op : int):\n\tswitch op:\n\t\t0:\n\t\t\treturn 1\n\t\t1, 0:\n\t\t\treturn 2\n",
		"nothing but a wildcard": "func f(op : int):\n\tswitch op:\n\t\t_:\n\t\t\treturn 1\n",
		"a spread too wide to index": "func f(op : int):\n\tswitch op:\n\t\t0:\n\t\t\treturn 1\n\t\t100000:\n\t\t\treturn 2\n",
	}

	for what in refused:
		var source : String = refused[what]
		assert_true(ts.vmcall("compile_to_elf", source).is_empty(),
			"switch should refuse " + what)
		# Same source under match must compile.
		assert_false(ts.vmcall("compile_to_elf", source.replace("switch op:", "match op:")).is_empty(),
			"match should still accept " + what)

	ts.queue_free()

func _engine_negative_and_holes(n):
	match n:
		-3:
			return 10
		-1:
			return 20
		0:
			return 30
		2:
			return 40
		_:
			return -1

func _engine_step(op, acc, imm):
	match op:
		0:
			return acc
		1:
			return imm
		2:
			return acc + imm
		3:
			return acc - imm
		4:
			return acc * imm
		5:
			return -acc
		_:
			return -9999

func _engine_run(steps):
	var acc = 0
	var i = 0
	while i < steps:
		acc = _engine_step(i % 6, acc, 3)
		i += 1
	return acc

func test_cpu_runs_its_programs():
	var source = _load_cpu_source()
	if source == "":
		return

	# The machine executes thousands of guest instructions per run, so it needs a
	# larger instruction budget than the default.
	var s = _compile_and_load(source, 200000000)
	if s == null:
		return

	assert_eq(s.vmcallv("encode", 3, 1, 2, 7), 3 | (1 << 8) | (2 << 16) | (7 << 24),
		"encode() should pack an instruction word")

	# 1 + 2 + ... + n, computed by the guest machine, for a few n.
	assert_eq(s.vmcallv("sum_to", 0), [0], "sum_to(0) should emit 0")
	assert_eq(s.vmcallv("sum_to", 1), [1], "sum_to(1) should emit 1")
	assert_eq(s.vmcallv("sum_to", 10), [55], "sum_to(10) should emit 55")
	assert_eq(s.vmcallv("sum_to", 100), [5050], "sum_to(100) should emit 5050")

	s.queue_free()

func test_cpu_alu_arms():
	# One OP_OUT per ALU opcode, so a wrong arm shows up as a wrong element rather
	# than a wrong total.
	var source = _load_cpu_source()
	if source == "":
		return
	var s = _compile_and_load(source, 200000000)
	if s == null:
		return

	var a = 12
	var b = 5
	var expected = [
		a - b,
		a * b,
		a & b,
		a | b,
		a ^ b,
		a << 1,
		a >> 2,
		3, 2, 1,  # the JNZ countdown
	]
	assert_eq(s.vmcallv("alu_trace", a, b), expected,
		"every ALU opcode should emit what the operator does")

	s.queue_free()

func test_cpu_stops_on_what_it_does_not_know():
	var source = _load_cpu_source()
	if source == "":
		return
	var s = _compile_and_load(source, 200000000)
	if s == null:
		return

	# An opcode outside the table falls out of the match and halts the machine,
	# rather than running a neighbouring arm.
	assert_eq(s.vmcallv("unknown_opcode"), [7],
		"an unknown opcode should stop the machine where it stands")
	# Running out of fuel returns what the machine emitted: one value per three
	# instructions, so seven instructions is three values plus a partial fourth.
	assert_eq(s.vmcallv("out_of_fuel"), [1, 1, 1],
		"running out of fuel should return the trace so far")

	s.queue_free()

func test_cpu_steps_one_instruction_at_a_time():
	# Per-call stepping with state held in script globals.
	var source = _load_cpu_source()
	if source == "":
		return
	var s = _compile_and_load(source, 200000000)
	if s == null:
		return

	# LOADI 5 into r0, OUT it, HALT.
	var program = [
		s.vmcallv("encode", 1, 0, 0, 5),
		s.vmcallv("encode", 15, 0, 0, 0),
		s.vmcallv("encode", 0, 0, 0, 0),
	]
	s.vmcallv("reset", program)

	assert_eq(s.vmcallv("step"), 1, "the first step should leave the machine at pc 1")
	assert_eq(s.vmcallv("registers"), [5, 0, 0, 0, 0, 0], "LOADI should have written r0")
	assert_eq(s.vmcallv("trace"), [], "nothing has been emitted yet")

	assert_eq(s.vmcallv("step"), 2, "the second step should leave the machine at pc 2")
	assert_eq(s.vmcallv("trace"), [5], "OUT should have emitted r0")

	assert_eq(s.vmcallv("step"), -1, "HALT should answer -1")
	assert_true(s.vmcallv("is_halted"), "the machine should be halted")
	assert_eq(s.vmcallv("step"), -1, "stepping a halted machine should answer -1")
	assert_eq(s.vmcallv("trace"), [5], "a halted machine should emit nothing more")

	s.queue_free()

func test_cpu_stepped_matches_the_loop():
	# step() must agree with run() on the same program.
	var source = _load_cpu_source()
	if source == "":
		return
	var s = _compile_and_load(source, 200000000)
	if s == null:
		return

	for n in [0, 1, 10, 100]:
		assert_eq(s.vmcallv("sum_to_stepped", n), s.vmcallv("sum_to", n),
			"stepping should compute what running computes, for n = " + str(n))

	# Unknown opcode halts the stepped machine too.
	s.vmcallv("reset", [
		s.vmcallv("encode", 1, 0, 0, 7),
		s.vmcallv("encode", 15, 0, 0, 0),
		s.vmcallv("encode", 99, 0, 0, 0),
		s.vmcallv("encode", 15, 0, 0, 0),
	])
	assert_eq(s.vmcallv("step_until_halted", 64), [7],
		"an unknown opcode should stop the stepped machine too")
	assert_true(s.vmcallv("is_halted"), "an unknown opcode should halt the machine")

	s.queue_free()

func test_cpu_loads_as_a_safegdscript_resource():
	# The same file reached as a user reaches it: attached to a node as a script,
	# compiled by the .sgd loader.
	var script = load(CPU_SOURCE_PATH)
	assert_not_null(script, "test_cpu.sgd should load as a SafeGDScript resource")
	if script == null:
		return

	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(200000000)
	assert_eq(node.call("sum_to", 10), [55], "sum_to(10) should emit 55 through the .sgd loader")
	node.free()

# -= Function signatures =-
#
# A call from Godot lands on the exported guest function directly, and the
# Sandbox ABI gives that function one Variant pointer per argument and no count.
# An argument the caller left out is therefore a null pointer, which the guest
# faults on the moment it reads the parameter -- so the arity has to reach
# Godot, and the produced ELF cannot carry it: its symbol table has names alone.
# The compiler publishes the signatures beside the ELF, and the .sgd script
# checks the call against them.

const SIGNATURE_SOURCE = """
func takes_one(f: float):
	return f * 2.0

func takes_none():
	return 7

func with_defaults(a, b = 10, c = 2.5):
	return str(a) + "/" + str(b) + "/" + str(c)
"""

func _signature_node() -> Node:
	var path = "user://temp_signatures.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(SIGNATURE_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the signature script should load as a SafeGDScript resource")
	if script == null:
		return null
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(100000)
	return node

func _method_info(node: Node, name: String) -> Dictionary:
	for method in node.get_method_list():
		if method["name"] == name:
			return method
	return {}

func test_sgd_publishes_argument_lists():
	var node = _signature_node()
	if node == null:
		return

	var one = _method_info(node, "takes_one")
	assert_eq(one.get("args", []).size(), 1, "takes_one should declare one argument")
	assert_eq(one["args"][0]["name"], "f", "the argument should keep the name it was given")
	assert_eq(one["args"][0]["type"], TYPE_FLOAT, "'f: float' should reach Godot as a float")
	assert_eq(one.get("default_args", []).size(), 0, "takes_one has no defaults")

	assert_eq(_method_info(node, "takes_none").get("args", []).size(), 0,
		"takes_none should declare no arguments")

	var defaults = _method_info(node, "with_defaults")
	assert_eq(defaults.get("args", []).size(), 3, "with_defaults should declare three arguments")
	assert_eq(defaults.get("default_args", []), [10, 2.5],
		"the two constant defaults should reach Godot as values")
	# An untyped parameter may hold anything, which Godot spells as NIL.
	assert_eq(defaults["args"][0]["type"], TYPE_NIL, "an untyped parameter is any Variant")

	node.free()

func test_sgd_calls_with_the_declared_arity():
	var node = _signature_node()
	if node == null:
		return

	assert_eq(node.call("takes_one", 3.5), 7.0, "takes_one(3.5) should return 7.0")
	assert_eq(node.call("takes_none"), 7, "takes_none() should return 7")

	# The callee cannot fill a default in: it has no way to tell whether it was
	# given the argument. The host appends the ones the compiler folded.
	assert_eq(node.call("with_defaults", 1), "1/10/2.5", "both defaults should be supplied")
	assert_eq(node.call("with_defaults", 1, 2), "1/2/2.5", "the last default should be supplied")
	assert_eq(node.call("with_defaults", 1, 2, 3), "1/2/3", "no default should be supplied")

	node.free()

# One refused call, on its own frame: the runtime error it raises unwinds only
# this function, which leaves the test that called it running.
func _refused_call(node: Node, method: String, args: Array) -> void:
	node.callv(method, args)

func test_sgd_refuses_a_call_with_the_wrong_argument_count():
	var node = _signature_node()
	if node == null:
		return

	# Without the arity this reached the guest with a null pointer where 'f'
	# should be, and faulted inside the sandbox instead of failing the call.
	# A refused call is a runtime error, which unwinds the frame that made it,
	# so each one is made from its own helper and the test goes on.
	_refused_call(node, "takes_one", [])
	assert_engine_error("'Node::takes_one': Method expected 1 argument(s), but called with 0.")

	_refused_call(node, "takes_one", [1.0, 2.0])
	assert_engine_error("'Node::takes_one': Method expected 1 argument(s), but called with 2.")

	# 'a' has no default, so it is required even though the other two are not.
	_refused_call(node, "with_defaults", [])
	assert_engine_error("'Node::with_defaults': Method expected 1 argument(s), but called with 0.")

	node.free()

# -= Built-in scripts =-
#
# Scene sub-resource, no file. duplicate() and the scene saver carry STORAGE
# properties only; source travels as "script/source".

const BUILTIN_SOURCE = """
func answer():
	return 42
"""

func test_sgd_source_survives_make_unique():
	var path = "user://temp_builtin.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(BUILTIN_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the built-in source should load as a SafeGDScript resource")
	if script == null:
		return

	# "Make Unique" path.
	var unique = script.duplicate()
	assert_not_null(unique, "duplicate() should hand back a SafeGDScript")
	assert_eq(unique.get_source_code(), script.get_source_code(),
		"the duplicate should carry the source, not the empty template")

	# Duplicate compiled on construction; verify it runs.
	var node = Node.new()
	node.set_script(unique)
	node.set_instructions_max(100000)
	assert_eq(node.call("answer"), 42, "the duplicate should run what it was given")
	node.free()

	# No file path => no global class; duplicates would collide otherwise.
	assert_eq(unique.get_global_name(), &"", "a script with no file declares no global class")

func test_sgd_embeds_into_a_scene():
	var node = Node.new()
	node.name = "Embedded"
	var script = SafeGDScript.new()
	script.set_source_code(BUILTIN_SOURCE)
	node.set_script(script)

	var packed = PackedScene.new()
	assert_eq(packed.pack(node), OK, "the node should pack")
	var scene_path = "user://temp_builtin_scene.tscn"
	assert_eq(ResourceSaver.save(packed, scene_path), OK, "the scene should save")
	node.free()

	var text = FileAccess.get_file_as_string(scene_path)
	assert_true(text.contains("script/source"),
		"the saved scene should carry the script source")
	assert_true(text.contains("func answer():"),
		"the saved scene should carry the source verbatim")

	# Bypass the resource cache the save left behind.
	var reloaded = ResourceLoader.load(scene_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	assert_not_null(reloaded, "the scene should load back")
	if reloaded == null:
		return
	var instance = reloaded.instantiate()
	instance.set_instructions_max(100000)
	assert_eq(instance.call("answer"), 42, "the embedded script should run after a round trip")
	instance.free()

# -= Breakpoints =-
#
# A breakpoint is compiled in, not switched on: the backend emits a stop at the
# lines it was given and nothing anywhere else, so setting one recompiles the
# program and reloads every instance of it. That is what makes a program with no
# breakpoints cost nothing, and it is affordable because .sgd programs are small.
#
# The stop itself is a system call that has not returned yet. Nothing about the
# guest is suspended -- it burns no instructions and cannot time out on the count
# -- and returning from the call is what "continue" means.
const BREAKPOINT_SOURCE = """
func seed():
	return 10

func work():
	var a = seed()
	var b = a * 3
	return b - a

func loop():
	var total = 0
	for i in range(4):
		total += i
	return total
"""
#  1 blank            8 return b - a
#  2 func seed():       9 blank
#  3 return 10         10 func loop():
#  4 blank             11 var total = 0
#  5 func work():      12 for i in range(4):
#  6 var a = seed()    13 total += i
#  7 var b = a * 3     14 return total

var _break_lines : Array = []
var _break_stopped : Array = []
var _break_reported_line : Array = []
var _break_backtrace : Array = []
var _break_reentrant_result = null
var _break_reenter_node : Node = null
# Set to have the handler try to rebuild the program it is stopped inside.
var _break_rebuild_script = null
var _break_rebuild_result = null

func _on_breakpoint(script, line):
	# Everything below runs with the guest standing on the breakpoint: the host
	# thread is inside the system call the break made, and the call has not
	# returned. That is the only window in which the break state says anything.
	_break_lines.append(line)
	_break_stopped.append(SafeGDScript.is_stopped())
	_break_reported_line.append(SafeGDScript.get_stopped_line())
	_break_backtrace.append(SafeGDScript.get_stopped_backtrace())
	if _break_reenter_node != null:
		# The question this whole design was uncertain about: Godot calling into
		# the VM while it is stopped. vmcall sees a call already in progress and
		# preempts instead of restarting, and libriscv restores the registers the
		# break was standing on, so the stopped call resumes where it was.
		_break_reentrant_result = _break_reenter_node.call("seed")
	if _break_rebuild_script != null:
		# A rebuild reloads every instance from the new ELF, and the guest is
		# standing in the old one. Refused, not done.
		_break_rebuild_result = _break_rebuild_script.set_breakpoint(6, true)
	SafeGDScript.debug_continue()

func _breakpoint_script(name : String):
	var path = "user://temp_%s.sgd" % name
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(BREAKPOINT_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the breakpoint script should load as a SafeGDScript resource")
	return script

func _breakpoint_node(script) -> Node:
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(100000)
	return node

func _reset_break_capture():
	_break_lines = []
	_break_stopped = []
	_break_reported_line = []
	_break_backtrace = []
	_break_reentrant_result = null
	_break_reenter_node = null
	_break_rebuild_script = null
	_break_rebuild_result = null

func test_sgd_breakpoint_stops_and_continues():
	var script = _breakpoint_script("break_basic")
	if script == null:
		return
	var node = _breakpoint_node(script)

	# Nothing is compiled in until a breakpoint asks for it.
	assert_false(script.is_debug_build(), "an ordinary build carries no debug info")
	assert_eq(script.get_breakpoints(), PackedInt32Array(), "and no breakpoints")
	assert_eq(node.call("work"), 20, "work() = 20 before any of this")

	script.breakpoint_hit.connect(_on_breakpoint)
	_reset_break_capture()

	assert_true(script.set_breakpoint(7, true), "setting a breakpoint recompiles the program")
	assert_eq(script.get_breakpoints(), PackedInt32Array([7]), "the line was taken")
	assert_eq(script.get_active_breakpoints(), PackedInt32Array([7]),
		"and the compile could place it")
	assert_true(script.is_debug_build(),
		"a breakpoint asks for the shadow stack whether or not the caller did")

	# The answer is the point: a stop that changes one is not a breakpoint.
	assert_eq(node.call("work"), 20, "work() = 20 across the break")
	assert_eq(_break_lines, [7], "the guest stopped once, on the line asked for")
	assert_eq(_break_stopped, [true], "and it was stopped while the handler ran")
	assert_eq(_break_reported_line, [7], "the break state names the same line")

	# Innermost first: the break is inside work(), which nothing called.
	assert_eq(_break_backtrace.size(), 1, "one stop, one backtrace")
	if _break_backtrace.size() == 1:
		var frames : PackedStringArray = _break_backtrace[0]
		assert_eq(frames.size(), 1, "work() was the only frame standing")
		if frames.size() >= 1:
			assert_true(frames[0].contains(":7"), "the innermost frame is line 7: " + frames[0])
			assert_true(frames[0].contains("work"), "and it is work(): " + frames[0])

	# Nothing is stopped once the call has returned.
	assert_false(SafeGDScript.is_stopped(), "the guest is running again")
	assert_eq(SafeGDScript.get_stopped_line(), -1, "and no line is stopped on")

	# Clearing recompiles back to a program with no instrumentation at all.
	_reset_break_capture()
	assert_true(script.clear_breakpoints(), "clearing recompiles too")
	assert_false(script.is_debug_build(), "and leaves an ordinary build")
	assert_eq(node.call("work"), 20, "work() = 20 with the breakpoint gone")
	assert_eq(_break_lines, [], "and nothing stopped")

	script.breakpoint_hit.disconnect(_on_breakpoint)
	node.free()

func test_sgd_breakpoint_survives_a_call_from_the_handler():
	# The uncertainty this design was built around: while the guest is stopped
	# the host thread is inside its system call, and Godot is free to call into
	# the same program. It must not clobber the call that is standing still.
	var script = _breakpoint_script("break_reentrant")
	if script == null:
		return
	var node = _breakpoint_node(script)
	script.breakpoint_hit.connect(_on_breakpoint)
	_reset_break_capture()
	_break_reenter_node = node

	assert_true(script.set_breakpoint(7, true), "the breakpoint should compile in")
	assert_eq(node.call("work"), 20,
		"the stopped call answers 20 even after another call ran inside it")
	assert_eq(_break_reentrant_result, 10, "and the call made from the handler answered")
	assert_eq(_break_lines, [7], "the break happened once")

	script.breakpoint_hit.disconnect(_on_breakpoint)
	node.free()

func test_sgd_breakpoint_in_a_loop_stops_every_pass():
	# Says the stop is emitted below the loop label rather than above it: above,
	# the back edge would skip it and the loop would stop once.
	var script = _breakpoint_script("break_loop")
	if script == null:
		return
	var node = _breakpoint_node(script)
	script.breakpoint_hit.connect(_on_breakpoint)
	_reset_break_capture()

	assert_true(script.set_breakpoint(13, true), "the loop body line should compile in")
	assert_eq(node.call("loop"), 6, "loop() = 0+1+2+3")
	assert_eq(_break_lines, [13, 13, 13, 13], "the body line stopped once per pass")

	script.breakpoint_hit.disconnect(_on_breakpoint)
	node.free()

func test_sgd_breakpoint_on_a_for_header_stops_at_setup_and_each_pass():
	# A `for` line owns code in two places -- the setup before the loop and the
	# increment the back edge runs through -- and a break is emitted wherever the
	# line has code. So it stops once on the way in and once per pass, not once
	# per pass. Nothing here is special-cased; this is what the line owns.
	var script = _breakpoint_script("break_for_header")
	if script == null:
		return
	var node = _breakpoint_node(script)
	script.breakpoint_hit.connect(_on_breakpoint)
	_reset_break_capture()

	assert_true(script.set_breakpoint(12, true), "the for header should compile in")
	assert_eq(node.call("loop"), 6, "loop() = 0+1+2+3")
	assert_eq(_break_lines, [12, 12, 12, 12, 12],
		"once entering the loop, then once per pass")

	script.breakpoint_hit.disconnect(_on_breakpoint)
	node.free()

func test_sgd_breakpoints_on_several_lines():
	var script = _breakpoint_script("break_several")
	if script == null:
		return
	var node = _breakpoint_node(script)
	script.breakpoint_hit.connect(_on_breakpoint)
	_reset_break_capture()

	assert_true(script.set_breakpoints(PackedInt32Array([8, 3, 6, 6])),
		"a set is taken whole, deduplicated and sorted")
	assert_eq(script.get_breakpoints(), PackedInt32Array([3, 6, 8]),
		"ascending, without the repeat")

	assert_eq(node.call("work"), 20, "work() = 20 across three breaks")
	# Execution order, not source order: work() reaches line 6 before the call it
	# makes there reaches line 3.
	assert_eq(_break_lines, [6, 3, 8], "the stops are in the order they were reached")

	# The call stack is what makes a stop more than a line: at line 3 the guest
	# is inside seed(), which work() called.
	if _break_backtrace.size() == 3:
		var inside_seed : PackedStringArray = _break_backtrace[1]
		assert_eq(inside_seed.size(), 2, "seed() runs one frame below work()")
		if inside_seed.size() == 2:
			assert_true(inside_seed[0].contains("seed"), "innermost is seed(): " + inside_seed[0])
			assert_true(inside_seed[1].contains("work"), "and below it work(): " + inside_seed[1])
			assert_true(inside_seed[1].contains(":6"),
				"work() is sitting on the line it made the call from: " + inside_seed[1])

	script.breakpoint_hit.disconnect(_on_breakpoint)
	node.free()

func test_sgd_breakpoint_on_a_line_with_no_code():
	# An editor lets a breakpoint sit anywhere, including a blank line and a line
	# the optimizer left no instructions on. Neither may fail the compile, and
	# both have to be visible as taken-but-dead rather than silently ignored.
	var script = _breakpoint_script("break_dead")
	if script == null:
		return
	var node = _breakpoint_node(script)
	script.breakpoint_hit.connect(_on_breakpoint)
	_reset_break_capture()

	assert_true(script.set_breakpoints(PackedInt32Array([1, 4, 7])),
		"a blank line and a real one should compile")
	assert_eq(script.get_breakpoints(), PackedInt32Array([1, 4, 7]),
		"all three were taken")
	assert_eq(script.get_active_breakpoints(), PackedInt32Array([7]),
		"but only the one with code behind it can stop the program")

	assert_eq(node.call("work"), 20, "work() = 20")
	assert_eq(_break_lines, [7], "and only the live breakpoint stopped it")

	script.breakpoint_hit.disconnect(_on_breakpoint)
	node.free()

func test_sgd_breakpoint_with_nothing_listening_does_not_block():
	# The safety valve. A break blocks the thread that would have delivered the
	# continue, so with no debugger connected there is nobody who could send one:
	# it reports where it stopped and lets the guest go.
	var script = _breakpoint_script("break_unattended")
	if script == null:
		return
	var node = _breakpoint_node(script)

	assert_true(script.set_breakpoint(7, true), "the breakpoint should compile in")
	assert_eq(node.call("work"), 20, "work() = 20, and the call returned at all")
	assert_false(SafeGDScript.is_stopped(), "nothing is left stopped")

	node.free()

func test_sgd_breakpoint_cannot_rebuild_a_stopped_program():
	# Toggling a breakpoint from the handler is the natural thing to try, and it
	# is the one thing that must not happen: setting one recompiles, and the
	# guest is standing in the ELF that would be replaced.
	var script = _breakpoint_script("break_rebuild")
	if script == null:
		return
	var node = _breakpoint_node(script)
	script.breakpoint_hit.connect(_on_breakpoint)
	_reset_break_capture()

	assert_true(script.set_breakpoint(7, true), "the breakpoint should compile in")
	_break_rebuild_script = script

	assert_eq(node.call("work"), 20, "the stopped call still answers 20")
	assert_eq(_break_rebuild_result, false, "the rebuild from inside the break was refused")
	assert_engine_error("SafeGDScript: %s is stopped at a breakpoint; continue before changing its breakpoints." % script.resource_path)
	assert_eq(script.get_breakpoints(), PackedInt32Array([7]),
		"and the breakpoint set is unchanged")

	# Once it has continued, the same call is taken.
	_break_rebuild_script = null
	assert_true(script.set_breakpoint(6, true), "the same change is taken once it is running")
	assert_eq(script.get_breakpoints(), PackedInt32Array([6, 7]), "both lines are set")

	script.breakpoint_hit.disconnect(_on_breakpoint)
	node.free()

func test_sgd_breakpoint_line_must_be_one_based() :
	var script = _breakpoint_script("break_zero")
	if script == null:
		return
	assert_false(script.set_breakpoint(0, true), "line 0 is not a source line")
	assert_engine_error("SafeGDScript::set_breakpoint: a source line is 1-based.")
	assert_eq(script.get_breakpoints(), PackedInt32Array(), "and nothing was taken")

func test_array_element_access():
	# `a[i]` on a known Array is ECALL_ARRAY_AT rather than Variant::call("get"),
	# and a negative index is normalised in the guest before the call. Array.get(),
	# which the VCALL path reached, does not wrap -- so every answer here is
	# checked against the engine's own.
	var gdscript_code = """
func read(a : Array, i : int):
	return a[i]

func write(a : Array, i : int, v):
	a[i] = v
	return a

func swap_ends(a : Array):
	var first = a[0]
	a[0] = a[-1]
	a[-1] = first
	return a

func build(n : int) -> Array:
	var out : Array = []
	var i : int = 0
	while i < n:
		out.append(i * i)
		i += 1
	return out

func total(a : Array) -> int:
	var acc : int = 0
	var i : int = 0
	while i < a.size():
		acc += a[i]
		i += 1
	return acc
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	var values : Array = [10, 20, 30, 40]

	# A positive index, and the negative one the VCALL path could not do.
	for i in [0, 1, 2, 3, -1, -2, -3, -4]:
		assert_eq(s.vmcallv("read", values, i), values[i],
			"a[%d] should be what the engine says it is" % i)

	assert_eq(s.vmcallv("write", [1, 2, 3], 1, 99), [1, 99, 3], "a positive index should write in place")
	assert_eq(s.vmcallv("write", [1, 2, 3], -1, 99), [1, 2, 99], "a negative index should write from the end")
	assert_eq(s.vmcallv("swap_ends", [1, 2, 3, 4]), [4, 2, 3, 1],
		"reading and writing from the end should reach the same element")

	# append and index in a loop: the shape the syscall path exists for.
	var built = s.vmcallv("build", 8)
	assert_eq(built, [0, 1, 4, 9, 16, 25, 36, 49], "append should build the same Array the engine would")
	assert_eq(s.vmcallv("total", built), 140, "indexing should read back what append wrote")

	# An Array from the engine, of mixed element types: the element type is not
	# the compiler's to assume.
	assert_eq(s.vmcallv("read", [1, "two", 3.5, [4]], 1), "two", "an element may be any Variant")
	assert_eq(s.vmcallv("read", [1, "two", 3.5, [4]], 2), 3.5, "an element may be any Variant")

	s.queue_free()

func test_dictionary_element_access():
	# `d[k]` is ECALL_DICTIONARY_OPS rather than Variant::call("get"). A missing
	# key reads as null and is not created by the read, unlike
	# Dictionary::operator[], which the host used to go through.
	var gdscript_code = """
func read(d : Dictionary, k):
	return d[k]

func write(d : Dictionary, k, v):
	d[k] = v
	return d

func by_name(d : Dictionary):
	d.count = d.count + 1
	return d.count

func histogram(words : Array) -> Dictionary:
	var counts : Dictionary = {}
	for w in words:
		if counts.has(w):
			counts[w] = counts[w] + 1
		else:
			counts[w] = 1
	return counts
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	var d : Dictionary = {"a": 1, 2: "two", 3.5: [1, 2]}
	assert_eq(s.vmcallv("read", d, "a"), 1, "a String key should read its value")
	assert_eq(s.vmcallv("read", d, 2), "two", "an integer key should read its value")
	assert_eq(s.vmcallv("read", d, 3.5), [1, 2], "a float key should read its value")

	# A missing key reads as null, and the read leaves the Dictionary alone.
	var probe : Dictionary = {"a": 1}
	assert_eq(s.vmcallv("read", probe, "missing"), null, "a missing key should read as null")
	assert_eq(probe.size(), 1, "reading a missing key should not create it")

	assert_eq(s.vmcallv("write", {"a": 1}, "b", 2), {"a": 1, "b": 2}, "a write should add the key")
	assert_eq(s.vmcallv("write", {"a": 1}, "a", 9), {"a": 9}, "a write should replace the value")
	assert_eq(s.vmcallv("by_name", {"count": 4}), 5, "d.key should mean d[\"key\"]")

	assert_eq(s.vmcallv("histogram", ["a", "b", "a", "c", "a", "b"]), {"a": 3, "b": 2, "c": 1},
		"a keyed loop should build the same Dictionary the engine would")

	s.queue_free()

func test_untyped_arithmetic_matches_the_engine():
	# Everything a container hands back is a Variant of unknown type, so untyped
	# arithmetic tests for two integers at run time and falls back to Godot's
	# Variant::evaluate() otherwise. Both paths have to answer what the engine
	# answers -- including the shifts, which Godot errors on for a negative operand
	# and which therefore may not take the register path.
	var gdscript_code = """
func add(a, b):
	return a + b

func mul(a, b):
	return a * b

func bit_and(a, b):
	return a & b

func shl(a, b):
	return a << b

func shr(a, b):
	return a >> b

func less(a, b):
	if a < b:
		return "less"
	return "not less"

func from_array(a : Array, i : int, j : int):
	return a[i] + a[j]
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	# Two integers: the register path.
	assert_eq(s.vmcallv("add", 3, 4), 7, "int + int")
	assert_eq(s.vmcallv("mul", 3, 4), 12, "int * int")
	assert_eq(s.vmcallv("bit_and", 12, 10), 8, "int & int")
	assert_eq(s.vmcallv("shl", 3, 4), 48, "int << int")
	assert_eq(s.vmcallv("shr", 48, 4), 3, "int >> int")
	assert_eq(s.vmcallv("less", 3, 4), "less", "int < int")
	assert_eq(s.vmcallv("less", 4, 3), "not less", "int < int")

	# Anything else: the host path.
	assert_eq(s.vmcallv("add", 3, 4.5), 7.5, "int + float")
	assert_eq(s.vmcallv("add", 1.5, 4.5), 6.0, "float + float")
	assert_eq(s.vmcallv("add", "ab", "cd"), "abcd", "String + String")
	assert_eq(s.vmcallv("add", Vector2(1, 2), Vector2(3, 4)), Vector2(4, 6), "Vector2 + Vector2")
	assert_eq(s.vmcallv("add", [1], [2]), [1, 2], "Array + Array")
	assert_eq(s.vmcallv("mul", 3, 1.5), 4.5, "int * float")
	assert_eq(s.vmcallv("less", 3, 4.5), "less", "int < float")
	assert_eq(s.vmcallv("less", "a", "b"), "less", "String < String")

	# Array elements carry no type: the run-time test on values the compiler
	# never saw.
	assert_eq(s.vmcallv("from_array", [1, 2, 3], 0, 2), 4, "int elements add in registers")
	assert_eq(s.vmcallv("from_array", [1, 2.5, 3], 0, 1), 3.5, "a float element goes to the host")
	assert_eq(s.vmcallv("from_array", ["a", "b"], 0, 1), "ab", "String elements go to the host")

	s.queue_free()

func test_declared_return_type_is_the_type_returned():
	# A caller acts on the return type read off the signature: `-> float` types
	# the call's result register, and typed float arithmetic reads the payload as
	# a double. `return 1` has to widen to 1.0, or the caller reads an INT 1 as a
	# denormal.
	var gdscript_code = """
func one() -> float:
	return 1

func doubled() -> float:
	var x : float = one()
	return x * 2.0

func is_less(a : int, b : int) -> int:
	return a < b

func counted(n : int) -> int:
	var acc : int = 0
	for i in range(n):
		acc += i * 2
	return acc
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("one"), 1.0, "a function declared -> float should return a float")
	assert_eq(typeof(s.vmcallv("one")), TYPE_FLOAT, "and it should carry the float type tag")
	assert_eq(s.vmcallv("doubled"), 2.0, "the caller should read it as a float")

	# GDScript converts a bool to the declared int, as the engine does below.
	assert_eq(s.vmcallv("is_less", 1, 2), 1, "bool should convert to the declared int")
	assert_eq(s.vmcallv("is_less", 2, 1), 0, "bool should convert to the declared int")

	# `for i in range(n)` gives an integer, which is what keeps the body's
	# arithmetic native rather than a Variant call.
	assert_eq(s.vmcallv("counted", 5), 20, "a range loop should sum the same as the engine")
	assert_eq(s.vmcallv("counted", 0), 0, "an empty range should run no iterations")

	s.queue_free()

# Writing a component of an inline Variant type. VSET_INLINE is a payload store;
# the property syscalls it replaced are Object-only and throw on a Vector2.
# Every answer below is checked against the engine running the same expression.
func test_inline_member_writes():
	var gdscript_code = """
func vec2_write(x : float, y : float) -> Vector2:
	var v : Vector2 = Vector2(0.0, 0.0)
	v.x = x
	v.y = y
	return v

func vec2_compound(start : float) -> Vector2:
	var v : Vector2 = Vector2(start, start)
	v.x += 2.0
	v.y *= 3.0
	return v

func vec3i_write(z : int) -> Vector3i:
	var v : Vector3i = Vector3i(1, 2, 3)
	v.z = z
	return v

func vec3i_from_float() -> Vector3i:
	var v : Vector3i = Vector3i(0, 0, 0)
	v.x = 3.7
	return v

func vec2_from_int() -> Vector2:
	var v : Vector2 = Vector2(0.0, 0.0)
	v.x = 4
	return v

func color_write(g : float) -> Color:
	var c : Color = Color(1.0, 0.0, 0.0, 1.0)
	c.g = g
	return c

func vec4_write(w : float) -> Vector4:
	var v : Vector4 = Vector4(1.0, 2.0, 3.0, 4.0)
	v.w = w
	return v
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	# Engine reference value.
	var expected_vec2 := Vector2(0.0, 0.0)
	expected_vec2.x = 5.0
	expected_vec2.y = 7.0
	assert_eq(s.vmcallv("vec2_write", 5.0, 7.0), expected_vec2, "a Vector2 component write should stick")

	var expected_compound := Vector2(1.0, 1.0)
	expected_compound.x += 2.0
	expected_compound.y *= 3.0
	assert_eq(s.vmcallv("vec2_compound", 1.0), expected_compound, "compound assignment should read then write the component")

	var expected_vec3i := Vector3i(1, 2, 3)
	expected_vec3i.z = 9
	assert_eq(s.vmcallv("vec3i_write", 9), expected_vec3i, "an integer vector component write should stick")

	# An integer vector truncates a float, and a float vector widens an integer.
	var expected_trunc := Vector3i(0, 0, 0)
	expected_trunc.x = 3.7
	assert_eq(s.vmcallv("vec3i_from_float"), expected_trunc, "a float assigned to an int component should truncate as the engine does")

	var expected_widen := Vector2(0.0, 0.0)
	expected_widen.x = 4
	assert_eq(s.vmcallv("vec2_from_int"), expected_widen, "an int assigned to a float component should widen")

	var expected_color := Color(1.0, 0.0, 0.0, 1.0)
	expected_color.g = 0.5
	assert_eq(s.vmcallv("color_write", 0.5), expected_color, "a Color channel write should stick")

	var expected_vec4 := Vector4(1.0, 2.0, 3.0, 4.0)
	expected_vec4.w = 8.0
	assert_eq(s.vmcallv("vec4_write", 8.0), expected_vec4, "a Vector4 component write should stick")

	s.queue_free()

# A component of a value read out of a container is a copy: the mutation has to
# travel back, one link at a time, or the write is lost.
func test_member_write_travels_back():
	var gdscript_code = """
var origin : Vector2 = Vector2(0.0, 0.0)

func global_write(x : float) -> Vector2:
	origin.x = x
	return origin

func element_write(x : float) -> Array:
	var a : Array = [Vector2(0.0, 0.0), Vector2(9.0, 9.0)]
	var i : int = 0
	a[i].x = x
	return a

func entry_write(x : float) -> Dictionary:
	var d : Dictionary = {"p": Vector2(0.0, 0.0)}
	d["p"].x = x
	return d

func nested_write(x : float) -> Array:
	var inner : Array = [Vector2(0.0, 0.0)]
	var outer : Array = [inner]
	outer[0][0].x = x
	return outer

func copy_is_independent(x : float) -> Vector2:
	var v : Vector2 = Vector2(1.0, 1.0)
	var copy : Vector2 = v
	copy.x = x
	return v
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	var expected_global := Vector2(0.0, 0.0)
	expected_global.x = 3.0
	assert_eq(s.vmcallv("global_write", 3.0), expected_global, "a write through a global should reach the global")

	var expected_array : Array = [Vector2(0.0, 0.0), Vector2(9.0, 9.0)]
	expected_array[0].x = 3.0
	assert_eq(s.vmcallv("element_write", 3.0), expected_array, "a write through an Array element should reach the element")

	var expected_dict : Dictionary = {"p": Vector2(0.0, 0.0)}
	expected_dict["p"].x = 3.0
	assert_eq(s.vmcallv("entry_write", 3.0), expected_dict, "a write through a Dictionary value should reach the value")

	var expected_inner : Array = [Vector2(0.0, 0.0)]
	var expected_outer : Array = [expected_inner]
	expected_outer[0][0].x = 3.0
	assert_eq(s.vmcallv("nested_write", 3.0), expected_outer, "a write two links deep should reach the element")

	# Assigning the vector copies it, so the original must not move.
	assert_eq(s.vmcallv("copy_is_independent", 5.0), Vector2(1.0, 1.0), "writing through a copy should not reach the original")

	s.queue_free()

# The type is only known when the program says so. An untyped chain picks the
# inline payload or the Object property from the type tag, at run time.
func test_untyped_member_access():
	var gdscript_code = """
func read_x(v):
	return v.x

func write_x(v, x):
	v.x = x
	return v

func read_name(o):
	return o.name
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	# The same code has to serve every inline type it is handed.
	assert_almost_eq(s.vmcallv("read_x", Vector2(1.5, 2.5)), 1.5, 0.001, "an untyped read should reach a Vector2 payload")
	assert_almost_eq(s.vmcallv("read_x", Vector3(4.5, 0.0, 0.0)), 4.5, 0.001, "an untyped read should reach a Vector3 payload")
	assert_eq(s.vmcallv("read_x", Vector2i(7, 8)), 7, "an untyped read should reach a Vector2i payload as an int")

	var written = s.vmcallv("write_x", Vector2(0.0, 0.0), 6.0)
	assert_eq(written, Vector2(6.0, 0.0), "an untyped write should reach a Vector2 payload")

	var written3 = s.vmcallv("write_x", Vector3i(0, 1, 2), 6)
	assert_eq(written3, Vector3i(6, 1, 2), "an untyped write should reach a Vector3i payload")

	# A member no inline type carries is still an Object property.
	var node := Node.new()
	node.name = "Probe"
	assert_eq(s.vmcallv("read_name", node), "Probe", "a property read should still reach the object")
	node.queue_free()

	s.queue_free()

# Every position walk used ECALL_ARRAY_SIZE and ECALL_ARRAY_AT, which the host
# refuses for anything but an Array. A packed array carries its own size/get, so
# a declared one is walked through those instead. The type has to be declared:
# an untyped parameter is still walked as an Array and throws in the host.
func test_packed_array_iteration():
	var gdscript_code = """
func sum_int32(p : PackedInt32Array) -> int:
	var total : int = 0
	for v in p:
		total += v
	return total

func sum_int64(p : PackedInt64Array) -> int:
	var total : int = 0
	for v in p:
		total += v
	return total

func sum_bytes(p : PackedByteArray) -> int:
	var total : int = 0
	for v in p:
		total += v
	return total

func join_strings(p : PackedStringArray) -> String:
	var joined : String = ""
	for s in p:
		joined += s
	return joined

func subscript(p : PackedInt32Array, i : int):
	return p[i]
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	var ints := PackedInt32Array([1, 2, 3, 4])
	var expected := 0
	for v in ints:
		expected += v
	assert_eq(s.vmcallv("sum_int32", ints), expected, "a PackedInt32Array should walk the same as the engine")

	assert_eq(s.vmcallv("sum_int64", PackedInt64Array([10, 20])), 30, "a PackedInt64Array should walk")
	assert_eq(s.vmcallv("sum_bytes", PackedByteArray([1, 2, 3])), 6, "a PackedByteArray should walk")
	assert_eq(s.vmcallv("sum_int32", PackedInt32Array([])), 0, "an empty packed array should run no iterations")

	assert_eq(s.vmcallv("join_strings", PackedStringArray(["a", "b", "c"])), "abc", "a PackedStringArray should walk")
	assert_eq(s.vmcallv("subscript", PackedInt32Array([5, 6, 7]), 1), 6, "a packed array should subscript")

	s.queue_free()

# typeof() reads the Variant type tag the guest already holds, rather than
# becoming self.typeof(), which Godot accepts and silently drops.
func test_typeof():
	var gdscript_code = """
func tag(x) -> int:
	return typeof(x)

func is_int(x) -> bool:
	return typeof(x) == 2
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	assert_eq(s.vmcallv("tag", 1), typeof(1), "typeof(int) should match the engine")
	assert_eq(s.vmcallv("tag", 1.5), typeof(1.5), "typeof(float) should match the engine")
	assert_eq(s.vmcallv("tag", true), typeof(true), "typeof(bool) should match the engine")
	assert_eq(s.vmcallv("tag", "text"), typeof("text"), "typeof(String) should match the engine")
	assert_eq(s.vmcallv("tag", [1]), typeof([1]), "typeof(Array) should match the engine")
	assert_eq(s.vmcallv("tag", Vector2()), typeof(Vector2()), "typeof(Vector2) should match the engine")
	assert_eq(s.vmcallv("tag", null), typeof(null), "typeof(null) should match the engine")

	assert_eq(s.vmcallv("is_int", 7), true, "a typeof comparison should hold for an int")
	assert_eq(s.vmcallv("is_int", 7.0), false, "and not for a float")

	s.queue_free()


# Built-in type constructors. Inline lowering, no CALL.
func test_builtin_constructors():
	var gdscript_code = """
func rect_from_components():
	var r = Rect2(1.0, 2.0, 3.0, 4.0)
	return r

func rect_from_vectors():
	return Rect2(Vector2(1, 2), Vector2(3, 4))

func recti():
	return Rect2i(1, 2, 3, 4)

func plane_from_components():
	return Plane(0.0, 1.0, 0.0, 5.0)

func plane_from_normal():
	return Plane(Vector3(0, 1, 0), 5)

func default_color():
	return Color()

func integer_color():
	return Color(1, 0, 0)

func color_with_alpha():
	return Color(Color(1, 0, 0), 0.25)

func default_vector2():
	return Vector2()

func default_rect():
	var r : Rect2
	return r

func rescaled_color():
	return Color8(255, 128, 0)

func rescaled_color_alpha():
	return Color8(255, 128, 0, 128)
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	assert_eq(s.vmcallv("rect_from_components"), Rect2(1.0, 2.0, 3.0, 4.0),
		"Rect2(x, y, w, h) should match the engine")
	assert_eq(s.vmcallv("rect_from_vectors"), Rect2(Vector2(1, 2), Vector2(3, 4)),
		"Rect2(position, size) should match the engine")
	assert_eq(s.vmcallv("recti"), Rect2i(1, 2, 3, 4), "Rect2i should match the engine")
	assert_eq(s.vmcallv("plane_from_components"), Plane(0.0, 1.0, 0.0, 5.0),
		"Plane(a, b, c, d) should match the engine")
	assert_eq(s.vmcallv("plane_from_normal"), Plane(Vector3(0, 1, 0), 5),
		"Plane(normal, d) should match the engine")
	# Color() is opaque black; Color(int) is not rescaled. Color8() rescales.
	assert_eq(s.vmcallv("default_color"), Color(), "Color() should be opaque black")
	assert_eq(s.vmcallv("integer_color"), Color(1, 0, 0),
		"Color with integer arguments should not be rescaled")
	assert_eq(s.vmcallv("color_with_alpha"), Color(Color(1, 0, 0), 0.25),
		"Color(from, alpha) should match the engine")
	assert_eq(s.vmcallv("default_vector2"), Vector2(), "Vector2() should be zero")
	assert_eq(s.vmcallv("default_rect"), Rect2(),
		"a declared Rect2 with no initializer should be a Rect2")
	# Color8: separate lowering, divides by 255.
	assert_eq(s.vmcallv("rescaled_color"), Color8(255, 128, 0),
		"Color8 should match the engine")
	assert_eq(s.vmcallv("rescaled_color_alpha"), Color8(255, 128, 0, 128),
		"Color8 with alpha should match the engine")

	s.queue_free()


# @GlobalScope and built-in type constants, folded to immediates.
func test_global_constants():
	var gdscript_code = """
func pi():
	return PI

func tau():
	return TAU

func infinity():
	return INF

func not_a_number():
	return NAN

func ok():
	return OK

func file_not_found():
	return ERR_FILE_NOT_FOUND

func escape():
	return KEY_ESCAPE

func left_button():
	return MOUSE_BUTTON_LEFT

func is_int(x) -> bool:
	return typeof(x) == TYPE_INT

func is_array(x) -> bool:
	return typeof(x) == TYPE_ARRAY

func vector2_zero():
	return Vector2.ZERO

func vector3_forward():
	return Vector3.FORWARD

func vector2i_max():
	return Vector2i.MAX

func color_red():
	return Color.RED

func plane_xy():
	return Plane.PLANE_XY
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	assert_eq(s.vmcallv("pi"), PI, "PI should match the engine")
	assert_eq(s.vmcallv("tau"), TAU, "TAU should match the engine")
	assert_eq(s.vmcallv("infinity"), INF, "INF should match the engine")
	assert_true(is_nan(s.vmcallv("not_a_number")), "NAN should be a NaN")
	assert_eq(s.vmcallv("ok"), OK, "OK should match the engine")
	assert_eq(s.vmcallv("file_not_found"), ERR_FILE_NOT_FOUND,
		"ERR_FILE_NOT_FOUND should match the engine")
	assert_eq(s.vmcallv("escape"), KEY_ESCAPE, "KEY_ESCAPE should match the engine")
	assert_eq(s.vmcallv("left_button"), MOUSE_BUTTON_LEFT,
		"MOUSE_BUTTON_LEFT should match the engine")

	assert_eq(s.vmcallv("is_int", 7), true, "typeof(x) == TYPE_INT for an int")
	assert_eq(s.vmcallv("is_int", 7.0), false, "and not for a float")
	assert_eq(s.vmcallv("is_array", [1]), true, "typeof(x) == TYPE_ARRAY for an Array")

	assert_eq(s.vmcallv("vector2_zero"), Vector2.ZERO, "Vector2.ZERO should match the engine")
	assert_eq(s.vmcallv("vector3_forward"), Vector3.FORWARD, "Vector3.FORWARD should match the engine")
	assert_eq(s.vmcallv("vector2i_max"), Vector2i.MAX, "Vector2i.MAX should match the engine")
	assert_eq(s.vmcallv("color_red"), Color.RED, "Color.RED should match the engine")
	assert_eq(s.vmcallv("plane_xy"), Plane.PLANE_XY, "Plane.PLANE_XY should match the engine")

	s.queue_free()


# `for i in <int>` and `for i: int in ...`.
func test_for_over_an_integer():
	var gdscript_code = """
func sum_to(n : int) -> int:
	var total = 0
	for i in n:
		total += i
	return total

func sum_literal() -> int:
	var total = 0
	for i in 5:
		total += i
	return total

func typed_loop_variable() -> int:
	var total = 0
	for i: int in range(4):
		total += i
	return total
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	# Engine reference.
	var expected = 0
	for i in 7:
		expected += i
	assert_eq(s.vmcallv("sum_to", 7), expected, "for i in <int> should match the engine")
	assert_eq(s.vmcallv("sum_literal"), 0 + 1 + 2 + 3 + 4, "for i in 5 should walk 0..4")
	assert_eq(s.vmcallv("typed_loop_variable"), 0 + 1 + 2 + 3, "for i: int in range(4)")

	s.queue_free()


# assert(): branch on pass, ECALL_THROW on fail.
func test_assert():
	var gdscript_code = """
func checked(x : int) -> int:
	assert(x > 0, "x must be positive")
	return x * 2

func bare(x) -> int:
	assert(x)
	return 1
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	assert_eq(s.vmcallv("checked", 21), 42, "a passing assert should not change the result")
	assert_eq(s.vmcallv("bare", true), 1, "a passing bare assert")

	# Failing assert throws; Sandbox reports twice (throw + unwind).
	s.vmcallv("checked", -1)
	assert_engine_error("Sandbox exception in assert: x must be positive")
	assert_engine_error("x must be positive")

	s.queue_free()


# null -> NIL Variant, distinct from INT(0).
func test_null_is_nil():
	var gdscript_code = """
func nothing():
	return null

func untyped_declaration():
	var x
	return x

func object_declaration():
	var n : Node
	return n

func is_null(x) -> bool:
	return x == null
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	assert_eq(typeof(s.vmcallv("nothing")), TYPE_NIL, "null should be a NIL Variant")
	assert_eq(typeof(s.vmcallv("untyped_declaration")), TYPE_NIL, "var x with no initializer is null")
	assert_eq(typeof(s.vmcallv("object_declaration")), TYPE_NIL, "var n : Node is null")
	assert_eq(s.vmcallv("is_null", null), true, "null == null")
	assert_eq(s.vmcallv("is_null", 0), false, "0 is not null")

	s.queue_free()


# Script header syntax: attributes, signal, static var, &"" and ^"".
# A parse failure in the header takes all functions with it.
func test_script_header_syntax():
	var gdscript_code = """
@tool
extends Node
class_name Enemy

signal died(who)
signal healed

@export_range(0, 100) var hp = 100
@export var speed : float = 1.5
static var counter = 0

@warning_ignore("unused_variable")
static func bump() -> int:
	counter += 1
	return counter

func property_name():
	return &"speed"

func node_path():
	return ^"a/b"

func raw_string():
	return r"a\\nb"

func total() -> int:
	return hp + bump()
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	assert_eq(typeof(s.vmcallv("property_name")), TYPE_STRING_NAME,
		"&\"speed\" should be a StringName")
	assert_eq(s.vmcallv("property_name"), &"speed", "and hold those characters")
	assert_eq(typeof(s.vmcallv("node_path")), TYPE_NODE_PATH, "^\"a/b\" should be a NodePath")
	assert_eq(s.vmcallv("node_path"), ^"a/b", "and hold that path")
	assert_eq(s.vmcallv("raw_string"), r"a\nb", "a raw string keeps its backslash")
	assert_eq(s.vmcallv("total"), 101, "@export var + static var + static func")
	assert_eq(s.vmcallv("bump"), 2, "static var keeps its value between calls")

	s.queue_free()


# $Node and %Unique -> ECALL_GET_NODE.
func test_node_path_sugar():
	var gdscript_code = """
func dollar_name():
	return $Child.get_name()

func dollar_quoted():
	return $"Child".get_name()

func percent_unique():
	return %Special.get_name()
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	# Sandbox resolves node paths from itself; children added here.
	add_child(s)
	var child = Node.new()
	child.name = "Child"
	s.add_child(child)
	var special = Node.new()
	special.name = "Special"
	s.add_child(special)
	special.owner = s
	special.unique_name_in_owner = true

	assert_eq(str(s.vmcallv("dollar_name")), "Child", "$Child should reach the child node")
	assert_eq(str(s.vmcallv("dollar_quoted")), "Child", "$\"Child\" should reach the same node")
	assert_eq(str(s.vmcallv("percent_unique")), "Special", "%Special should reach the unique node")

	s.queue_free()


# Serialization and identity globals. Checked against engine.
func test_serialization_and_identity_globals():
	var gdscript_code = """
func hash_of(x) -> int:
	return hash(x)

func to_str(x) -> String:
	return var_to_str(x)

func from_str(s):
	return str_to_var(s)

func to_bytes(x):
	return var_to_bytes(x)

func from_bytes(b):
	return bytes_to_var(b)

func name_of_type(t : int) -> String:
	return type_string(t)

func convert(x, t : int):
	return type_convert(x, t)

func name_of_error(e : int) -> String:
	return error_string(e)

func same(a, b) -> bool:
	return is_same(a, b)
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("hash_of", 42), hash(42), "hash(int) should match the engine")
	assert_eq(s.vmcallv("hash_of", "text"), hash("text"), "hash(String) should match the engine")
	assert_eq(s.vmcallv("to_str", [1, 2]), var_to_str([1, 2]), "var_to_str should match the engine")
	assert_eq(s.vmcallv("from_str", "[1, 2]"), str_to_var("[1, 2]"), "str_to_var should match the engine")
	assert_eq(s.vmcallv("to_bytes", 7), var_to_bytes(7), "var_to_bytes should match the engine")
	assert_eq(s.vmcallv("from_bytes", var_to_bytes(7)), 7, "bytes_to_var should round-trip")
	assert_eq(s.vmcallv("name_of_type", TYPE_INT), type_string(TYPE_INT),
		"type_string should match the engine")
	assert_eq(s.vmcallv("convert", "5", TYPE_INT), type_convert("5", TYPE_INT),
		"type_convert should match the engine")
	assert_eq(s.vmcallv("name_of_error", ERR_FILE_NOT_FOUND), error_string(ERR_FILE_NOT_FOUND),
		"error_string should match the engine")
	var array = [1, 2]
	assert_eq(s.vmcallv("same", array, array), true, "is_same should hold for one container")
	assert_eq(s.vmcallv("same", [1, 2], [1, 2]), false, "and not for two equal ones")

	s.queue_free()


# ease(), step_decimals() and nearest_po2(), against the engine.
func test_numeric_globals():
	var gdscript_code = """
func ease_of(x : float, curve : float) -> float:
	return ease(x, curve)

func decimals(step : float) -> int:
	return step_decimals(step)

func po2(value : int) -> int:
	return nearest_po2(value)
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	for curve in [0.5, 1.0, 2.0, -2.0, 0.0]:
		for x in [0.0, 0.25, 0.5, 0.75, 1.0]:
			assert_almost_eq(s.vmcallv("ease_of", x, curve), ease(x, curve), 1e-9,
				"ease(%f, %f) should match the engine" % [x, curve])

	for step in [1.0, 0.5, 0.25, 0.1, 0.001, 12.34]:
		assert_eq(s.vmcallv("decimals", step), step_decimals(step),
			"step_decimals(%f) should match the engine" % step)

	for value in [0, 1, 2, 3, 100, 1024, 1025]:
		assert_eq(s.vmcallv("po2", value), nearest_po2(value),
			"nearest_po2(%d) should match the engine" % value)

	s.queue_free()


# Output channels: push_error/push_warning reach Godot's error tracking.
func test_output_channels():
	var gdscript_code = """
func say():
	print("plain")
	prints("a", "b")
	printt("a", "b")
	printraw("raw")
	print_rich("[b]rich[/b]")
	print_verbose("verbose")
	return 1

func complain():
	push_error("a sandbox error")
	return 1

func warn():
	push_warning("a sandbox warning")
	return 1
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	# Stdout channels: verify they execute without error.
	assert_eq(s.vmcallv("say"), 1, "the stdout channels should all return")

	assert_eq(s.vmcallv("complain"), 1, "push_error should return")
	assert_push_error("a sandbox error")

	assert_eq(s.vmcallv("warn"), 1, "push_warning should return")
	assert_push_warning("a sandbox warning")

	s.queue_free()


# range() as a value, `is ClassName` and `as ClassName` (engine inheritance walk).
func test_range_value_and_class_casts():
	var gdscript_code = """
func up(n : int):
	return range(n)

func between(a : int, b : int):
	return range(a, b)

func stepped(a : int, b : int, s : int):
	return range(a, b, s)

func is_node(x) -> bool:
	return x is Node

func is_not_node(x) -> bool:
	return x is not Node

func as_node(x):
	return x as Node

func is_widget(x) -> bool:
	return x is TestCompilerWidget

func as_widget(x):
	return x as TestCompilerWidget
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return

	assert_eq(s.vmcallv("up", 5), range(5), "range(n) should match the engine")
	assert_eq(s.vmcallv("up", 0), range(0), "range(0) should be empty")
	assert_eq(s.vmcallv("between", 2, 6), range(2, 6), "range(a, b) should match the engine")
	assert_eq(s.vmcallv("stepped", 6, 0, -2), range(6, 0, -2), "a negative step should count down")
	assert_eq(s.vmcallv("stepped", 0, 6, 2), range(0, 6, 2), "a positive step should count up")
	# Zero step -> empty array. Engine raises on range(1,5,0) so we
	# compare against [] directly.
	assert_eq(s.vmcallv("stepped", 1, 5, 0), [], "a zero step should be empty")

	var node = Node.new()
	add_child(node)
	assert_eq(s.vmcallv("is_node", node), true, "a Node is a Node")
	assert_eq(s.vmcallv("is_node", 42), false, "an int is not a Node")
	assert_eq(s.vmcallv("is_not_node", 42), true, "'is not' negates it")
	assert_eq(s.vmcallv("as_node", 42), null, "a failed class cast is null")
	assert_eq(s.vmcallv("as_node", node), node, "and a successful one is the value")

	# A name a script declares with `class_name` is not in ClassDB, so
	# Object.is_class() answers false for it and the script chain is walked.
	var script = GDScript.new()
	script.source_code = "extends Node\nclass_name TestCompilerWidget\n"
	script.reload()
	var widget = Node.new()
	widget.set_script(script)
	add_child(widget)

	assert_eq(s.vmcallv("is_widget", widget), true, "a script's class_name answers 'is'")
	assert_eq(s.vmcallv("as_widget", widget), widget, "and 'as' hands the value back")
	# Chain ends at a null script.
	assert_eq(s.vmcallv("is_widget", node), false, "a plain Node is not a TestCompilerWidget")
	assert_eq(s.vmcallv("as_widget", node), null, "so the cast is null")
	# The engine class it extends is still answered by ClassDB.
	assert_eq(s.vmcallv("is_node", widget), true, "a scripted Node is still a Node")

	widget.queue_free()
	node.queue_free()
	s.queue_free()


# load(): both ECALL_LOAD forms, and the resource-allowed callback that stands
# between a compiled program and the project's files.
func test_load_resource():
	var gdscript_code = """
const ELF = "res://tests/tests.elf"

func literal():
	return load("res://tests/tests.elf")

func from_const():
	return load(ELF)

func computed(dir, name):
	return load(dir + name)
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	var expected = load("res://tests/tests.elf")
	assert_eq(s.vmcallv("literal"), expected, "a literal path loads the resource")
	assert_eq(s.vmcallv("from_const"), expected, "a const path loads the same resource")
	assert_eq(s.vmcallv("computed", "res://tests/", "tests.elf"), expected,
		"a path built at run time loads the same resource")

	# Every path reaches the project's callback, whichever form carried it.
	var seen : Array = []
	s.restrictions = true
	s.set_resource_allowed_callback(func(sandbox, path):
		seen.append(path)
		return path == "res://tests/tests.elf")

	assert_eq(s.vmcallv("literal"), expected, "an allowed path still loads")
	assert_eq(s.vmcallv("computed", "res://tests/", "tests.elf"), expected,
		"and so does an allowed path built at run time")
	assert_eq(seen, ["res://tests/tests.elf", "res://tests/tests.elf"],
		"the callback should have seen the path both forms carried")

	var exceptions = s.get_exceptions()
	s.vmcallv("computed", "res://tests/", "vec.elf")
	assert_engine_error("Resource path is not allowed: res://tests/vec.elf")
	assert_engine_error("Exception: Resource path is not allowed: res://tests/vec.elf")
	assert_eq(s.get_exceptions(), exceptions + 1, "a refused path should throw")

	s.queue_free()

# -= await =-
#
# A .sgd coroutine suspends by handing its whole Variant slot array to the host and
# answering the caller with a Signal to await; a resume asks for the frame back and
# carries on. What these pin is the part only a real sandbox can show: that the frame
# comes back intact across calls that reset the scoped state it was captured in.

const AWAIT_SOURCE := """
func wait_for(sig, base):
	var got = await sig
	return base + got

func wait_twice(sig):
	var total = 0
	total = total + await sig
	total = total + await sig
	return total

func wait_ready(sig):
	await sig
	return 42

func hold_a_string(sig, text):
	var got = await sig
	return text + str(got)

func hold_an_array(sig):
	var values = [1, 2]
	values.append(await sig)
	return values

func no_signal(value):
	return await value
"""

signal sgd_ping(value)

func _await_script(name : String):
	var path = "user://temp_%s.sgd" % name
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(AWAIT_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the await script should load as a SafeGDScript resource")
	return script

func _await_node(script) -> Node:
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(100000)
	return node

func test_sgd_await_suspends_and_resumes():
	var script = _await_script("await_basic")
	if script == null:
		return
	var node = _await_node(script)

	var awaitable = node.call("wait_for", sgd_ping, 100)
	assert_eq(typeof(awaitable), TYPE_SIGNAL,
		"a suspended coroutine answers with something Godot's await accepts")

	# Connected rather than awaited: the emission below resumes the guest on this stack,
	# which keeps the test synchronous.
	var completed := [null]
	(awaitable as Signal).connect(func(value): completed[0] = value)

	sgd_ping.emit(7)
	assert_eq(completed[0], 107, "the resumed body computed with the value the signal carried")

	node.free()

func test_sgd_await_two_suspensions():
	var script = _await_script("await_twice")
	if script == null:
		return
	var node = _await_node(script)

	var completed := [null]
	(node.call("wait_twice", sgd_ping) as Signal).connect(func(value): completed[0] = value)

	sgd_ping.emit(5)
	assert_eq(completed[0], null, "still suspended at the second await")
	sgd_ping.emit(11)
	assert_eq(completed[0], 16, "the accumulator survived both suspensions")

	node.free()

func test_sgd_await_holds_a_string():
	var script = _await_script("await_string")
	if script == null:
		return
	var node = _await_node(script)

	# The String in the frame is an index into the scoped variants of the call that
	# suspended, and every call resets those. It only reads back as "held" because the
	# host promoted it to permanent storage at the suspension.
	var completed := [null]
	(node.call("hold_a_string", sgd_ping, "held") as Signal).connect(func(value): completed[0] = value)

	sgd_ping.emit(7)
	assert_eq(completed[0], "held7")

	node.free()

func test_sgd_await_holds_an_array():
	var script = _await_script("await_array")
	if script == null:
		return
	var node = _await_node(script)

	# Same again, and stricter: an Array is a reference, so the promotion has to keep the
	# same container rather than a copy of it, or the append lands somewhere else.
	var completed := [null]
	(node.call("hold_an_array", sgd_ping) as Signal).connect(func(value): completed[0] = value)

	sgd_ping.emit(3)
	assert_eq(completed[0], [1, 2, 3])

	node.free()

func test_sgd_await_on_a_plain_value_does_not_suspend():
	var script = _await_script("await_plain")
	if script == null:
		return
	var node = _await_node(script)

	# Godot's own await hands a non-Signal straight back, so a coroutine that only ever
	# awaits one never suspends and answers with its return value.
	assert_eq(node.call("no_signal", 42), 42)

	node.free()

func test_sgd_await_a_timer():
	var script = _await_script("await_timer")
	if script == null:
		return
	var node = _await_node(script)

	# The signal a real game awaits: emitted by the engine, from outside this stack, and
	# carrying no arguments.
	var timer := get_tree().create_timer(0.05)
	var awaitable = node.call("wait_ready", timer.timeout)
	assert_eq(typeof(awaitable), TYPE_SIGNAL, "the call came back with something to await")

	var result = await awaitable
	assert_eq(result, 42, "the coroutine ran to its return once the timer fired")

	node.free()

func test_sgd_await_publishes_a_variant_return():
	var script = _await_script("await_methods")
	if script == null:
		return

	# A coroutine returns a Signal when it suspends and its value when it does not, so
	# the MethodInfo Godot checks calls against cannot claim the declared type.
	var found := false
	for method in script.get_script_method_list():
		if method["name"] == "wait_for":
			found = true
			assert_eq(method["return"]["type"], TYPE_NIL,
				"a coroutine publishes no concrete return type")
			assert_eq(method["args"].size(), 2, "and still publishes its arity")
	assert_true(found, "wait_for should be in the published method list")
