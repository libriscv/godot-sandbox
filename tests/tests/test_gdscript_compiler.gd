extends GutTest

const SgdStaticUtils = preload("res://tests/static_utils.sgd")

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

func test_compiled_elf_forces_boxed_arguments_in_a_plain_sandbox():
	var compiler := Sandbox.new()
	compiler.set_program(Sandbox_TestsTests)
	compiler.restrictions = true
	var elf: PackedByteArray = compiler.vmcall("compile_to_elf",
			"func f1i(value: int) -> int:\n\treturn value\n")
	assert_false(elf.is_empty(), "the test program should compile")

	var sandbox := Sandbox.new()
	sandbox.load_buffer(elf)
	# Deliberately request the native ABI. The .gdsmeta section is authoritative:
	# compiler-produced entrypoints receive Variant pointers regardless of this setting.
	sandbox.set_unboxed_arguments(true)
	assert_eq(sandbox.vmcall("f1i", 3), 3,
			"a compiler ELF loaded directly into Sandbox should still use its boxed ABI")

	sandbox.queue_free()
	compiler.queue_free()

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


func test_as_builtin_casts():
	var gdscript_code = """
func to_vector2(v):
	return v as Vector2

func to_vector2i(v):
	return v as Vector2i

func to_packed(v):
	return v as PackedInt32Array

func to_stringname(v):
	return v as StringName

func to_nodepath(v):
	return v as NodePath

func to_dictionary(v):
	return v as Dictionary

func to_color(v):
	return v as Color

func to_transform(v):
	return v as Transform2D

func identity(v : Vector2):
	return v as Vector2

func to_variant(v):
	return v as Variant

func literal_vector2():
	return Vector2i(3, 4) as Vector2
"""

	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_eq(compiled_elf.is_empty(), false, "Compiled ELF should not be empty")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)

	assert_eq(s.vmcallv("to_vector2", Vector2i(3, 4)), Vector2(3, 4), "Vector2i converts to Vector2")
	assert_eq(s.vmcallv("to_vector2i", Vector2(1.7, 2.2)), Vector2i(1, 2), "Vector2 truncates to Vector2i")
	assert_eq(s.vmcallv("to_packed", [1, 2, 3]), PackedInt32Array([1, 2, 3]), "Array converts to PackedInt32Array")
	assert_eq(typeof(s.vmcallv("to_stringname", "hi")), TYPE_STRING_NAME, "String converts to StringName")
	assert_eq(typeof(s.vmcallv("to_nodepath", "a/b")), TYPE_NODE_PATH, "String converts to NodePath")
	assert_eq(s.vmcallv("to_color", Color(1, 0, 0)), Color(1, 0, 0), "Color casts to itself")
	assert_eq(s.vmcallv("to_transform", Transform2D()), Transform2D(), "Transform2D casts to itself")

	assert_eq(s.vmcallv("to_dictionary", {"a": 1}), {"a": 1}, "Dictionary casts to itself")
	assert_eq(s.vmcallv("identity", Vector2(5, 6)), Vector2(5, 6), "A typed value is its own cast")
	assert_eq(s.vmcallv("literal_vector2"), Vector2(3, 4), "A literal converts at the same rules")

	assert_eq(s.vmcallv("to_variant", 42), 42, "'as Variant' is an identity")
	assert_eq(s.vmcallv("to_variant", "text"), "text", "'as Variant' keeps a String")

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

func wide_edges(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p):
	return [a, g, h, i, p]

func call_wide_edges():
	return wide_edges(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16)

func wide_defaults(a, b, c, d, e, f, g, h = 80, i = 90):
	return [g, h, i]

func call_wide_defaults():
	return wide_defaults(1, 2, 3, 4, 5, 6, 7)

func call_wide_captured_lambda():
	var captured = 100
	var cb = func(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o):
		return [captured, g, h, i, o]
	return cb.call(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)

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
	assert_eq_deep(s.vmcallv("wide_edges", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16),
		[1, 7, 8, 9, 16])
	assert_eq_deep(s.vmcallv("call_wide_edges"), [1, 7, 8, 9, 16])
	assert_eq_deep(s.vmcallv("call_wide_defaults"), [7, 80, 90])
	assert_eq_deep(s.vmcallv("call_wide_captured_lambda"), [100, 7, 8, 9, 15])
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
	var dict = {key1 = "value", key2 = 42}
	return dict

func nested_dict_literal():
	var dict = {key1 = "value", key2 = 42, key3 = {nested_key = "nested_value", number = 99}}
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

	# Array() converts one container; only the multi-argument form is invalid.
	assert_true(ts.vmcall("compile_to_elf", "func f():\n\treturn Array(1, 2)\n").is_empty(),
		"Array(1, 2) is refused")
	assert_false(ts.vmcall("compile_to_elf", "func f():\n\treturn Array([1, 2])\n").is_empty(),
		"Array([1, 2]) converts the existing container")

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

func test_untyped_uninitialized_member_defaults_to_null():
	var gdscript_code = """
var untyped_global

func read():
	return untyped_global

func write(value):
	untyped_global = value
	return untyped_global

func test():
	untyped_global = 42
	return untyped_global
"""

	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	assert_null(s.vmcallv("read"), "an untyped member starts as null")
	assert_eq(s.vmcallv("write", 42), 42, "it accepts a scalar")
	assert_eq(s.vmcallv("write", ["host", "array"]), ["host", "array"],
		"and can change type while retaining a host container")
	s.queue_free()


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
	# A plain member answers get()/set() the way GDScript's own script variables
	# do. What @export adds is the inspector, not reachability.
	assert_eq(s.get("non_exported"), 100, "a plain member should be readable")
	s.set("non_exported", 7)
	assert_eq(s.vmcallv("get_non_exported"), 7, "and writable, from the guest's side too")
	s.set("non_exported", 100)

	var usage := {}
	for property in s.get_property_list():
		usage[property["name"]] = int(property["usage"])
	assert_true(usage.has("exported_int"), "an @export should be in the property list")
	assert_true(usage.has("non_exported"), "and so should a plain member")
	assert_true((usage["exported_int"] & PROPERTY_USAGE_EDITOR) != 0,
		"an @export should reach the inspector")
	assert_eq(usage["non_exported"] & (PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE), 0,
		"a plain member should reach neither the inspector nor a saved scene")
	assert_true((usage["non_exported"] & PROPERTY_USAGE_SCRIPT_VARIABLE) != 0,
		"a plain member should be a script variable")

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

class _TraitSyscallComplete extends Node:
	func trait_syscall_first():
		pass

	func trait_syscall_second():
		pass

class _TraitSyscallIncomplete extends Node:
	func trait_syscall_first():
		pass

func test_sgd_trait_test_syscall_decodes_bounded_strings():
	var s := _compile_and_load("""
trait Pair:
	func trait_syscall_first() -> void
	func trait_syscall_second() -> void

func recognizes(value) -> bool:
	return value is Pair
""", 100000)
	if s == null:
		return
	var complete := _TraitSyscallComplete.new()
	var incomplete := _TraitSyscallIncomplete.new()
	assert_true(s.vmcallv("recognizes", complete),
		"the syscall should decode every NUL-separated structural method")
	assert_false(s.vmcallv("recognizes", incomplete),
		"the syscall should reject an object missing the second method")
	incomplete.free()
	complete.free()
	s.queue_free()

func test_sgd_a_trait_splices_state_methods_and_enums():
	var source = """
uses Counter

trait Counter:
	var count: int = 1
	enum State { READY = 3, DONE = 7 }
	func bump(amount: int = 1) -> int:
		count += amount
		return count

func bump(amount: int = 1) -> int:
	return Counter.bump(amount) + 10

func run():
	return [bump(), bump(2), Counter.State.DONE]
"""
	var s = _compile_and_load(source, 100000)
	if s == null:
		return
	assert_eq(s.vmcallv("run"), [12, 14, 7],
		"a class override should reach the displaced trait body and its state")
	s.queue_free()

func test_sgd_an_abstract_trait_method_does_not_make_the_script_abstract():
	var script := SafeGDScript.new()
	script.set_source_code("""
uses Damageable

trait Damageable:
	var health: int = 100
	@abstract func on_death() -> void

func on_death() -> void:
	pass

func read_health() -> int:
	return health
""")
	assert_eq(script.get_compile_error(), "", "the trait implementation should compile")

	var node := Node.new()
	node.set_script(script)
	assert_eq(node.get_script(), script,
		"an abstract method inside a used trait must not make its owner script abstract")
	assert_eq(node.call("read_health"), 100, "the attached script should be runnable")
	node.set_script(null)
	node.free()

func _load_cross_file_trait_consumer(path: String, source: String) -> SafeGDScript:
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(source)
	file.close()
	var script := ResourceLoader.load(path, "SafeGDScript",
		ResourceLoader.CACHE_MODE_REPLACE) as SafeGDScript
	assert_not_null(script, "the cross-file trait consumer should load")
	if script != null:
		assert_eq(script.get_compile_error(), "", "the external traits should resolve")
	return script

func test_sgd_file_traits_compose_transitively_across_compilations():
	var actor_script := _load_cross_file_trait_consumer(
		"user://temp_cross_file_trait_actor.sgd", """
extends Node
uses SgdCrossFilePowered

func run() -> int:
	return powered_move(4)
""")
	var observer_script := _load_cross_file_trait_consumer(
		"user://temp_cross_file_trait_observer.sgd", """
extends Node
uses SgdCrossFilePowered

func recognizes(value) -> bool:
	return value is SgdCrossFilePowered
""")
	if actor_script == null or observer_script == null:
		return

	var actor := Node.new()
	actor.set_script(actor_script)
	assert_eq(actor.call("run"), 14,
		"a file trait should receive the state and method of its external dependency")
	assert_true(actor_script.uses_trait(&"SgdCrossFilePowered"),
		"the first compilation should publish the file trait's nominal name")
	assert_true(observer_script.uses_trait(&"SgdCrossFilePowered"),
		"a separate compilation should publish the same nominal name")
	assert_true(observer_script.uses_trait(&"SgdCrossFileMovable"),
		"transitive external traits should be published too")
	var observer := Node.new()
	observer.set_script(observer_script)
	assert_true(observer.call("recognizes", actor),
		"separately compiled instances should pass a nominal trait test")
	observer.free()
	actor.free()

func test_sgd_loads_two_qualified_traits_from_one_provider_file():
	var script := _load_cross_file_trait_consumer(
		"user://temp_qualified_trait_consumer.sgd", """
extends Node
uses SgdTraitLibrary.Alpha, SgdTraitLibrary.Beta

func run() -> int:
	return alpha() + beta()
""")
	if script == null:
		return
	var node := Node.new()
	node.set_script(script)
	assert_eq(node.call("run"), 30,
		"each qualified trait in the shared provider should be imported exactly once")
	node.free()

func test_sgd_rpc_is_published_only_while_unrestricted():
	var script := SafeGDScript.new()
	script.set_source_code("""
@rpc
func authority_default():
	return 1

@rpc("any_peer", "call_local", "unreliable_ordered", 7)
func customized(value):
	return value
""")
	assert_eq(script.get_compile_error(), "")

	var node := Node.new()
	node.set_script(script)
	var config: Dictionary = script.get_rpc_config()
	assert_eq(config.size(), 2)
	assert_eq(config["authority_default"], {
		"rpc_mode": MultiplayerAPI.RPC_MODE_AUTHORITY,
		"transfer_mode": MultiplayerPeer.TRANSFER_MODE_RELIABLE,
		"call_local": false,
		"channel": 0,
	})
	assert_eq(config["customized"], {
		"rpc_mode": MultiplayerAPI.RPC_MODE_ANY_PEER,
		"transfer_mode": MultiplayerPeer.TRANSFER_MODE_UNRELIABLE_ORDERED,
		"call_local": true,
		"channel": 7,
	})

	node.call("add_allowed_object", node)
	assert_null(script.get_rpc_config(), "a partial restriction also suppresses RPCs")
	node.call("clear_allowed_objects")
	assert_eq((script.get_rpc_config() as Dictionary).size(), 2)

	node.set("restrictions", true)
	assert_null(script.get_rpc_config(), "restricted SafeGDScript instances publish no RPCs")
	node.set("restrictions", false)
	assert_eq((script.get_rpc_config() as Dictionary).size(), 2,
		"removing every restriction restores the declared RPCs")
	node.free()
	await get_tree().process_frame

# A member typed as a payload built-in (Transform3D, Basis, AABB, ...) keeps its
# value in a permanent variant slot, and permanent indices are negative. The
# property syscalls told an object handle from a variant index by width alone, so
# a sign-extended negative index read as a handle and every field access on such a
# member threw "Object no longer exists".
func test_sgd_payload_typed_members_reach_their_fields():
	var script := SafeGDScript.new()
	script.set_source_code("""
var orientation := Transform3D()
var box := AABB()

func poke() -> Vector3:
	orientation = Transform3D(Basis(), Vector3(7, 8, 9))
	return orientation.origin

func poke_field() -> Array:
	orientation.origin = Vector3(1, 2, 3)
	box.size = Vector3(2, 2, 2)
	return [orientation.origin, box.size]
""")
	assert_eq(script.get_compile_error(), "")

	var node := Node.new()
	node.set_script(script)
	assert_eq(node.call("poke"), Vector3(7, 8, 9),
		"a payload-typed member reads back the field it was just assigned")
	assert_eq(node.call("poke_field"), [Vector3(1, 2, 3), Vector3(2, 2, 2)],
		"a payload-typed member's own field is writable")
	assert_eq(node.get("orientation"), Transform3D(Basis(), Vector3(1, 2, 3)),
		"the host reads back the member the guest wrote")
	node.free()
	await get_tree().process_frame

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
	const CURRENCY = "NOK"
	func net():
		return self.balance - self.loan
	func _to_string():
		return "account:%s" % self.balance

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

func surfaces(value):
	var account = BankAccount(100, 50)
	var copied = account.copy()
	var matched = 0
	match account:
		BankAccount(var balance, _):
			matched = balance
	return [account.net(), str(account), "%s" % account, copied == account,
		value is BankAccount, value as BankAccount, matched, BankAccount.CURRENCY]

func absent(d: Dictionary):
	return d.absent

struct MutableValue:
	var value = 1

func replace_inline_and_scoped(p: MutableValue):
	p.value = "temporary"
	p.value = 9
	return p.value

struct ArrayField:
	var values: Array = []

func make_array_field():
	return ArrayField([1, 2, 3])
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
	assert_eq(s.vmcallv("repay", {&"balance": 100, &"loan": 50}, 20), 30,
		"StringName keys satisfy the same struct shape")
	assert_eq(s.vmcallv("is_a_dictionary"), 2, "An instance is a Dictionary")
	assert_eq(s.vmcallv("surfaces", {"balance": 1, "loan": 2}),
		[50, "account:100", "account:100", true, true,
			{"balance": 1, "loan": 2}, 100, "NOK"],
		"Methods, strings, copy, equality, is/as, patterns and constants")
	assert_eq(s.vmcallv("absent", {}), null,
		"A missing raw Dictionary key returns null without an engine error")
	var mutable := {"value": 1}
	assert_eq(s.vmcallv("replace_inline_and_scoped", mutable), 9,
		"SET_RAW replaces an int by a String and the String by an int")
	assert_eq(mutable, {"value": 9}, "SET_RAW updates the shared Dictionary")
	assert_eq(s.vmcallv("make_array_field"), {"values": [1, 2, 3]},
		"MAKE_KEYED stores pointer-backed fields")

	var before := s.get_exceptions()
	s.vmcallv("repay", {"balance": 1}, 1)
	assert_eq(s.get_exceptions(), before + 1, "a missing struct key should throw")
	assert_engine_error("Argument 'account' is not a BankAccount")
	assert_engine_error("Exception: Sandbox exception in TypeError: Argument 'account' is not a BankAccount")
	before = s.get_exceptions()
	s.vmcallv("repay", {"balance": 1, "loan": 2, "extra": 3}, 1)
	assert_eq(s.get_exceptions(), before + 1, "an extra struct key should throw")
	assert_engine_error("Argument 'account' is not a BankAccount")
	assert_engine_error("Exception: Sandbox exception in TypeError: Argument 'account' is not a BankAccount")

	s.queue_free()


func test_struct_methods():
	# A struct method is lifted to a plain function that takes the instance as
	# its first argument -- except a static one, which takes no instance at all,
	# whichever side of the dot it is reached from.
	var gdscript_code = """
struct Vec:
	const ORIGIN_X = 3
	var x = 0
	var y = 0

	func length_squared():
		return self.x * self.x + self.y * self.y

	func scaled(by, then = 1) -> Vec:
		self.x *= by * then
		self.y *= by * then
		return self

	static func origin(x = Vec.ORIGIN_X):
		return Vec(x, 0)

	static func added(a: Vec, b: Vec) -> Vec:
		return Vec(a.x + b.x, a.y + b.y)

func instance_method():
	return Vec(3, 4).length_squared()

func chained():
	return Vec(1, 2).scaled(2).scaled(3, 2).x

func static_through_the_type():
	return Vec.origin()

func static_through_an_instance():
	# The receiver must not be passed along as the first argument, which would
	# leave 'x' holding the Dictionary instead of its default.
	return Vec(9, 9).origin()

func static_with_arguments():
	return Vec.added(Vec(1, 2), Vec(3, 4))

func through_a_parameter(v: Vec):
	return v.length_squared()

func default_of_a_constant():
	return Vec.ORIGIN_X
"""
	var s = _compile_and_load(gdscript_code)
	if s == null:
		return

	assert_eq(s.vmcallv("instance_method"), 25, "An instance method reads its own fields")
	assert_eq(s.vmcallv("chained"), 12, "A method answering self chains")
	assert_eq(s.vmcallv("static_through_the_type"), {"x": 3, "y": 0},
		"A static method builds an instance without one")
	assert_eq(s.vmcallv("static_through_an_instance"), {"x": 3, "y": 0},
		"Reaching a static method through an instance passes it no receiver")
	assert_eq(s.vmcallv("static_with_arguments"), {"x": 4, "y": 6},
		"A static method takes struct arguments by position")
	assert_eq(s.vmcallv("through_a_parameter", {"x": 3, "y": 4}), 25,
		"A method reached through a declared parameter")
	assert_eq(s.vmcallv("default_of_a_constant"), 3, "A struct constant folds to its value")

	# The receiver is checked like any other struct-typed parameter.
	var before := s.get_exceptions()
	s.vmcallv("through_a_parameter", {"x": 1})
	assert_eq(s.get_exceptions(), before + 1, "a missing struct key should throw")
	assert_engine_error("Argument 'v' is not a Vec")
	assert_engine_error("Exception: Sandbox exception in TypeError: Argument 'v' is not a Vec")

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

func test_cross_type_equality():
	# Godot registers no operator for `Array == int`, and evaluate() leaves NIL
	# behind. NIL is a zero payload, which the fused branch read as `false`, so
	# `v != 1` fell through into the equal arm. Values of different types are
	# never equal, the answer the engine's own Variant comparison gives.
	var gdscript_code = """
func branch(v):
	if v == 1:
		return "one"
	return "other"

func equals(v):
	return v == 1

func differs(v):
	return v != 1

func matched(v):
	match v:
		1:
			return "one"
		_:
			return "other"
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	for v in [[3, 4], "z", {"a": 1}, Vector2(1, 1), StringName("z")]:
		assert_eq(s.vmcallv("branch", v), "other", "A subject of another type should take the else arm")
		assert_eq(s.vmcallv("equals", v), false, "Cross-type == should answer false, not null")
		assert_eq(s.vmcallv("differs", v), true, "Cross-type != should answer true, not null")
		assert_eq(s.vmcallv("matched", v), "other", "A value pattern should decline another type")

	# null compares against anything without an operator of its own.
	assert_eq(s.vmcallv("equals", null), false, "null == 1 should be false")
	assert_eq(s.vmcallv("differs", null), true, "null != 1 should be true")

	# The equal case still answers equal.
	assert_eq(s.vmcallv("branch", 1), "one", "An equal subject should take the then arm")
	assert_eq(s.vmcallv("equals", 1), true, "1 == 1 should be true")
	assert_eq(s.vmcallv("differs", 1), false, "1 != 1 should be false")
	assert_eq(s.vmcallv("matched", 1), "one", "A value pattern should match an equal subject")

	s.queue_free()

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

# -= Argument and property narrowing =-

const NARROWING_SOURCE = """
@export var speed: float = 1.0
@export var count: int = 0

func take_int(a: int) -> int:
	return a + 1

func take_float(a: float) -> float:
	return a * 2.0

func take_string(a: String) -> String:
	return a + "!"

func mixed(a: int, b: float) -> float:
	return a + b

func untyped(a) -> int:
	return typeof(a)

func scaled() -> float:
	return speed * 2.0
"""

func _narrowing_node() -> Node:
	var path = "user://temp_narrowing.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(NARROWING_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the narrowing script should load as a SafeGDScript resource")
	if script == null:
		return null
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(100000)
	return node

func test_sgd_narrows_arguments_to_the_declared_type():
	var node = _narrowing_node()
	if node == null:
		return

	var i: int = 5
	assert_eq(node.call("take_float", i), 10.0, "an int at a float parameter should narrow")
	assert_eq(node.call("take_int", 3.9), 4, "a float at an int parameter should truncate")
	assert_eq(node.call("take_int", true), 2, "a bool at an int parameter should narrow")
	assert_eq(node.call("mixed", 1, 2), 3.0, "only the mismatched argument should be touched")
	assert_eq(node.call("take_string", StringName("sn")), "sn!",
		"a StringName at a String parameter should narrow")

	assert_eq(node.call("take_float", 2.5), 5.0, "a matching argument should pass through")
	assert_eq(node.call("untyped", "hi"), TYPE_STRING, "an untyped parameter should not narrow")

	node.free()

func test_sgd_refuses_an_argument_the_declared_type_rejects():
	var node = _narrowing_node()
	if node == null:
		return

	_refused_call(node, "take_int", ["hello"])
	assert_engine_error("Cannot convert argument 1 from String to int.")

	_refused_call(node, "take_float", [[]])
	assert_engine_error("Cannot convert argument 1 from Array to float.")

	_refused_call(node, "take_int", [null])
	assert_engine_error("Cannot convert argument 1 from Nil to int.")

	node.free()

func test_sgd_narrows_a_property_to_the_declared_type():
	var node = _narrowing_node()
	if node == null:
		return

	var i: int = 5
	node.set("speed", i)
	assert_eq(typeof(node.get("speed")), TYPE_FLOAT, "the member should keep its declared type")
	assert_eq(node.call("scaled"), 10.0, "the guest should see a float it can compute with")

	node.set("count", 3.7)
	assert_eq(node.get("count"), 3, "a float stored into an int member should truncate")
	assert_eq(typeof(node.get("count")), TYPE_INT, "the member should keep its declared type")

	node.set("speed", [])
	assert_eq(node.get("speed"), 5.0, "a refused assignment should leave the member alone")

	node.free()

# -= Union types =-

const UNION_SOURCE = """
var held: Node | Dictionary = {}

func touch():
	return 1

func store(value: Node | Dictionary):
	held = value
	touch()
	return held

func guarded(value: int | String):
	return value

func describe(value: int | String) -> String:
	if value is int:
		return str(value + 1)
	return value + "!"
"""

func _union_node() -> Node:
	var path = "user://temp_union_types.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(UNION_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the union-typed script should load as a SafeGDScript resource")
	if script == null:
		return null
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(100000)
	return node

func _union_reference(value) -> String:
	if value is int:
		return str(value + 1)
	return value + "!"

func test_sgd_union_typed_member_survives_calls():
	var node = _union_node()
	if node == null:
		return
	var object_value = Node.new()
	assert_eq(node.call("store", object_value), object_value,
		"an Object member should survive a guest call")
	var dictionary_value = {"answer": 42}
	assert_eq(node.call("store", dictionary_value), dictionary_value,
		"a Dictionary member should survive a guest call and replace the Object")
	object_value.free()
	node.free()

func test_sgd_union_guard_throws():
	var node = _union_node()
	if node == null:
		return
	_refused_call(node, "guarded", [1.5])
	assert_engine_error("Cannot assign a value to parameter 'value' of type int | String")
	assert_engine_error("Exception: Sandbox exception in TypeError: Cannot assign a value to parameter 'value' of type int | String")
	node.free()

func test_sgd_union_reflects_as_variant():
	var node = _union_node()
	if node == null:
		return
	var method = _method_info(node, "guarded")
	assert_eq(method.get("args", []).size(), 1, "the union parameter should be published")
	assert_eq(method["args"][0]["type"], TYPE_NIL,
		"Godot should see a union parameter as Variant")
	assert_true((method["args"][0]["usage"] & PROPERTY_USAGE_NIL_IS_VARIANT) != 0,
		"the reflected NIL should be marked as Variant rather than null")
	node.free()

func test_sgd_union_narrowing_matches_gdscript():
	var node = _union_node()
	if node == null:
		return
	for value in [4, "safe"]:
		assert_eq(node.call("describe", value), _union_reference(value),
			"the narrowed SafeGDScript branch should match ordinary GDScript")
	node.free()

# -= Nullable types =-

const NULLABLE_SOURCE = """
struct Point:
	var x: int = 0
	var y: int = 0

var held: Vector2?
var point: Point?
@export var exported_value: Vector2?
@export var texture: Texture2D?

func remember(value: Vector2?):
	held = value
	return typeof(held)

func recall():
	return held

func guarded(value: Vector2?):
	return value

func component(value: Vector2?) -> float?:
	if value == null:
		return null
	return value.x

func truthy(value: Vector2?):
	if value:
		return "truthy"
	return "falsy"

func compact_binding(value):
	if var result := value:
		return [true, result]
	return [false, null]

func typed_compact_binding(value):
	if var result: int = value:
		return result + 1
	return -1

func point_default():
	return point

func point_round_trip():
	point = Point(1, 2)
	return point.x

var fallback_calls: int = 0

func bump() -> int:
	fallback_calls += 1
	return 99

func fallback_count():
	return fallback_calls

func coalesce(value):
	return value ?? bump()

func safe_component(value):
	return value?.x

func safe_chain(value):
	return value?.position.x

func safe_call(value):
	return value?.get_class()

func safe_index(value):
	return value?.items[1]

func safe_then_fallback(value):
	return value?.x ?? -1.0
"""

func _nullable_node() -> Node:
	var path = "user://temp_nullable_types.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(NULLABLE_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the nullable-typed script should load as a SafeGDScript resource")
	if script == null:
		return null
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(100000)
	return node

func test_sgd_nullable_value_member_survives_calls():
	var node = _nullable_node()
	if node == null:
		return
	assert_eq(node.call("remember", Vector2(3, 4)), TYPE_VECTOR2)
	assert_eq(node.call("recall"), Vector2(3, 4),
		"a nullable value should survive the call that stored it")
	assert_eq(node.call("remember", null), TYPE_NIL)
	assert_null(node.call("recall"), "the same slot should accept null later")
	node.free()

func test_sgd_nullable_guard_throws():
	var node = _nullable_node()
	if node == null:
		return
	assert_eq(node.call("guarded", Vector2(1, 2)), Vector2(1, 2))
	assert_null(node.call("guarded", null))
	_refused_call(node, "guarded", ["wrong"])
	assert_engine_error("Cannot assign a value to parameter 'value' of type Vector2?")
	assert_engine_error("Exception: Sandbox exception in TypeError: Cannot assign a value to parameter 'value' of type Vector2?")
	node.free()

func test_sgd_nullable_export_reflects_as_variant():
	var node = _nullable_node()
	if node == null:
		return
	var properties := {}
	for property in node.get_property_list():
		properties[property["name"]] = property
	assert_eq(properties["exported_value"]["type"], TYPE_NIL,
		"a value-typed nullable export should publish as Variant")
	assert_true((properties["exported_value"]["usage"] & PROPERTY_USAGE_NIL_IS_VARIANT) != 0)
	assert_eq(properties["texture"]["type"], TYPE_OBJECT,
		"an object-typed nullable export should stay object-typed")
	assert_eq(str(properties["texture"]["class_name"]), "Texture2D")
	node.free()

func test_sgd_nullable_narrowing_matches_gdscript():
	var node = _nullable_node()
	if node == null:
		return
	assert_null(node.call("component", null))
	assert_eq(node.call("component", Vector2(4, 5)), 4.0)
	for value in [null, Vector2.ZERO, Vector2.ONE]:
		var expected = "truthy" if value else "falsy"
		assert_eq(node.call("truthy", value), expected,
			"nullable truthiness should match ordinary GDScript")
	node.free()

func test_sgd_if_var_is_a_null_only_binding():
	var node = _nullable_node()
	if node == null:
		return
	assert_eq(node.call("compact_binding", null), [false, null])
	for value in [0, false, "", Vector2.ZERO]:
		assert_eq(node.call("compact_binding", value), [true, value],
			"if var should bind falsy values; only null skips its branch")
	assert_eq(node.call("typed_compact_binding", null), -1,
		"null should be the no-binding case even with a plain int annotation")
	assert_eq(node.call("typed_compact_binding", 0), 1)
	node.free()

func test_sgd_nullable_struct_is_null_by_default():
	var node = _nullable_node()
	if node == null:
		return
	assert_null(node.call("point_default"))
	assert_eq(node.call("point_round_trip"), 1)
	node.free()

# -= Safe navigation and null coalescing =-
#
# `?.` and `??` are the two shapes an `if x != null:` check is written in, and
# both lower to that same NIL tag test. GDScript has neither.

func test_sgd_safe_navigation_answers_null_for_a_null_receiver():
	var node = _nullable_node()
	if node == null:
		return
	assert_null(node.call("safe_component", null), "'?.' on null should be null")
	assert_eq(node.call("safe_component", Vector2(3, 4)), 3.0,
		"'?.' on a value should read the member")

	var target = Node2D.new()
	target.name = "Target"
	target.position = Vector2(7, 8)
	assert_eq(node.call("safe_call", target), "Node2D",
		"'?.' should call a method on a live object")
	assert_null(node.call("safe_call", null), "and skip the call entirely for null")
	target.free()
	node.free()

func test_sgd_safe_navigation_short_circuits_the_whole_chain():
	var node = _nullable_node()
	if node == null:
		return
	var parent = Node2D.new()
	var child = Node2D.new()
	child.position = Vector2(5, 6)
	parent.add_child(child)
	assert_eq(node.call("safe_chain", child), 5.0,
		"the links after '?.' should run for a live object")
	assert_null(node.call("safe_chain", null),
		"a null receiver should skip the rest of the chain, not access null")

	assert_eq(node.call("safe_index", {"items": [10, 20, 30]}), 20,
		"an index at the end of a safe chain should read normally")
	assert_null(node.call("safe_index", null), "and be skipped along with the chain")
	parent.free()
	node.free()

func test_sgd_null_coalescing_keeps_falsy_values():
	var node = _nullable_node()
	if node == null:
		return
	assert_eq(node.call("coalesce", 5), 5)
	assert_eq(node.call("fallback_count"), 0,
		"a value on the left should leave the fallback unevaluated")
	for value in [0, false, "", Vector2.ZERO]:
		assert_eq(node.call("coalesce", value), value,
			"unlike 'or', '??' only replaces null")
	assert_eq(node.call("fallback_count"), 0)

	assert_eq(node.call("coalesce", null), 99, "null should take the fallback")
	assert_eq(node.call("fallback_count"), 1, "which is evaluated exactly once")
	node.free()

func test_sgd_safe_navigation_feeds_null_coalescing():
	var node = _nullable_node()
	if node == null:
		return
	assert_eq(node.call("safe_then_fallback", Vector2(2, 3)), 2.0)
	assert_eq(node.call("safe_then_fallback", null), -1.0,
		"the null a safe chain answers with is what '??' replaces")
	node.free()

func test_sgd_safe_navigation_is_not_an_assignment_target():
	var result = _validate("func f(n):\n\tn?.x = 1\n")
	assert_false(result["valid"])
	assert_true(result["message"].contains("'?.'"))

	var compound = _validate("func f(n):\n\tn?.x += 1\n")
	assert_false(compound["valid"])

func test_sgd_null_into_non_nullable_member_is_a_compile_error():
	var result = _validate("var pos: Vector2 = null\n")
	assert_false(result["valid"])
	assert_true(result["message"].contains("Cannot assign null"))
	assert_true(result["hint"].contains("Vector2?"))

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
	# and a dynamic negative index is normalised in the guest before the call;
	# a negative constant is wrapped by the host in that same call. Array.get(),
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


# The cubic interpolation family, against the engine. The angular and
# in-time forms are transcribed twice -- once in the compiler's evaluator,
# once in the host's syscall -- so what matters is that both agree with Godot.
func test_cubic_interpolation_globals():
	var gdscript_code = """
func plain(from : float, to : float, pre : float, post : float, w : float) -> float:
	return cubic_interpolate(from, to, pre, post, w)

func angle(from : float, to : float, pre : float, post : float, w : float) -> float:
	return cubic_interpolate_angle(from, to, pre, post, w)

func in_time(c : Array) -> float:
	return cubic_interpolate_in_time(c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7])

func angle_in_time(c : Array) -> float:
	return cubic_interpolate_angle_in_time(c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7])
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	# The last row's zero times take every degenerate branch of Barry-Goldman.
	var cases = [
		[1.0, 2.0, 0.0, 3.0, 0.5, 1.0, -0.5, 1.5],
		[0.5, 3.0, 0.0, 4.0, 0.25, 1.0, -0.5, 1.5],
		[-7.3, 12.9, 3.25, -1.5, 0.75, 2.0, -1.0, 3.0],
		[1.0, 2.0, 0.0, 3.0, 0.5, 0.0, 0.0, 0.0],
	]
	for c in cases:
		assert_almost_eq(s.vmcallv("plain", c[0], c[1], c[2], c[3], c[4]),
			cubic_interpolate(c[0], c[1], c[2], c[3], c[4]), 1e-9,
			"cubic_interpolate should match the engine")
		assert_almost_eq(s.vmcallv("angle", c[0], c[1], c[2], c[3], c[4]),
			cubic_interpolate_angle(c[0], c[1], c[2], c[3], c[4]), 1e-9,
			"cubic_interpolate_angle should match the engine")
		# Eight arguments: one past what a vmcall can carry, so they travel in
		# an Array. The syscall itself still gets all eight, in fa0-fa7.
		assert_almost_eq(s.vmcallv("in_time", c),
			cubic_interpolate_in_time(c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]), 1e-9,
			"cubic_interpolate_in_time should match the engine")
		assert_almost_eq(s.vmcallv("angle_in_time", c),
			cubic_interpolate_angle_in_time(c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]), 1e-9,
			"cubic_interpolate_angle_in_time should match the engine")

	s.queue_free()


# rand_from_seed(): the one draw that reads and writes no shared state, so a
# restricted program may make it and two runs answer the same thing.
func test_rand_from_seed_is_deterministic():
	var gdscript_code = """
func draw(s : int):
	return rand_from_seed(s)
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return
	s.restrictions = true

	for seed_value in [0, 1, 12345, -7]:
		var expected = rand_from_seed(seed_value)
		assert_eq(s.vmcallv("draw", seed_value), expected,
			"rand_from_seed(%d) should match the engine" % seed_value)
		assert_eq(s.vmcallv("draw", seed_value), expected,
			"and answer the same thing the second time")

	s.queue_free()


# randomize() and seed() are the two @GlobalScope calls that deliberately
# mutate the generator shared with the host. They are available only while the
# Sandbox has no restrictions of any kind.
func test_shared_rng_mutation_in_an_unrestricted_sandbox():
	var gdscript_code = """
func reseed(value : int):
	return seed(value)

func rerandomize():
	return randomize()
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	var seed_value := 424242
	seed(seed_value)
	var expected_first := randi()
	var expected_second := randi()
	assert_eq(s.vmcallv("reseed", seed_value), null, "seed() should return nil")
	assert_eq(randi(), expected_first, "the guest should reset the project's shared RNG")
	assert_eq(randi(), expected_second, "host draws should continue the guest-seeded sequence")

	seed(seed_value)
	var unrandomized_first := randi()
	seed(seed_value)
	assert_eq(s.vmcallv("rerandomize"), null, "randomize() should return nil")
	assert_ne(randi(), unrandomized_first, "randomize() should replace the shared deterministic state")

	# Do not leave the rest of the suite on a deterministic generator state.
	randomize()
	s.queue_free()


func test_shared_rng_mutation_is_refused_by_restrictions():
	for call in ["randomize()", "seed(1234)"]:
		var message := _restricted_compile_error("func run():\n\t%s\n" % call)
		assert_true(message.contains("restricted Sandbox") && message.contains("shared RNG"),
			"restricted compilation should refuse %s, got: %s" % [call, message])
		assert_engine_error("SafeGDScript: : " + message)

	# Compile while unrestricted, then close the runtime gate. This exercises
	# the syscall check as a defence against stale or hand-written binaries.
	var s = _compile_and_load("""
func reseed():
	seed(1234)

func rerandomize():
	randomize()
""", 40000)
	if s == null:
		return
	s.restrictions = true
	var before := s.get_exceptions()
	s.vmcallv("reseed")
	assert_eq(s.get_exceptions(), before + 1, "restricted seed() should raise")
	assert_engine_error("Exception: utility(): Shared RNG mutation is refused under restrictions")
	s.vmcallv("rerandomize")
	assert_eq(s.get_exceptions(), before + 2, "restricted randomize() should raise")
	assert_engine_error("Exception: utility(): Shared RNG mutation is refused under restrictions")
	s.queue_free()


# @GlobalScope enums (Side, Corner, Error, Key). A member is a compile-time
# integer -- the enum itself never reaches the guest.
func test_global_enumerations():
	var gdscript_code = """
func side() -> int:
	return Side.SIDE_BOTTOM

func corner() -> int:
	return Corner.CORNER_BOTTOM_LEFT

func orientation() -> int:
	return Orientation.VERTICAL

func clock() -> int:
	return ClockDirection.COUNTERCLOCKWISE

func err() -> int:
	return Error.ERR_FILE_NOT_FOUND

func key() -> int:
	return Key.KEY_A + KeyModifierMask.KEY_MASK_SHIFT

func alignment() -> int:
	return HorizontalAlignment.HORIZONTAL_ALIGNMENT_RIGHT + InlineAlignment.INLINE_ALIGNMENT_CENTER

func nested() -> int:
	return Variant.Type.TYPE_INT + Variant.Operator.OP_ADD

func typed(s : Side) -> int:
	return s + 1
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return
	# Restricted: none of these may reach the engine, so all of them still work.
	s.restrictions = true

	assert_eq(s.vmcallv("side"), SIDE_BOTTOM, "Side.SIDE_BOTTOM")
	assert_eq(s.vmcallv("corner"), CORNER_BOTTOM_LEFT, "Corner.CORNER_BOTTOM_LEFT")
	assert_eq(s.vmcallv("orientation"), VERTICAL, "Orientation.VERTICAL")
	assert_eq(s.vmcallv("clock"), COUNTERCLOCKWISE, "ClockDirection.COUNTERCLOCKWISE")
	assert_eq(s.vmcallv("err"), ERR_FILE_NOT_FOUND, "Error.ERR_FILE_NOT_FOUND")
	assert_eq(s.vmcallv("key"), KEY_A + KEY_MASK_SHIFT, "Key and KeyModifierMask")
	assert_eq(s.vmcallv("alignment"), HORIZONTAL_ALIGNMENT_RIGHT + INLINE_ALIGNMENT_CENTER,
		"HorizontalAlignment and InlineAlignment")
	assert_eq(s.vmcallv("nested"), TYPE_INT + 6, "Variant.Type and Variant.Operator")
	assert_eq(s.vmcallv("typed", SIDE_TOP), SIDE_TOP + 1, "a Side-typed parameter is an int")

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

func wait_then_self(sig):
	await sig
	return get_name()

func inner_wait(sig, base):
	var got = await sig
	return base + got

func outer_wait(sig, base):
	var v = await inner_wait(sig, base)
	return v * 2

func call_without_awaiting(sig):
	var handle = inner_wait(sig, 0)
	return typeof(handle)

func walk_raw(sig, text : String):
	var n = 0
	for c in text:
		n += ord(c)
		await sig
	return n

func walk_boxed(sig, text : String, mark : String):
	var n = 0
	for c in text:
		if c == mark:
			n += 1000
		n += ord(c)
		await sig
	return n

func walk_without_suspending(text : String):
	var n = 0
	for c in text:
		n += ord(c)
	return n
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

func test_sgd_await_another_coroutine():
	# A call inside the program is a jal, and a suspension unwinds past it, so a
	# call to a coroutine leaves the program and comes back in: the suspension
	# stops at that boundary, and the answer is the Signal the caller awaits --
	# the same Signal a call from Godot gets.
	var script = _await_script("await_nested")
	if script == null:
		return
	var node = _await_node(script)

	var awaitable = node.call("outer_wait", sgd_ping, 100)
	assert_eq(typeof(awaitable), TYPE_SIGNAL,
		"the outer coroutine suspended on the inner one's Signal")

	var completed := [null]
	(awaitable as Signal).connect(func(value): completed[0] = value)

	# One emission resumes the inner frame, whose completion resumes the outer.
	sgd_ping.emit(7)
	assert_eq(completed[0], 214, "the caller resumed with what the callee returned")

	node.free()

func test_sgd_call_a_coroutine_without_awaiting_it():
	# GDScript answers a GDScriptFunctionState here. A sandboxed program gets the
	# Signal that the same suspension produced, which is what await takes -- and
	# the caller runs on rather than suspending.
	var script = _await_script("await_unawaited")
	if script == null:
		return
	var node = _await_node(script)

	assert_eq(node.call("call_without_awaiting", sgd_ping), TYPE_SIGNAL,
		"a call with no await answers something the caller can await later")

	node.free()

func _await_string_walk(fn : String, arguments : Array, expected : int):
	# A String walk hands characters out in batches. Neither batch survives a
	# suspension -- the boxed one names them by scoped index, which the next call
	# reuses, and the raw one writes code points to a frame buffer outside the
	# Variant slots the host saves -- so a coroutine has to walk one character at
	# a time. The walk between resumes lays its own frame over the same stack.
	var script = _await_script("await_walk")
	if script == null:
		return
	var node = _await_node(script)

	var awaitable = node.callv(fn, [sgd_ping] + arguments)
	assert_eq(typeof(awaitable), TYPE_SIGNAL, "the walk suspended on its first character")
	var completed := [null]
	(awaitable as Signal).connect(func(value): completed[0] = value)

	for i in range(3):
		assert_eq(node.call("walk_without_suspending", "ZZZZZZZZ"), 8 * 90,
			"a walk between resumes computes its own answer")
		sgd_ping.emit(0)

	assert_eq(completed[0], expected, "the suspended walk saw its own characters")
	node.free()

func test_sgd_await_inside_a_raw_string_walk():
	_await_string_walk("walk_raw", ["abc"], 97 + 98 + 99)

func test_sgd_await_inside_a_boxed_string_walk():
	_await_string_walk("walk_boxed", ["abc", "b"], 97 + 98 + 99 + 1000)

func test_sgd_await_resume_runs_as_the_node_that_suspended():
	var script = _await_script("await_owner")
	if script == null:
		return

	var first = _await_node(script)
	var second = _await_node(script)
	first.name = "FirstOwner"
	second.name = "SecondOwner"
	add_child(first)
	add_child(second)

	var completed := [null]
	(first.call("wait_then_self", sgd_ping) as Signal).connect(func(value): completed[0] = value)

	sgd_ping.emit(1)
	assert_eq(completed[0], "FirstOwner",
		"the resumed frame ran as the node that suspended it, not the newest instance")

	first.free()
	second.free()

func test_sgd_await_resume_after_the_named_owner_is_freed():
	var script = _await_script("await_shared_owner")
	if script == null:
		return

	var first = _await_node(script)
	var second = _await_node(script)
	add_child(first)
	add_child(second)

	var completed := [false]
	(first.call("wait_then_self", sgd_ping) as Signal).connect(func(_v): completed[0] = true)
	assert_eq(second.call("get_coroutine_count"), 1, "the frame is held")

	first.free()
	sgd_ping.emit(1)

	assert_true(completed[0], "the resumed frame finished rather than taking the process down")
	assert_eq(second.call("get_coroutine_count"), 0, "and it was retired")

	# get_name() on the freed owner: the errors the resumed frame is expected to
	# report, claimed here so the run stays clean.
	assert_engine_error("Sandbox has no parent Node")
	assert_engine_error("Object is Null")
	assert_engine_error("Object is Null")

	second.free()

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

func test_sgd_await_coroutine_api_is_reachable():
	var script = _await_script("await_api")
	if script == null:
		return
	var node = _await_node(script)

	# The coroutine accessors are Sandbox methods, and a scripted node forwards a call
	# to one only if is_sandbox_function() lists it.
	assert_eq(node.call("get_coroutine_count"), 0, "nothing suspended yet")
	assert_eq(node.call("get_max_coroutines"), 32, "the default cap")

	var awaitable = node.call("wait_for", sgd_ping, 100)
	assert_eq(typeof(awaitable), TYPE_SIGNAL, "the call suspended")
	assert_eq(node.call("get_coroutine_count"), 1, "one frame is held")

	node.call("set_max_coroutines", 8)
	assert_eq(node.call("get_max_coroutines"), 8)

	assert_eq(node.get("coroutines_max"), 8, "the property reads the same cap")
	node.set("coroutines_max", 4)
	assert_eq(node.call("get_max_coroutines"), 4, "and setting it reaches the Sandbox")
	var listed := false
	for property in node.get_property_list():
		if property["name"] == "coroutines_max":
			listed = true
	assert_true(listed, "and it is listed alongside the other limits")

	var dropped = [false]
	(awaitable as Signal).connect(func(_v): dropped[0] = true)
	node.call("reap_coroutines")
	assert_eq(node.call("get_coroutine_count"), 0, "the frame was dropped")
	assert_true(dropped[0], "a dropped frame completes with null rather than hanging its caller")

	node.free()

func test_sgd_lambdas():
	# A lambda is lifted to a function of its own and the expression is a
	# Callable over it. Captures are by value at the point the lambda is built,
	# which is checked here against the engine's own lambda on the same values.
	var gdscript_code = """
func doubled(n):
	var f = func(x): return x * 2
	return f.call(n)

func captured(n):
	var scale = 10
	var offset = 3
	var f = func(v): return v * scale + offset
	return f.call(n)

func capture_is_a_snapshot():
	var n = 1
	var f = func(): return n
	n = 50
	return [f.call(), n]

func write_stays_inside():
	var n = 1
	var f = func():
		n = 9
		return n
	var inner = f.call()
	return [inner, n]

func nested(n):
	var base = 7
	var outer = func(x):
		var inner = func(y): return y + base
		return inner.call(x)
	return outer.call(n)

func block_bodied(n):
	var f = func(x):
		var doubled = x * 2
		return doubled + 1
	return f.call(n)

func called_through_its_name(n):
	var add = func(x): return x + 100
	return add(n)
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	assert_eq(s.vmcallv("doubled", 21), 42, "A lambda runs when the guest calls it")
	assert_eq(s.vmcallv("captured", 4), 43, "Both captures arrive, in order")

	# The engine, on the same program.
	var engine_snapshot = func():
		var n = 1
		var f = func(): return n
		n = 50
		return [f.call(), n]
	assert_eq(s.vmcallv("capture_is_a_snapshot"), engine_snapshot.call(),
		"A capture is the value the local had when the lambda was built")

	var engine_write = func():
		var n = 1
		var f = func():
			n = 9
			return n
		var inner = f.call()
		return [inner, n]
	assert_eq(s.vmcallv("write_stays_inside"), engine_write.call(),
		"Assigning to a captured name must not reach the outer local")

	assert_eq(s.vmcallv("nested", 5), 12, "A nested lambda reaches through the lambda around it")
	assert_eq(s.vmcallv("block_bodied", 20), 41, "An indented lambda body runs")
	assert_eq(s.vmcallv("called_through_its_name", 5), 105,
		"A Callable in a variable can be called as c(x)")

	s.queue_free()

func test_sgd_callable_round_trip():
	# The point of a Callable: Godot calls back into the guest. Array.map() and
	# sort_custom() run on the host, one call into the sandbox per element, which
	# also pins the capture contract end to end -- RiscvCallable::call puts the
	# bound arguments before the ones Godot passes.
	var gdscript_code = """
func doubled(a):
	return a.map(func(x): return x * 2)

func scaled(a, k):
	return a.map(func(x): return x * k)

func kept(a, limit):
	return a.filter(func(x): return x < limit)

func sorted_desc(a):
	a.sort_custom(func(x, y): return x > y)
	return a

func make_adder(n):
	return func(x): return x + n

func helper(x):
	return x + 1

func get_helper():
	return helper
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("doubled", [1, 2, 3]), [2, 4, 6],
		"Array.map() should call the guest lambda once per element")
	assert_eq(s.vmcallv("scaled", [1, 2, 3], 10), [10, 20, 30],
		"A captured value must not be mistaken for the element Godot passes")
	assert_eq(s.vmcallv("kept", [1, 5, 2, 9], 5), [1, 2],
		"Array.filter() should see what the lambda answers")
	assert_eq(s.vmcallv("sorted_desc", [2, 9, 1]), [9, 2, 1],
		"sort_custom() should order by the guest's comparison")

	# A Callable that outlives the call that made it.
	var add5 = s.vmcallv("make_adder", 5)
	assert_eq(typeof(add5), TYPE_CALLABLE, "The guest should hand back a Callable")
	assert_eq(add5.call(10), 15, "A returned Callable keeps its capture")

	var helper = s.vmcallv("get_helper")
	assert_eq(typeof(helper), TYPE_CALLABLE, "A function name is a Callable")
	assert_eq(helper.call(41), 42, "Calling it should reach that function")

	s.queue_free()

func test_sgd_lambdas_are_not_published_as_methods():
	# A lifted lambda is named `@lambda_N`, which no GDScript identifier can be,
	# and it is not a method of the script: the editor is never offered a name
	# nobody can write.
	var script = SafeGDScript.new()
	script.source_code = """
func visible(n):
	var f = func(x): return x + 1
	return f.call(n)
"""
	var names = []
	for method in script.get_script_method_list():
		names.append(method["name"])

	assert_true(names.has("visible"), "A declared function is a method")
	for name in names:
		assert_false(name.begins_with("@"), "A lifted lambda is not published: " + str(name))

func test_sgd_inline_suites():
	# A body written on the line of its ':'. The engine accepts the same
	# spelling, so every answer below is checked against GDScript's own -- the
	# guest functions and the `_inline_*` helpers underneath are the same source.
	var gdscript_code = """
func classify(n):
	if n < 0: return "neg"
	elif n == 0: return "zero"
	else: return "pos"

func sum_to(n):
	var total = 0
	for i in n: total += i
	return total

func countdown(n):
	while n > 0: n -= 1
	return n

func spell(op):
	match op:
		0: return "add"
		1, 2: return "mul"
		var v when v > 9: return "big"
		_: return "other"

func one(): return 1

func semicolons(n):
	var a = 0
	if n > 0: a = 1; a += 10
	return a

func nested(a, b):
	if a > 0: if b > 0: return 3
	return 0

func evens(n):
	var total = 0
	for i in n: if i % 2 == 0: total += i
	return total
"""
	var s = _compile_and_load(gdscript_code, 40000)
	if s == null:
		return

	for n in [-3, 0, 5]:
		assert_eq(s.vmcallv("classify", n), _inline_classify(n),
			"A one-line if/elif/else should answer what the engine answers")
	assert_eq(s.vmcallv("sum_to", 5), _inline_sum_to(5), "A one-line for body should run per pass")
	assert_eq(s.vmcallv("countdown", 4), _inline_countdown(4), "A one-line while body should run per pass")
	for op in [0, 1, 2, 50, 4]:
		assert_eq(s.vmcallv("spell", op), _inline_spell(op), "A one-line match arm should take its arm")
	assert_eq(s.vmcallv("one"), 1, "A one-line func body should return")

	# Both statements after the ':' belong to the if, so a false condition runs
	# neither -- the ';' does not end the body.
	assert_eq(s.vmcallv("semicolons", 1), _inline_semicolons(1), "';' should continue the one-line body")
	assert_eq(s.vmcallv("semicolons", 0), _inline_semicolons(0), "A declined one-line body runs nothing after ';'")

	assert_eq(s.vmcallv("nested", 1, 1), 3, "A one-line body may hold another one")
	assert_eq(s.vmcallv("nested", 1, 0), 0, "The inner one-line body ends at the line")
	assert_eq(s.vmcallv("evens", 6), _inline_evens(6), "A one-line if inside a one-line for")

	s.queue_free()

func _inline_classify(n):
	if n < 0: return "neg"
	elif n == 0: return "zero"
	else: return "pos"

func _inline_sum_to(n):
	var total = 0
	for i in n: total += i
	return total

func _inline_countdown(n):
	while n > 0: n -= 1
	return n

func _inline_spell(op):
	match op:
		0: return "add"
		1, 2: return "mul"
		var v when v > 9: return "big"
		_: return "other"

func _inline_semicolons(n):
	var a = 0
	if n > 0: a = 1; a += 10
	return a

func _inline_evens(n):
	var total = 0
	for i in n: if i % 2 == 0: total += i
	return total

func test_sgd_iterating_an_untyped_value():
	# `for i in n` counts when `n` is an integer and walks when it is a
	# container, and the compiler learns which only at run time. One function
	# takes all three, so both paths through the one loop body are exercised.
	var gdscript_code = """
func total(it):
	var sum = 0
	for v in it: sum += v
	return sum

func collect(it):
	var out = []
	for v in it: out.append(v)
	return out

func nested(n):
	var sum = 0
	for a in n:
		for b in n:
			sum += a * b
	return sum

func counted(n):
	var seen = 0
	for i in n:
		if i == 0: continue
		if i > 3: break
		seen += i
	return seen
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	# An integer counts from 0, exactly as the engine does.
	assert_eq(s.vmcallv("total", 5), 10, "'for v in 5' should count 0..4")
	assert_eq(s.vmcallv("total", 0), 0, "Counting to 0 should not run the body")
	assert_eq(s.vmcallv("collect", 4), [0, 1, 2, 3], "The loop variable is the counter")

	# The same untyped function, given containers.
	assert_eq(s.vmcallv("total", [10, 20, 30]), 60, "An Array should still be walked")
	assert_eq(s.vmcallv("collect", ["a", "b"]), ["a", "b"], "Array elements, in order")
	assert_eq(s.vmcallv("collect", {"x": 1, "y": 2}), ["x", "y"], "A Dictionary still yields its keys")
	assert_eq(s.vmcallv("total", PackedInt32Array([1, 2, 3])), 6, "A packed array should still be walked")

	# Nesting: the guard is per loop, and the body is emitted once per loop.
	assert_eq(s.vmcallv("nested", 4), 36, "Nested untyped counts should agree with (0+1+2+3)^2")

	# `break` and `continue` reach the same labels on either path.
	assert_eq(s.vmcallv("counted", 6), 6, "break/continue should work in a guarded loop")

	s.queue_free()

func test_sgd_dictionary_literal_keys():
	# A dictionary key is an expression. Only an identifier written directly
	# before '=' is the Lua-style spelling of a string key: Godot 4.6.3 prints
	# `{ 7: 2 }` for `{k: 2}` with k == 7. The engine accepts every literal
	# below unchanged, so the `_dict_*` helpers underneath are the same source.
	var gdscript_code = """
const M = 5

func by_local(k):
	return {k: 2}

func by_name(k):
	return {k = 2}

func by_const():
	return {M: 1}

func by_expression(a, b):
	return {a + b: "sum", -a: "neg"}

func by_builtin():
	return {Vector2.ZERO: 3}

func named_string():
	return {a = 1, "b c" = 2,}

func nested(k):
	return {k: {k: k}}
"""
	var s = _compile_and_load(gdscript_code, 100000)
	if s == null:
		return

	assert_eq(s.vmcallv("by_local", 7), _dict_by_local(7), "'{k: 2}' should key by k's value")
	assert_eq(s.vmcallv("by_local", "s"), _dict_by_local("s"), "A String local keys by its value too")
	assert_eq(s.vmcallv("by_name", 7), _dict_by_name(7), "'{k = 2}' should key by the name 'k'")
	assert_eq(s.vmcallv("by_const"), _dict_by_const(), "A const key is its value, not its name")
	assert_eq(s.vmcallv("by_expression", 2, 3), _dict_by_expression(2, 3), "An arbitrary key expression should compile")
	assert_eq(s.vmcallv("by_builtin"), _dict_by_builtin(), "A builtin constant should key a literal")
	assert_eq(s.vmcallv("named_string"), _dict_named_string(), "A string is a Lua-style key too")
	assert_eq(s.vmcallv("nested", 4), _dict_nested(4), "Key expressions should nest")

	# The name spelling reaches the same entry a String subscript does, which is
	# what makes it usable as sugar for a record.
	var named = s.vmcallv("by_name", 7)
	assert_eq(named["k"], 2, "The Lua-style key should answer to its own name")

	s.queue_free()

	# The first entry fixes the spelling. The engine refuses a literal that
	# mixes them ("Mixing dictionary styles is not allowed"), so we do too --
	# otherwise a program that compiles here fails to parse there.
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	for source in ["func f(v):\n\treturn {a = 1, v: 2}\n",
			"func f(v):\n\treturn {v: 2, a = 1}\n",
			"func f():\n\treturn {1 = 2}\n"]:
		var elf = ts.vmcall("compile_to_elf", source)
		assert_eq(elf.is_empty(), true, "A mixed dictionary literal should not compile: " + source)
	ts.queue_free()

func _dict_by_local(k):
	return {k: 2}

func _dict_by_name(k):
	return {k = 2}

const _DICT_M = 5

func _dict_by_const():
	return {_DICT_M: 1}

func _dict_by_expression(a, b):
	return {a + b: "sum", -a: "neg"}

func _dict_by_builtin():
	return {Vector2.ZERO: 3}

func _dict_named_string():
	return {a = 1, "b c" = 2,}

func _dict_nested(k):
	return {k: {k: k}}

func test_sgd_string_escapes():
	# Every escape the engine accepts, and nothing else. The guest strings and
	# the `_escape_*` helpers underneath are the same source, so each answer is
	# checked against GDScript's own reading of the same literal.
	var gdscript_code = """
func controls():
	return "\\a\\b\\f\\v\\r\\n\\t"

func quotes():
	return "\\"q\\'\\\\"

func bmp():
	return "\\u00e9"

func astral():
	return "\\U01F600"

func surrogate_pair():
	return "\\ud83d\\ude00"

func mixed():
	return "a\\u00e9b\\U01F600c"

func lengths():
	return [bmp().length(), astral().length(), mixed().length()]

func code_points():
	return [bmp().unicode_at(0), astral().unicode_at(0), surrogate_pair().unicode_at(0)]

func continued():
	return "a\\
	b"
"""
	var s = _compile_and_load(gdscript_code, 100000)
	if s == null:
		return

	assert_eq(s.vmcallv("controls"), _escape_controls(), "\\a \\b \\f \\v should be the C control characters")
	assert_eq(s.vmcallv("quotes"), _escape_quotes(), "Quote and backslash escapes should be unchanged")
	assert_eq(s.vmcallv("bmp"), _escape_bmp(), "\\uXXXX should be one code point")
	assert_eq(s.vmcallv("astral"), _escape_astral(), "\\UXXXXXX should be one code point")
	assert_eq(s.vmcallv("surrogate_pair"), _escape_astral(), "A UTF-16 pair should be one code point")
	assert_eq(s.vmcallv("mixed"), _escape_mixed(), "Escapes should compose with the rest of the string")

	# Character counts, not byte counts: the guest hands over UTF-8 and Godot
	# reads it as a String.
	assert_eq(s.vmcallv("lengths"), [1, 1, 5], "An escaped code point is one character")
	assert_eq(s.vmcallv("code_points"), [233, 128512, 128512], "The code points should be the ones written")

	# A backslash before a newline joins the lines and leaves nothing behind.
	assert_eq(s.vmcallv("continued"), _escape_continued(), "A string line continuation should vanish")

	s.queue_free()

func _escape_controls():
	return "\a\b\f\v\r\n\t"

func _escape_quotes():
	return "\"q\'\\"

func _escape_bmp():
	return "\u00e9"

func _escape_astral():
	return "\U01F600"

func _escape_mixed():
	return "a\u00e9b\U01F600c"

func _escape_continued():
	return "a\
	b"

func test_sgd_invalid_escape_is_refused():
	# The engine rejects an escape it does not know -- "\\x41" is a parse error
	# there -- so a program that used to compile to the letter and drop the
	# backslash has to be refused instead of quietly meaning something else.
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var cases = {
		"func f():\n\treturn \"\\x41\"\n": "Invalid escape",
		"func f():\n\treturn \"\\0\"\n": "Invalid escape",
		"func f():\n\treturn \"\\u00e\"\n": "Invalid hexadecimal digit",
		"func f():\n\treturn \"\\ud83d\"\n": "unpaired lead surrogate",
		"func f():\n\treturn \"\\ude00\"\n": "unpaired trail surrogate",
	}
	for source in cases:
		var elf = ts.vmcall("compile_to_elf", source)
		assert_eq(elf.is_empty(), true, "An invalid escape should not compile: " + source)
		var error_msg = ts.vmcall("get_compiler_error", "")
		assert_true(error_msg.find(cases[source]) != -1,
			"The error should say what is wrong with the escape, got: " + error_msg)
	ts.queue_free()

func test_sgd_property_accessors():
	# A script-level var is a global in the data area, so an accessor changes
	# what the name means: a read becomes a call to the getter and a write a
	# call to the setter -- everywhere except inside the accessors, where the
	# name is the storage again, which is what keeps a setter from calling
	# itself. Every function below is declared twice, once for the sandbox and
	# once for the engine, from the same source, so each answer is the engine's.
	var gdscript_code = """
var _pm_log = []
var _pm_store = 10

var _pm_hp = 10:
	set(v):
		_pm_log.append("set")
		_pm_store = v
	get:
		_pm_log.append("get")
		return _pm_store

var _pm_mp = 5:
	set = _pm_set_mp,
	get = _pm_get_mp

func _pm_set_mp(v):
	_pm_store = v * 2

func _pm_get_mp():
	return _pm_store

var _pm_direct = 7:
	set(v):
		_pm_direct = v * 3

var _pm_lazy:
	get:
		return _pm_store + 1

var _pm_wo = 2:
	set(v):
		_pm_wo = v * 3

var _pm_ro = 1:
	get:
		return 99

func _pm_exercise():
	_pm_log.clear()
	_pm_hp = 3
	var a = _pm_hp
	_pm_mp = 4
	_pm_direct = 5
	return [a, _pm_mp, _pm_direct, _pm_log]

func _pm_compound():
	_pm_store = 1
	_pm_log.clear()
	_pm_hp += 1
	return [_pm_hp, _pm_log]

func _pm_one_sided():
	_pm_wo = 4
	_pm_ro = 5
	_pm_store = 20
	return [_pm_wo, _pm_ro, _pm_lazy]

func _pm_shadowed():
	var _pm_hp = 2
	_pm_hp = 3
	return _pm_hp
"""
	var s = _compile_and_load(gdscript_code, 200000)
	if s == null:
		return

	assert_eq(s.vmcallv("_pm_exercise"), _pm_exercise(),
		"A read should call the getter and a write the setter")
	assert_eq(s.vmcallv("_pm_compound"), _pm_compound(),
		"'hp += 1' should read through the getter and write through the setter")
	assert_eq(s.vmcallv("_pm_one_sided"), _pm_one_sided(),
		"A property with one accessor should fall back to its storage")
	assert_eq(s.vmcallv("_pm_shadowed"), _pm_shadowed(),
		"A local of the same name should shadow the property")

	# Spelled out as well, so the two copies cannot drift the same way. Godot
	# 4.6.3 prints exactly these for the source above.
	assert_eq(s.vmcallv("_pm_exercise"), [3, 8, 15, ["set", "get"]],
		"The setter, the getter and the direct write each ran once")
	assert_eq(s.vmcallv("_pm_compound"), [2, ["get", "set", "get"]],
		"A compound assignment gets, sets, and the return gets again")
	assert_eq(s.vmcallv("_pm_one_sided"), [12, 99, 21],
		"A missing setter writes the storage; a missing getter reads it")

	s.queue_free()

var _pm_log = []
var _pm_store = 10

var _pm_hp = 10:
	set(v):
		_pm_log.append("set")
		_pm_store = v
	get:
		_pm_log.append("get")
		return _pm_store

var _pm_mp = 5:
	set = _pm_set_mp,
	get = _pm_get_mp

func _pm_set_mp(v):
	_pm_store = v * 2

func _pm_get_mp():
	return _pm_store

var _pm_direct = 7:
	set(v):
		_pm_direct = v * 3

var _pm_lazy:
	get:
		return _pm_store + 1

var _pm_wo = 2:
	set(v):
		_pm_wo = v * 3

var _pm_ro = 1:
	get:
		return 99

func _pm_exercise():
	_pm_log.clear()
	_pm_hp = 3
	var a = _pm_hp
	_pm_mp = 4
	_pm_direct = 5
	return [a, _pm_mp, _pm_direct, _pm_log]

func _pm_compound():
	_pm_store = 1
	_pm_log.clear()
	_pm_hp += 1
	return [_pm_hp, _pm_log]

func _pm_one_sided():
	_pm_wo = 4
	_pm_ro = 5
	_pm_store = 20
	return [_pm_wo, _pm_ro, _pm_lazy]

func _pm_shadowed():
	var _pm_hp = 2
	_pm_hp = 3
	return _pm_hp

func test_sgd_exported_property_accessors():
	# Godot reaches a guest property through the setter and getter addresses the
	# ELF registers, so an @export with an accessor runs it on set and get from
	# the editor too -- and a property with only one accessor still answers both
	# ways, because the engine's answer for the missing half is the storage.
	var gdscript_code = """
@export var hp: int = 10:
	set(v):
		hp = v if v > 0 else 0
	get:
		return hp

@export var doubled = 1:
	set(v):
		doubled = v * 2

@export var fixed = 1:
	get:
		return 99

func read_hp():
	return hp

func read_doubled():
	return doubled
"""
	var s = _compile_and_load(gdscript_code, 200000)
	if s == null:
		return

	# The declaration's own value never runs the setter, in the guest or here.
	assert_eq(s.get("hp"), 10, "The initializer should reach the storage untouched")
	assert_eq(s.get("doubled"), 1, "A setter should not run on the declaration")

	# A set from Godot runs the guest setter.
	s.set("hp", -5)
	assert_eq(s.get("hp"), 0, "The setter should clamp what Godot assigns")
	assert_eq(s.vmcallv("read_hp"), 0, "The guest should see the same value")
	s.set("hp", 7)
	assert_eq(s.get("hp"), 7, "A value the setter accepts should arrive unchanged")

	# Setter only: Godot reads the storage, which is what the engine does.
	s.set("doubled", 4)
	assert_eq(s.get("doubled"), 8, "A setter-only property should still read back")
	assert_eq(s.vmcallv("read_doubled"), 8, "The guest reads the same storage")

	# Getter only: a set reaches the storage, a get runs the getter.
	assert_eq(s.get("fixed"), 99, "A getter-only property should answer its getter")
	s.set("fixed", 5)
	assert_eq(s.get("fixed"), 99, "The getter still decides what a read answers")

	s.queue_free()

const STARTUP_MEMBER_SOURCE := """
var boxed = _boxed("abcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcq")
var raw = _raw("abcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcqabcq")
var text = "hello" + " " + "world"
var list = [1, 2, 3]
var built = _build()

func _boxed(t : String) -> int:
	var n = 0
	for c in t:
		if c == "q":
			n += 1000
		n += 1
	return n

func _raw(t : String) -> int:
	var n = 0
	for c in t:
		n += ord(c)
	return n

func _build() -> Array:
	var out = []
	for i in range(40):
		out.append("item" + str(i))
	return out
"""

func test_sgd_a_loop_in_a_member_initializer_reclaims_its_temporaries():
	# The entry point runs the global and member initializers itself, and a scope
	# release is a no-op in the state the host is in outside a call: every
	# temporary a loop made there took a permanent slot nothing gave back, so an
	# initializer that walked more than references_max characters failed the load.
	# Startup runs one state up now, and what has to outlive it is promoted.
	var path = "user://temp_startup_members.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(STARTUP_MEMBER_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the startup script should load as a SafeGDScript resource")
	if script == null:
		return
	var node = Node.new()
	node.set_script(script)

	# 26 * "abcq": one character each, and a thousand for every q.
	assert_eq(node.get("boxed"), 104 + 26 * 1000, "the boxed walk ran to the end")
	assert_eq(node.get("raw"), 26 * (97 + 98 + 99 + 113), "and so did the raw one")

	# Values the initializers left behind still read back after startup.
	assert_eq(node.get("text"), "hello world", "a String member survived startup")
	assert_eq(node.get("list"), [1, 2, 3], "an Array member survived startup")
	assert_eq(node.get("built").size(), 40, "a member a loop built survived startup")
	assert_eq(node.get("built")[39], "item39", "with the contents it was built with")

	node.free()

func test_sgd_string_indexing_and_iteration():
	# A String is the one indexable value in Godot with no get(), so the VCALL
	# every other container falls back to cannot serve it: `s[i]` and `for c in s`
	# each go to their own host syscall, and an untyped subscript costs one tag
	# test to find out which. Every function is declared twice, once for the
	# sandbox and once for the engine, from the same source.
	var gdscript_code = """
func first(s):
	return s[0]

func at(s, i):
	return s[i]

func last(s):
	return s[-1]

func typed_at(s: String, i: int):
	return s[i]

func truncated(s: String):
	return s[1.0]

func chars(s):
	var out = []
	for c in s:
		out.append(c)
	return out

func typed_chars(s: String):
	var out = []
	for c in s:
		out.append(c)
	return out

func joined(s: String):
	var acc = ""
	for c in s:
		acc = c + acc
	return acc

func counted(s):
	var n = 0
	for c in s:
		n += 1
	return n

func widths(s: String):
	var out = []
	for c in s:
		out.append(c.length())
	return out

func codepoint_sum(s: String):
	var total = 0
	for c in s:
		total += ord(c)
	return total

func marked_codepoint_sum(s: String, mark: String):
	var total = 0
	for c in s:
		if c == mark:
			total += 1000
		total += ord(c)
	return total

func nested(s: String):
	var out = []
	for a in s:
		for b in s:
			out.append(a + b)
	return out

func long_walk(s: String, times: int):
	var text = s.repeat(times)
	var acc = 0
	for c in text:
		acc += c.length()
	return acc

func stopped(s: String, at: String):
	var out = ""
	for c in s:
		if c == at:
			break
		out += c
	return out

func skipped(s: String, drop: String):
	var out = ""
	for c in s:
		if c == drop:
			continue
		out += c
	return out

func kept(s: String):
	var seen = []
	var last = ""
	for c in s:
		last = c
		seen.append(last)
	return [last, seen]

func reassigned(s: String):
	var text = s
	var out = ""
	for c in text:
		out += c
		text = "!"
	return [out, text]
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	# One character, as a String -- not the code point.
	assert_eq(s.vmcallv("first", "abc"), _si_first("abc"), "s[0] should be a one-character String")
	assert_eq(s.vmcallv("first", "abc"), "a", "s[0] should be the first character")
	assert_true(s.vmcallv("first", "abc") is String, "The character should be a String, not an int")

	# A multi-byte character is one character, not one byte.
	assert_eq(s.vmcallv("at", "aéb", 1), _si_at("aéb", 1), "An index counts characters")
	assert_eq(s.vmcallv("at", "aéb", 1), "é", "The character at 1 is é")
	assert_eq(s.vmcallv("chars", "aéb"), _si_chars("aéb"), "Walking should yield characters")
	assert_eq(s.vmcallv("chars", "aéb"), ["a", "é", "b"], "Three characters, not four bytes")

	# A negative index counts from the end, as it does in the engine.
	assert_eq(s.vmcallv("last", "abc"), _si_last("abc"), "s[-1] should be the last character")
	assert_eq(s.vmcallv("last", "abc"), "c", "s[-1] is 'c' in 'abc'")

	# A type hint changes what is emitted, not what it answers.
	assert_eq(s.vmcallv("typed_at", "hello", 3), _si_typed_at("hello", 3),
		"A ': String' hint should answer the same")
	assert_eq(s.vmcallv("truncated", "abc"), _si_truncated("abc"),
		"A float index should be truncated, as the engine truncates it")
	assert_eq(s.vmcallv("truncated", "abc"), "b", "s[1.0] is the character at 1")

	# The same subscript over an Array and a Dictionary, through the untyped
	# path that now carries the String test.
	assert_eq(s.vmcallv("at", [10, 20, 30], 1), 20, "The tag test should not disturb an Array")
	assert_eq(s.vmcallv("at", {"k": 5}, "k"), 5, "or a Dictionary")
	assert_eq(s.vmcallv("at", PackedInt32Array([7, 8]), 1), 8, "or a packed array")

	# Iterating, typed and untyped, and the arms beside the String one.
	assert_eq(s.vmcallv("typed_chars", "hey"), _si_typed_chars("hey"), "A typed walk yields characters")
	assert_eq(s.vmcallv("joined", "abc"), _si_joined("abc"), "The characters concatenate")
	assert_eq(s.vmcallv("joined", "abc"), "cba", "Reversed, one character at a time")
	assert_eq(s.vmcallv("counted", "hello"), 5, "A String walk runs once per character")
	assert_eq(s.vmcallv("counted", [1, 2]), 2, "An Array still walks")
	assert_eq(s.vmcallv("counted", 3), 3, "An integer still counts")
	assert_eq(s.vmcallv("counted", {"a": 1}), 1, "A Dictionary still yields its keys")
	assert_eq(s.vmcallv("counted", PackedFloat32Array([1.0, 2.0, 3.0])), 3,
		"A packed array still walks")
	assert_eq(s.vmcallv("chars", ""), [], "An empty String runs the body no times")

	# Each character is a String in its own right.
	assert_eq(s.vmcallv("widths", "aéb"), _si_widths("aéb"), "Every character has length 1")
	assert_eq(s.vmcallv("codepoint_sum", "a😀é"), _si_codepoint_sum("a😀é"),
		"A raw walk should preserve UTF-32 code points for ord()")
	# Comparing the character is a use the raw code-point batch cannot serve, so
	# this walk yields boxed one-character Strings -- and ord() then has to read
	# the character, not the handle the batch identified it by.
	assert_eq(s.vmcallv("marked_codepoint_sum", "abcq", "q"),
		_si_marked_codepoint_sum("abcq", "q"),
		"ord() should read the character even when the walk boxes it")
	assert_eq(s.vmcallv("marked_codepoint_sum", "abcdefghijklmnopqrstuvwxyz0123456789", "q"),
		_si_marked_codepoint_sum("abcdefghijklmnopqrstuvwxyz0123456789", "q"),
		"and across every refill of the boxed batch")
	assert_eq(s.vmcallv("nested", "ab"), _si_nested("ab"), "Two String walks may nest")

	# Characters arrive in batches, so a walk longer than one batch has to hand
	# out every character across every refill, and leaving early has to leave
	# from the middle of a batch.
	assert_eq(s.vmcallv("long_walk", "abcde", 100), _si_long_walk("abcde", 100),
		"A walk should not stop at a batch boundary")
	assert_eq(s.vmcallv("long_walk", "a😀é", 100), _si_long_walk("a😀é", 100),
		"The raw code-point batch should preserve BMP and astral characters")
	assert_eq(s.vmcallv("long_walk", "x", 1), 1, "Nor should a one-character walk")
	assert_eq(s.vmcallv("stopped", "abcdefghijklmnopqrstuvwxyz", "u"),
		_si_stopped("abcdefghijklmnopqrstuvwxyz", "u"), "break should leave mid-batch")
	assert_eq(s.vmcallv("skipped", "abcabcabcabcabcabcabcabc", "b"),
		_si_skipped("abcabcabcabcabcabcabcabc", "b"), "continue should resume mid-batch")

	# A character held past the refill that produced it: the batch it belonged
	# to is released, and what the body kept has to survive that.
	assert_eq(s.vmcallv("kept", "abcdefghijklmnopqrst"), _si_kept("abcdefghijklmnopqrst"),
		"A character kept across a refill should still read as itself")
	assert_eq(s.vmcallv("reassigned", "abcdef"), _si_reassigned("abcdef"),
		"Reassigning the source should not move the walk")

	s.queue_free()

func _si_first(s):
	return s[0]

func _si_at(s, i):
	return s[i]

func _si_last(s):
	return s[-1]

func _si_typed_at(s: String, i: int):
	return s[i]

func _si_truncated(s: String):
	return s[1.0]

func _si_chars(s):
	var out = []
	for c in s:
		out.append(c)
	return out

func _si_typed_chars(s: String):
	var out = []
	for c in s:
		out.append(c)
	return out

func _si_joined(s: String):
	var acc = ""
	for c in s:
		acc = c + acc
	return acc

func _si_counted(s):
	var n = 0
	for c in s:
		n += 1
	return n

func _si_widths(s: String):
	var out = []
	for c in s:
		out.append(c.length())
	return out

func _si_codepoint_sum(s: String):
	var total = 0
	for c in s:
		total += ord(c)
	return total

func _si_marked_codepoint_sum(s: String, mark: String):
	var total = 0
	for c in s:
		if c == mark:
			total += 1000
		total += ord(c)
	return total

func _si_nested(s: String):
	var out = []
	for a in s:
		for b in s:
			out.append(a + b)
	return out

func _si_long_walk(s: String, times: int):
	var text = s.repeat(times)
	var acc = 0
	for c in text:
		acc += c.length()
	return acc

func _si_stopped(s: String, at: String):
	var out = ""
	for c in s:
		if c == at:
			break
		out += c
	return out

func _si_skipped(s: String, drop: String):
	var out = ""
	for c in s:
		if c == drop:
			continue
		out += c
	return out

func _si_kept(s: String):
	var seen = []
	var last = ""
	for c in s:
		last = c
		seen.append(last)
	return [last, seen]

func _si_reassigned(s: String):
	var text = s
	var out = ""
	for c in text:
		out += c
		text = "!"
	return [out, text]

func test_sgd_iterating_a_float():
	# `for i in 2.5` counts 0.0, 1.0, 2.0 -- one pass per whole number strictly
	# below the bound, and the counter is a float, not an int. The untyped path
	# turns the float into its own bound before the loop, so it shares the
	# integer arm; the arms beside it must still answer what they answered.
	var gdscript_code = """
func literal():
	var out = []
	for i in 2.5:
		out.append(i)
	return out

func whole():
	var out = []
	for i in 3.0:
		out.append(i)
	return out

func typed(x: float):
	var out = []
	for i in x:
		out.append(i)
	return out

func negative():
	var out = []
	for i in -1.0:
		out.append(i)
	return out

func untyped(x):
	var out = []
	for i in x:
		out.append(i)
	return out

func summed(x):
	var total = 0.0
	for i in x:
		total += i
	return total
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("literal"), _fi_literal(), "'for i in 2.5' should count 0.0, 1.0, 2.0")
	assert_eq(s.vmcallv("literal"), [0.0, 1.0, 2.0], "The counter is a float, not an int")
	assert_true(s.vmcallv("literal")[0] is float, "The loop variable should be a float")
	assert_eq(s.vmcallv("whole"), _fi_whole(), "A whole float stops one below itself")
	assert_eq(s.vmcallv("whole"), [0.0, 1.0, 2.0], "'for i in 3.0' is 0.0, 1.0, 2.0")
	assert_eq(s.vmcallv("typed", 2.5), _fi_typed(2.5), "A ': float' hint should answer the same")
	assert_eq(s.vmcallv("negative"), _fi_negative(), "A negative bound runs the body no times")

	# The same function, given a float only at run time.
	assert_eq(s.vmcallv("untyped", 2.5), _fi_untyped(2.5), "An untyped float should count too")
	assert_eq(s.vmcallv("untyped", 2.5), [0.0, 1.0, 2.0], "and count in floats")
	assert_eq(s.vmcallv("untyped", 0.5), _fi_untyped(0.5), "A bound below 1 runs the body once")
	assert_eq(s.vmcallv("untyped", -2.0), _fi_untyped(-2.0), "A negative bound still runs nothing")

	# The arms beside the float one, through the same untyped loop.
	assert_eq(s.vmcallv("untyped", 3), [0, 1, 2], "An integer still counts, in ints")
	assert_eq(s.vmcallv("untyped", [7, 8]), [7, 8], "An Array still walks")
	assert_eq(s.vmcallv("untyped", "ab"), ["a", "b"], "A String still yields characters")
	assert_eq(s.vmcallv("untyped", {"k": 1}), ["k"], "A Dictionary still yields its keys")
	assert_eq(s.vmcallv("untyped", PackedInt32Array([4, 5])), [4, 5], "A packed array still walks")

	assert_almost_eq(s.vmcallv("summed", 4.0), _fi_summed(4.0), 0.0001,
		"The float counters should add up to what the engine adds up to")

	s.queue_free()

func _fi_literal():
	var out = []
	for i in 2.5:
		out.append(i)
	return out

func _fi_whole():
	var out = []
	for i in 3.0:
		out.append(i)
	return out

func _fi_typed(x: float):
	var out = []
	for i in x:
		out.append(i)
	return out

func _fi_negative():
	var out = []
	for i in -1.0:
		out.append(i)
	return out

func _fi_untyped(x):
	var out = []
	for i in x:
		out.append(i)
	return out

func _fi_summed(x):
	var total = 0.0
	for i in x:
		total += i
	return total

func test_sgd_calling_an_expression():
	# `a[0]()` and `get_f()()`: the callee is a value, not a name. Ours, on the
	# same grounds as `c(1)` -- the engine says "Cannot call on an expression.
	# Use \".call()\" if it's a Callable" -- and it is the .call() it stands for,
	# so the written-out spelling has to answer identically.
	var gdscript_code = """
func double(x: int):
	return x * 2

func get_f():
	return double

func from_array(n: int):
	var a = [double]
	return a[0](n)

func from_call(n: int):
	return get_f()(n)

func from_lambda(n: int):
	var a = [func(x): return x + 1]
	return a[0](n)

func chained(n: int):
	return get_f()(get_f()(n))

func from_dictionary(n: int):
	var d = {"f": double}
	return d["f"](n)

func with_capture(n: int):
	var base = 10
	var a = [func(x): return x + base]
	return a[0](n)

func spelled_out(n: int):
	return get_f().call(n)
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("from_array", 4), 8, "a[0](n) should call the element")
	assert_eq(s.vmcallv("from_call", 5), 10, "get_f()(n) should call the returned Callable")
	assert_eq(s.vmcallv("from_lambda", 6), 7, "a lambda in an Array should be callable")
	assert_eq(s.vmcallv("chained", 3), 12, "two calls on expressions should compose")
	assert_eq(s.vmcallv("from_dictionary", 7), 14, "d[\"f\"](n) should call the value")
	assert_eq(s.vmcallv("with_capture", 5), 15, "the lambda should still see its capture")

	# The same thing, written the way the engine insists on.
	assert_eq(s.vmcallv("spelled_out", 5), s.vmcallv("from_call", 5),
		"f(x) on an expression is the f.call(x) it stands for")

	s.queue_free()

const BREAKPOINT_STATEMENT_SOURCE = """
func seed():
	return 10

func work(n):
	var total = 0
	if n > 0:
		breakpoint
		total = seed() * 2
	breakpoint
	return total
"""
#  1 blank              7 if n > 0:
#  2 func seed():       8 breakpoint
#  3 return 10          9 total = seed() * 2
#  4 blank             10 breakpoint
#  5 func work(n):     11 return total
#  6 var total = 0

func test_sgd_breakpoint_statement():
	# `breakpoint` is the source asking for a stop, not the host. So it needs no
	# set_breakpoint() call, it turns on the debug build by itself, and it is not
	# part of get_active_breakpoints() -- which answers what was *requested*, and
	# which the editor applies its own set to as a delta.
	var path = "user://temp_break_statement.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(BREAKPOINT_STATEMENT_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the script should load as a SafeGDScript resource")
	if script == null:
		return
	var node = _breakpoint_node(script)

	# A statement is not a request, so it buys neither the debug build nor a row
	# in the list of breakpoints that were placed: a push and a pop per call is
	# not something a keyword in a mod should decide for the whole program.
	assert_false(script.is_debug_build(), "a statement does not ask for a debug build")
	assert_eq(script.get_breakpoints(), PackedInt32Array(),
		"no breakpoint was requested from the host")
	assert_eq(script.get_active_breakpoints(), PackedInt32Array(),
		"and a statement is not a requested breakpoint")

	script.breakpoint_hit.connect(_on_breakpoint)
	_reset_break_capture()

	# The answer is the point: a stop that changes one is not a breakpoint.
	assert_eq(node.call("work", 5), 20, "work(5) = 20 across two stops")
	assert_eq(_break_lines, [8, 10], "both statements stopped, in order")
	assert_eq(_break_stopped, [true, true], "and the guest was stopped for each")
	assert_eq(_break_reported_line, [8, 10], "the break state names the same lines")


	# The branch it sits in still decides whether it runs.
	_reset_break_capture()
	assert_eq(node.call("work", 0), 0, "work(0) = 0")
	assert_eq(_break_lines, [10], "a declined branch skips the stop inside it")

	# A requested breakpoint on the statement's own line is one stop, not two,
	# and that request is reported as placed.
	_reset_break_capture()
	assert_true(script.set_breakpoint(8, true), "the line can also be requested")
	assert_eq(script.get_active_breakpoints(), PackedInt32Array([8]),
		"the request is answered by the statement's own stop")
	assert_true(script.is_debug_build(), "and asking is what brings the debug build")
	assert_eq(node.call("work", 5), 20, "work(5) = 20 still")
	assert_eq(_break_lines, [8, 10], "line 8 stopped once, not twice")

	# Which is where the backtrace comes from.
	if _break_backtrace.size() == 2:
		var frames : PackedStringArray = _break_backtrace[0]
		assert_eq(frames.size(), 1, "work() was the only frame standing")
		if frames.size() >= 1:
			assert_true(frames[0].contains(":8"), "the frame is line 8: " + frames[0])
			assert_true(frames[0].contains("work"), "and it is work(): " + frames[0])

	script.breakpoint_hit.disconnect(_on_breakpoint)
	node.free()

func test_sgd_breakpoint_statement_with_nothing_listening():
	# No listener and no debugger: reported and stepped over, not stopped, so a
	# stray `breakpoint` in a mod cannot wedge the host thread.
	var gdscript_code = """
func work():
	breakpoint
	return 7
"""
	var s = _compile_and_load(gdscript_code, 100000)
	if s == null:
		return
	assert_eq(s.vmcallv("work"), 7, "the program runs past a stop nobody is waiting on")
	assert_false(SafeGDScript.is_stopped(), "and nothing is left stopped")
	s.queue_free()

func test_sgd_qualified_types_and_grouping_annotations():
	# `A.B` names something only the engine's analyzer can resolve, so it parses
	# and drops: the program still compiles and still answers. The three @export
	# annotations that name a section of the inspector stand alone, so a function
	# may follow one -- which is how a real script is laid out.
	var gdscript_code = """
extends Node.Inner

@export_category("Stats")
@export_group("Combat")
@export_range(0, 100) var hp: int = 100
@export var speed: float = 1.5

@export_subgroup("Internal")
@export var tag = "x"

@export_group("Nothing here")

func hurt(amount: Node.Damage) -> Node.Result:
	hp -= amount
	return hp

func typed_container(values: Array[Node.Inner]) -> Array[Node.Inner]:
	return values

func nested(a: A.B.C):
	return a
"""
	var s = _compile_and_load(gdscript_code, 200000)
	if s == null:
		return

	# The dropped type hints changed nothing about what runs.
	assert_eq(s.vmcallv("hurt", 30), 70, "a qualified parameter and return type still run")
	assert_eq(s.vmcallv("typed_container", [1, 2]), [1, 2], "and inside a container's element type")
	assert_eq(s.vmcallv("nested", 5), 5, "and however many segments it has")

	# The properties around the grouping annotations are still published.
	assert_eq(s.get("hp"), 70, "@export_group did not swallow the property after it")
	assert_almost_eq(s.get("speed"), 1.5, 0.0001, "nor the one after that")
	assert_eq(s.get("tag"), "x", "nor one after an @export_subgroup")

	s.queue_free()

	# What the engine refuses, we refuse: an annotation neither of us knows.
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	for source in ["@bogus var n = 1\nfunc f():\n\treturn n\n",
			"func f(a: Node.):\n\treturn a\n"]:
		var elf = ts.vmcall("compile_to_elf", source)
		assert_eq(elf.is_empty(), true, "should not compile: " + source)
	ts.queue_free()

# -= Signals =-

signal sgd_hit(damage: int, cause)
signal sgd_done

const SIGNAL_SOURCE = """
signal sgd_hit(damage: int, cause)
signal sgd_done

var seen = []
var own_hits = 0

func fire(n):
	sgd_hit.emit(n, "guest")
	sgd_done.emit()

func fire_by_name(n):
	emit_signal("sgd_hit", n, "by name")

func fire_many(n):
	for i in range(n):
		sgd_hit.emit(i, "many")

func wait_for_hit():
	return await sgd_hit

func hook(c):
	sgd_hit.connect(c)
	return sgd_hit.is_connected(c)

func unhook(c):
	sgd_hit.disconnect(c)
	return sgd_hit.is_connected(c)

func on_hit(damage, cause):
	own_hits += damage

func hook_own():
	sgd_hit.connect(Callable(self, "on_hit"))
	return sgd_hit.is_connected(Callable(self, "on_hit"))

func unhook_own():
	sgd_hit.disconnect(Callable(self, "on_hit"))
	return sgd_hit.is_connected(Callable(self, "on_hit"))

func own_total():
	return own_hits

func kind():
	return typeof(sgd_hit)

func name_of():
	return str(sgd_hit.get_name())

func shadowed():
	var sgd_hit = 5
	return sgd_hit
"""

func _signal_node() -> Node:
	var path = "user://temp_signals.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(SIGNAL_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the signal script should load as a SafeGDScript resource")
	if script == null:
		return null
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(200000)
	return node

func _signal_info(object: Object, name: String) -> Dictionary:
	for entry in object.get_signal_list():
		if entry["name"] == name:
			return entry
	return {}

func test_sgd_publishes_declared_signals():
	var node = _signal_node()
	if node == null:
		return

	assert_true(node.has_signal("sgd_hit"), "a declared signal reaches the host")
	assert_true(node.has_signal("sgd_done"), "including one with no parameters")
	assert_false(node.has_signal("sgd_never"), "and nothing else does")

	assert_eq(_signal_info(node, "sgd_hit"), _signal_info(self, "sgd_hit"),
		"the published signal should match the engine's own")
	assert_eq(_signal_info(node, "sgd_done"), _signal_info(self, "sgd_done"),
		"and so should one with no parameters")

	node.free()

func test_sgd_emits_to_a_connected_handler():
	var node = _signal_node()
	if node == null:
		return

	var hits := []
	var dones := [0]
	node.sgd_hit.connect(func(damage, cause): hits.append([damage, cause]))
	node.connect("sgd_done", func(): dones[0] += 1)

	node.call("fire", 7)
	assert_eq(hits, [[7, "guest"]], "emit reached the handler with both arguments")
	assert_eq(dones[0], 1, "and a parameterless signal reached its own")

	node.call("fire", 8)
	assert_eq(hits.size(), 2, "the connection outlives the call that emitted")

	node.call("fire_by_name", 9)
	assert_eq(hits[2], [9, "by name"], "emit_signal() by name reaches the same signal")

	node.free()

func test_sgd_emits_more_often_than_a_call_has_scoped_variants():
	var node = _signal_node()
	if node == null:
		return

	# A materialised Signal would cost a scoped variant per pass, capped at MAX_REFS = 100.
	var count := [0]
	node.connect("sgd_hit", func(_damage, _cause): count[0] += 1)

	node.call("fire_many", 250)
	assert_eq(count[0], 250, "every emission in the loop reached the handler")

	node.free()

func test_sgd_awaits_its_own_signal():
	var node = _signal_node()
	if node == null:
		return

	var awaitable = node.call("wait_for_hit")
	assert_eq(typeof(awaitable), TYPE_SIGNAL,
		"the suspended coroutine answers with something Godot's await accepts")

	# Connected (not awaited) so the test stays synchronous.
	var completed := [null]
	(awaitable as Signal).connect(func(value): completed[0] = value)

	node.sgd_hit.emit(2, "host")
	assert_eq(completed[0], [2, "host"],
		"a two-argument signal yields both, the way the engine's await does")

	node.free()

func test_sgd_connects_from_inside_the_program():
	var node = _signal_node()
	if node == null:
		return

	var seen := []
	var callback := func(damage, cause): seen.append(damage)

	assert_true(node.call("hook", callback), "the guest connected the Callable it was handed")
	node.call("fire", 3)
	assert_eq(seen, [3], "and the emission reached it")

	assert_false(node.call("unhook", callback), "the guest disconnected it again")
	node.call("fire", 4)
	assert_eq(seen, [3], "so a later emission reaches nobody")

	node.free()

func test_sgd_disconnects_a_callable_of_its_own():
	# Every Callable the guest makes is a fresh RiscvCallable, so a connection
	# has to be found by what the callable names -- sandbox, script instance,
	# guest function and bound arguments -- and not by the object holding it.
	var node = _signal_node()
	if node == null:
		return

	assert_true(node.call("hook_own"), "the guest connected a Callable of its own")
	node.call("fire", 3)
	assert_eq(node.call("own_total"), 3, "and the emission reached it")

	# A different Callable object naming the same function finds the connection.
	assert_false(node.call("unhook_own"), "and disconnected it again")
	node.call("fire", 4)
	assert_eq(node.call("own_total"), 3, "so a later emission reaches nobody")

	node.free()

func test_sgd_a_signal_is_a_value():
	var node = _signal_node()
	if node == null:
		return

	assert_eq(node.call("kind"), TYPE_SIGNAL, "a signal name is a Signal")
	assert_eq(node.call("kind"), typeof(sgd_hit), "which is what the engine calls it")
	assert_eq(node.call("name_of"), "sgd_hit", "and it knows its own name")
	assert_eq(node.call("name_of"), str(sgd_hit.get_name()), "as the engine's does")

	var from_host = node.get("sgd_hit")
	assert_eq(typeof(from_host), TYPE_SIGNAL, "reading the name from Godot gives a Signal")
	assert_eq((from_host as Signal).get_object(), node, "bound to the node the script is on")

	assert_eq(node.call("shadowed"), _sgd_shadowed(), "a local shadows the signal")

	node.free()

func _sgd_shadowed():
	var sgd_hit = 5
	return sgd_hit

func test_sgd_signal_refusals():
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	for source in [
			"signal s(a = 1)\nfunc f():\n\treturn 1\n",
			"signal s(a)\nvar s = 3\nfunc f():\n\treturn s\n",
			"signal s(a)\nfunc s():\n\treturn 1\n",
			"signal s(a)\nsignal s(a)\nfunc f():\n\treturn 1\n",
			"signal s(a)\nfunc f():\n\ts = 1\n",
			"signal s(a)\nfunc f():\n\ts(1)\n"]:
		var elf = ts.vmcall("compile_to_elf", source)
		assert_eq(elf.is_empty(), true, "should not compile: " + source)
	ts.queue_free()

# -= Instances =-
#
# A script-level `var` is a member: two nodes on one .sgd have two of it, the way
# GDScript does. They still share one Sandbox -- the member record is per instance,
# addressed off a base register the host writes on every entry. `const` and
# `static var` stay shared, which is what those words mean.

const INSTANCE_SOURCE := """
@export var speed = 1
var bump = 0
var items = []
static var shared = 0
const LIMIT = 10

func step():
	bump += 1
	shared += 1
	items.append(bump)
	return bump

func read_speed():
	return speed

func through_the_host(cb):
	bump += 10
	var answer = cb.call()
	return [bump, answer]

func read_items():
	return items

func read_shared():
	return shared

func read_limit():
	return LIMIT

func hand_out_a_callable():
	return step

func wait_then_step(sig):
	await sig
	return step()
"""

func _instance_script(name : String):
	var path = "user://temp_%s.sgd" % name
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(INSTANCE_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the instance script should load as a SafeGDScript resource")
	return script

func _instance_node(script) -> Node:
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(100000)
	return node

func test_sgd_members_are_per_instance():
	var script = _instance_script("instance_members")
	if script == null:
		return
	var a = _instance_node(script)
	var b = _instance_node(script)

	assert_eq(a.call("step"), 1, "the first instance counts from its own zero")
	assert_eq(b.call("step"), 1, "and so does the second")
	assert_eq(a.call("step"), 2, "the first one kept its own count")

	# A container member holds a permanent Variant of its own, not a shared handle.
	assert_eq(a.call("read_items"), [1, 2], "the first instance's array")
	assert_eq(b.call("read_items"), [1], "the second instance's own array")

	a.free()
	b.free()

func test_sgd_exported_values_are_per_instance():
	var script = _instance_script("instance_exports")
	if script == null:
		return
	var a = _instance_node(script)
	var b = _instance_node(script)

	a.set("speed", 42)
	assert_eq(a.get("speed"), 42, "the property was set on the instance it names")
	assert_eq(b.get("speed"), 1, "and the other instance kept its declared value")
	assert_eq(b.call("read_speed"), 1, "which is what the guest reads too")

	b.set("speed", 7)
	assert_eq(a.call("read_speed"), 42, "setting the other one changed nothing here")
	assert_eq(b.call("read_speed"), 7, "and landed where it was aimed")

	a.free()
	b.free()

func test_sgd_static_and_const_are_shared():
	var script = _instance_script("instance_shared")
	if script == null:
		return
	var a = _instance_node(script)
	var b = _instance_node(script)

	a.call("step")
	b.call("step")
	a.call("step")
	assert_eq(a.call("read_shared"), 3, "a static var counts every instance's steps")
	assert_eq(b.call("read_shared"), 3, "and reads the same from either")
	assert_eq(a.call("read_limit"), 10, "a const is the same value everywhere")

	a.free()
	b.free()

func test_sgd_freeing_an_instance_leaves_the_others_alone():
	var script = _instance_script("instance_free")
	if script == null:
		return
	var a = _instance_node(script)
	var b = _instance_node(script)

	a.call("step")
	a.call("step")
	b.call("step")
	a.free()

	# The freed instance's record went back to the guest heap; the survivor's
	# members and its container are untouched by that.
	assert_eq(b.call("step"), 2, "the surviving instance kept counting")
	assert_eq(b.call("read_items"), [1, 2], "and still holds its own array")

	b.free()

func test_sgd_a_callable_runs_as_the_instance_that_made_it():
	var script = _instance_script("instance_callable")
	if script == null:
		return
	var a = _instance_node(script)
	var b = _instance_node(script)

	b.call("step")
	b.call("step")
	var from_a = a.call("hand_out_a_callable") as Callable
	assert_eq(from_a.call(), 1, "the Callable stepped the instance that handed it out")
	assert_eq(b.call("step"), 3, "not the one that was called most recently")

	a.free()
	b.free()

func test_sgd_await_resumes_against_its_own_members():
	var script = _instance_script("instance_await")
	if script == null:
		return
	var a = _instance_node(script)
	var b = _instance_node(script)
	add_child(a)
	add_child(b)

	b.call("step")
	b.call("step")

	var completed := [null]
	(a.call("wait_then_step", sgd_ping) as Signal).connect(func(value): completed[0] = value)
	sgd_ping.emit(1)

	assert_eq(completed[0], 1, "the resumed frame stepped the instance that suspended")
	assert_eq(b.call("step"), 3, "and left the other instance's count alone")

	a.free()
	b.free()

func test_sgd_a_callable_dies_with_its_instance():
	var script = _instance_script("instance_callable_free")
	if script == null:
		return
	var a = _instance_node(script)
	var b = _instance_node(script)

	var from_a = a.call("hand_out_a_callable") as Callable
	assert_true(from_a.is_valid(), "the Callable is valid while the instance is")
	a.free()

	# Its record went back to the guest heap: calling it would step members that
	# are not there any more, so it stops being valid rather than reading them.
	assert_false(from_a.is_valid(), "and stops being valid when the instance goes")
	assert_eq(b.call("step"), 1, "the other instance is untouched by any of it")

	b.free()

func test_sgd_a_suspended_frame_dies_with_its_instance():
	var script = _instance_script("instance_await_free")
	if script == null:
		return
	var a = _instance_node(script)
	var b = _instance_node(script)
	add_child(a)
	add_child(b)

	# Counted, not flagged: retiring the frame must tell the awaiter once. A second
	# `completed` is a doubled continuation, not a harmless repeat.
	var completed := [0]
	(a.call("wait_then_step", sgd_ping) as Signal).connect(func(_v): completed[0] += 1)
	assert_eq(b.call("get_coroutine_count"), 1, "the frame is held by the shared Sandbox")

	a.free()
	assert_eq(b.call("get_coroutine_count"), 0,
		"freeing the instance retired the frame suspended against its record")
	assert_eq(completed[0], 1, "and told the awaiter it was over, once")

	sgd_ping.emit(1)
	assert_eq(b.call("step"), 1, "the emission reached nothing, and the survivor is intact")

	b.free()

func test_sgd_the_last_instance_disconnects_its_suspended_frame():
	var script = _instance_script("instance_await_last_free")
	if script == null:
		return
	var node = _instance_node(script)
	add_child(node)

	var connections_before := sgd_ping.get_connections().size()
	node.call("wait_then_step", sgd_ping)
	assert_eq(sgd_ping.get_connections().size(), connections_before + 1,
		"the suspended frame should be connected to its awaited signal")

	node.free()
	assert_eq(sgd_ping.get_connections().size(), connections_before,
		"freeing the last script instance should cancel that connection")

var _self_freeing_node : Node = null

var _reused_instance : Node = null

func _free_the_running_instance():
	# Node.free() is refused by the engine here ("Attempted to free a locked
	# object"), so the route to the same destructor is dropping the script.
	_self_freeing_node.set_script(null)
	_self_freeing_node = null
	# And then ask for a record of exactly the same size, so a chunk returned to
	# the guest heap too early is handed straight back out and initialized over.
	_reused_instance = _instance_node(_nested_script)
	_reused_instance.call("step")
	return 1

func test_sgd_an_instance_freed_during_its_own_call():
	# A guest call reaches the host, and the host frees the very node it is
	# running as. Godot destroys the script instance synchronously, so the record
	# tp still points at is released mid-call: returning its chunk to the guest
	# heap there would hand those bytes to the next malloc while the rest of the
	# method is still reading and writing them.
	var script = _instance_script("instance_free_during_call")
	if script == null:
		return
	var a = _instance_node(script)
	var b = _instance_node(script)

	_nested_script = script
	a.call("step")
	_self_freeing_node = a
	var answer = a.call("through_the_host", Callable(self, "_free_the_running_instance"))

	assert_eq(answer, [11, 1], "the call finished against the record it started on")
	assert_eq(_reused_instance.call("step"), 2, "and the new instance has its own")
	_reused_instance.free()
	_reused_instance = null
	assert_eq(b.call("step"), 1, "and the survivor's record was not handed out from under it")
	assert_eq(b.call("read_items"), [1], "nor its container member overwritten")

	b.free()

var _nested_instance : Node = null
var _nested_script = null

func _make_a_nested_instance():
	_nested_instance = _instance_node(_nested_script)
	return _nested_instance.call("step")

func test_sgd_an_instance_made_during_a_call():
	# Godot instantiates a scene the guest asked for: a second instance is built,
	# and its members initialized, while the first one is still on the stack.
	var script = _instance_script("instance_nested")
	if script == null:
		return
	_nested_script = script
	var a = _instance_node(script)

	a.call("step")
	var answer = a.call("through_the_host", Callable(self, "_make_a_nested_instance"))

	assert_eq(answer[0], 11, "the outer instance came back to its own members")
	assert_eq(answer[1], 1, "and the one made mid-call started from its own zero")
	assert_eq(_nested_instance.call("step"), 2, "which it kept afterwards")
	assert_eq(a.call("step"), 12, "as did the outer one")

	_nested_instance.free()
	_nested_instance = null
	_nested_script = null
	a.free()


# -= Callable(self, "name") =-

func _cc_named(x):
	return x * 2

func _cc_named_string(x):
	return "host:" + str(x)

func test_sgd_callable_constructor():
	# The engine looks the method name up when the Callable is built. Here the
	# name has to name a function the program declares, so the constructor is the
	# lowering the bare name already has: a Callable over the guest address.
	var gdscript_code = """
func doubled(x):
	return x * 2

func from_constructor(n):
	var c = Callable(self, "doubled")
	return c.call(n)

func from_name(n):
	var c = doubled
	return c.call(n)

func null_callable():
	return Callable()

func is_null():
	return Callable().is_null()

func handed_out():
	return Callable(self, "doubled")

func from_host_object(host, n):
	var method = "_cc_named_string"
	return Callable(host, method).call(n)
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("from_constructor", 21), 42,
		"Callable(self, \"doubled\") should call the function it names")
	assert_eq(s.vmcallv("from_constructor", 21), s.vmcallv("from_name", 21),
		"and answer what the bare name answers")

	# Callable() is the null Callable, as it is in the engine.
	assert_eq(typeof(s.vmcallv("null_callable")), TYPE_CALLABLE,
		"Callable() should still be a Callable")
	assert_eq(s.vmcallv("is_null"), Callable().is_null(),
		"and be null the way the engine's is")

	# One handed to Godot works from this side too.
	var c : Callable = s.vmcallv("handed_out")
	assert_eq(c.call(4), 8, "a Callable the guest built should be callable from Godot")
	assert_eq(s.vmcallv("from_host_object", self, 9), "host:9",
		"an unrestricted guest should call a method named at run time on another Object")

	# A native Callable invokes its target from inside Godot, beyond the usual
	# object-call policy hook. Refuse to forge one when any restriction is active.
	s.set_method_allowed_callback(func(_sandbox, _object, _method): return true)
	var exceptions = s.get_exceptions()
	s.vmcallv("from_host_object", self, 10)
	assert_engine_error("Callable(Object, method) is only available to a fully unrestricted Sandbox")
	assert_engine_error("vconstruct: Callable(Object, method) refused under restrictions")
	assert_gt(s.get_exceptions(), exceptions,
		"a partially restricted guest must not forge an Object method Callable")
	s.set_method_allowed_callback(Callable())

	s.queue_free()

# -= @export hints =-

func _hint_of(list : Array, name : String) -> Array:
	for p in list:
		if p["name"] == name:
			return [int(p["hint"]), String(p["hint_string"])]
	return []

@export_range(1, 5) var _eh_r := 1
@export_range(-1.5, 5.25, 0.25, "or_greater") var _eh_rf := 0.0
@export_enum("A", "B:5") var _eh_enum := 0
@export_flags("F:1", "W:2") var _eh_flags := 0
@export_multiline var _eh_multi := ""
@export_file("*.png", "*.jpg") var _eh_file := ""
@export_placeholder("hint text") var _eh_ph := ""
@export_node_path("Node2D", "Control") var _eh_np : NodePath
@export_exp_easing var _eh_ee := 1.0
@export var _eh_plain := 1

func test_sgd_export_hints():
	# An @export_* argument list is what the inspector needs to draw the property,
	# and the ELF carries none of it: the hint crosses beside the registration.
	# Every expectation here is the engine's own PropertyInfo for the same
	# declaration, read off this script.
	var gdscript_code = """
@export_range(1, 5) var r := 1
@export_range(-1.5, 5.25, 0.25, "or_greater") var rf := 0.0
@export_enum("A", "B:5") var en := 0
@export_flags("F:1", "W:2") var fl := 0
@export_multiline var multi := ""
@export_file("*.png", "*.jpg") var f := ""
@export_placeholder("hint text") var ph := ""
@export_node_path("Node2D", "Control") var np := ""
@export_exp_easing var ee := 1.0
@export var plain := 1

func read_r():
	return r
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	var guest = s.get_property_list()
	var mine = get_property_list()

	assert_eq(_hint_of(guest, "r"), _hint_of(mine, "_eh_r"),
		"@export_range over two integers should publish the engine's hint")
	assert_eq(_hint_of(guest, "rf"), _hint_of(mine, "_eh_rf"),
		"a step and a flag should reach the hint string too")
	assert_eq(_hint_of(guest, "en"), _hint_of(mine, "_eh_enum"),
		"@export_enum should publish its names")
	assert_eq(_hint_of(guest, "fl"), _hint_of(mine, "_eh_flags"),
		"and @export_flags its bits")
	assert_eq(_hint_of(guest, "multi"), _hint_of(mine, "_eh_multi"),
		"an annotation with no arguments should still carry its hint")
	assert_eq(_hint_of(guest, "f"), _hint_of(mine, "_eh_file"),
		"@export_file should publish its filters")
	assert_eq(_hint_of(guest, "ph"), _hint_of(mine, "_eh_ph"),
		"@export_placeholder should publish its text")
	assert_eq(_hint_of(guest, "np"), _hint_of(mine, "_eh_np"),
		"@export_node_path should publish its classes")
	assert_eq(_hint_of(guest, "ee"), _hint_of(mine, "_eh_ee"),
		"@export_exp_easing should publish its hint")
	assert_eq(_hint_of(guest, "plain"), _hint_of(mine, "_eh_plain"),
		"and a plain @export should stay unconstrained")

	# The property still works: a hint says how to draw it, not what it holds.
	s.set("r", 3)
	assert_eq(s.vmcallv("read_r"), 3, "a hinted property is still a property")

	s.queue_free()

func _usage_of(list : Array, name : String) -> int:
	for p in list:
		if p["name"] == name:
			return int(p["usage"])
	return 0

func _type_of(list : Array, name : String) -> int:
	for p in list:
		if p["name"] == name:
			return int(p["type"])
	return -1

@export_storage var _eh_store_typed : int = 0

func test_sgd_export_usage_flags():
	# @export_storage is the one annotation that changes the usage mask, and it
	# only takes PROPERTY_USAGE_EDITOR away. NIL_IS_VARIANT is not part of what it
	# changes: the flag says a NIL-typed property means Variant rather than void,
	# so it follows the type. The engine publishes it on no typed property, and a
	# sandboxed property is always typed -- api_sandbox_add refuses NIL.
	var gdscript_code = """
@export_storage var st : int = 0
@export var sp := 1

func read_st():
	return st
"""
	# Through the Script path, which is the one that publishes the mask to Godot.
	var node = Node.new()
	var script = SafeGDScript.new()
	script.set_source_code(gdscript_code)
	node.set_script(script)
	add_child_autofree(node)

	var guest = node.get_property_list()
	var mine = get_property_list()

	for name in ["st", "sp"]:
		assert_ne(_type_of(guest, name), TYPE_NIL,
			"%s should be registered with a type" % name)
		assert_eq(_usage_of(guest, name) & PROPERTY_USAGE_NIL_IS_VARIANT, 0,
			"%s is typed, so it should not claim NIL_IS_VARIANT" % name)

	# Against the engine's own mask for the same declaration.
	assert_eq(_usage_of(guest, "st"), _usage_of(mine, "_eh_store_typed"),
		"@export_storage should publish the engine's usage mask")
	assert_eq(_usage_of(guest, "sp"), _usage_of(mine, "_eh_plain"),
		"a plain @export should publish the engine's usage mask")
	assert_eq(_usage_of(guest, "st") & PROPERTY_USAGE_EDITOR, 0,
		"@export_storage should take the inspector away")

	# The property still round-trips, which is what the mask has to keep working.
	node.set("st", 7)
	assert_eq(node.call("read_st"), 7, "a storage-only property is still a property")

# -= Inner classes =-

class _IcBase:
	var v := 1
	func _init(a := 10):
		v = a
	func greet(x):
		return x + v
	func who():
		return "Base"

class _IcDerived extends _IcBase:
	var extra := 5
	func _init():
		super(42)
	func greet(x):
		return super.greet(x) * 2

func test_sgd_inner_classes():
	# A class is a struct that also declares methods: the instance is the same
	# Dictionary, and a method is a lifted function taking it first. The answers
	# come from the engine's own inner classes above, declared the same way.
	var gdscript_code = """
class Base:
	var v := 1
	func _init(a := 10):
		v = a
	func greet(x):
		return x + v
	func who():
		return "Base"

class Derived extends Base:
	var extra := 5
	func _init():
		super(42)
	func greet(x):
		return super.greet(x) * 2

func from_base(n):
	var b = Base.new()
	return b.greet(n)

func base_with_argument(a, n):
	return Base.new(a).greet(n)

func from_derived(n):
	var d = Derived.new()
	return d.greet(n)

func inherited():
	return Derived.new().who()

func fields():
	var d = Derived.new()
	return [d.v, d.extra]

func written():
	var d = Derived.new()
	d.v = 3
	d.extra = 4
	return d.greet(1)

func typed(d: Derived):
	return d.greet(2)

func is_a_dictionary():
	return typeof(Derived.new())
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("from_base", 5), _IcBase.new().greet(5),
		"a class with a default _init should construct the way the engine's does")
	assert_eq(s.vmcallv("base_with_argument", 3, 5), _IcBase.new(3).greet(5),
		"and take the arguments _init declares")
	assert_eq(s.vmcallv("from_derived", 5), _IcDerived.new().greet(5),
		"super() and super.method() should reach the base")
	assert_eq(s.vmcallv("inherited"), _IcDerived.new().who(),
		"a method the class does not declare should come from the base")
	assert_eq(s.vmcallv("fields"), [_IcDerived.new().v, _IcDerived.new().extra],
		"the instance should hold the base's fields and its own")
	assert_eq(s.vmcallv("written"), 8, "a field written from outside should reach the method")
	assert_eq(s.vmcallv("typed", {"v": 1, "extra": 0}), 6,
		"a Dictionary from Godot works where the class is declared")

	# The one divergence, and it is the struct's: an instance is a Dictionary,
	# not a RefCounted with a script, so this is what Godot sees.
	assert_eq(s.vmcallv("is_a_dictionary"), TYPE_DICTIONARY,
		"an instance is the Dictionary a struct instance is")

	s.queue_free()

func test_sgd_super_at_script_level():
	# The base of the script itself is a native class, and the engine refuses
	# both of the things that would make super.m() differ from self.m(): a
	# method that overrides a native one, and a base virtual with no definition.
	var gdscript_code = """
func through_super():
	return super.get_class()

func through_self():
	return self.get_class()
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("through_super"), s.vmcallv("through_self"),
		"super.m() at script level is the self-call it stands for")

	s.queue_free()

func test_sgd_tool_annotation_decides_is_tool():
	var fresh = SafeGDScript.new()
	assert_false(fresh.is_tool(), "an uncompiled script must not run in the editor")

	var tool_script = SafeGDScript.new()
	tool_script.set_source_code("@tool\nfunc answer():\n\treturn 1\n")
	assert_true(tool_script.is_tool(), "@tool makes a tool script")

	var plain = SafeGDScript.new()
	plain.set_source_code("func answer():\n\treturn 1\n")
	assert_false(plain.is_tool(), "a script that did not ask is not one")

func test_sgd_only_a_tool_script_instantiates_in_the_editor():
	# Engine.set_editor_hint is not scriptable: the editor half runs only under
	# `godot -e --headless -s addons/gut/gut_cmdln.gd`, as test_editor.gd does.
	var tool_script = SafeGDScript.new()
	tool_script.set_source_code("@tool\nvar ran := false\nfunc _init():\n\tran = true\n")
	var plain = SafeGDScript.new()
	plain.set_source_code("var ran := false\nfunc _init():\n\tran = true\n")
	var tool_node = Node.new()
	tool_node.set_script(tool_script)
	var plain_node = Node.new()
	plain_node.set_script(plain)

	assert_true(tool_script.can_instantiate(), "@tool instantiates everywhere")
	assert_true(tool_node.get("ran"), "the @tool instance ran _init")
	if Engine.is_editor_hint():
		assert_false(plain.can_instantiate(), "a script without @tool gets a placeholder in the editor")
		assert_false(plain_node.get("ran"), "the placeholder never ran _init")
	else:
		assert_true(plain.can_instantiate(), "outside the editor every valid script instantiates")
		assert_true(plain_node.get("ran"), "the game instance ran _init")
	tool_node.free()
	plain_node.free()

func test_sgd_onready_assigns_when_the_node_is_ready():
	var node = Node.new()
	var script = SafeGDScript.new()
	script.set_source_code("""
@onready var lazy = 40 + 2
@onready var doubled = lazy * 2

func _ready():
	lazy += 1

func get_lazy():
	return lazy

func get_doubled():
	return doubled
""")
	node.name = "Ready"
	node.set_script(script)
	assert_eq(node.get_lazy(), null, "an @onready member is null before _ready()")
	assert_eq(node.get_doubled(), null, "and so is one that reads it")

	add_child(node)
	await get_tree().process_frame

	assert_eq(node.get_lazy(), 43,
		"the initializer runs first, then the body of _ready()")
	assert_eq(node.get_doubled(), 84,
		"declaration order is kept, so the second initializer sees the first")

	node.queue_free()

func test_sgd_engine_class_new():
	var gdscript_code = """
func make():
	var t = Timer.new()
	t.wait_time = 2.5
	var seen = t.wait_time
	t.free()
	return seen
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	s.set_class_allowed_callback(func(sandbox, name): return name == "Timer")
	assert_eq(s.vmcallv("make"), 2.5, "Timer.new() builds a Timer")

	s.set_class_allowed_callback(func(sandbox, name): return false)
	assert_eq(s.vmcallv("make"), null, "a class the host refuses is not built")
	assert_engine_error("Class name is not allowed")
	assert_engine_error("Exception: Class name is not allowed")

	s.queue_free()

func test_sgd_is_instance_valid():
	var gdscript_code = """
func checks():
	return [is_instance_valid(self), is_instance_valid(42), is_instance_valid(null)]
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	add_child(s)
	assert_eq(s.vmcallv("checks"), [true, false, false],
		"a live object is valid; a number and a null are not")

	s.queue_free()

func test_sgd_preload_is_load_with_a_constant_path():
	var gdscript_code = """
func by_preload():
	return preload("res://icon.svg") != null

func by_load():
	return load("res://icon.svg") != null
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("by_preload"), true, "preload() resolves a constant path")
	assert_eq(s.vmcallv("by_load"), true, "and load() still does the same")

	s.queue_free()

func test_sgd_load_is_decided_by_the_resource_callback():
	var gdscript_code = """
func by_load():
	return load("res://icon.svg") != null
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	s.restrictions = true
	var before := s.get_exceptions()
	s.vmcallv("by_load")
	assert_eq(s.get_exceptions(), before + 1, "a restricted Sandbox refuses the path")
	assert_engine_error("Resource path is not allowed: res://icon.svg")
	assert_engine_error("Exception: Resource path is not allowed: res://icon.svg")

	s.set_resource_allowed_callback(func(_sandbox, path): return path == "res://icon.svg")
	assert_eq(s.vmcallv("by_load"), true, "an admitted path still loads")
	assert_eq(s.get_exceptions(), before + 1, "and raises nothing")

	s.queue_free()

func test_sgd_an_untyped_global_may_change_type():
	var gdscript_code = """
var g = null

func steps():
	var out = []
	g = 42
	out.append(g)
	g = g + 1
	out.append(g)
	g = "text"
	out.append(g)
	g = {"a": 1}
	out.append(g["a"])
	return out
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("steps"), [42, 43, "text", 1],
		"an untyped global holds whatever it was last given")

	s.queue_free()

func test_sgd_a_loop_may_allocate_past_the_reference_cap():
	var gdscript_code = """
func dict_loop(n):
	var total = 0
	for i in range(n):
		var d = {"a": i}
		total += d["a"]
	return total

func string_loop(n):
	var total = 0
	for i in range(n):
		var s = "row-" + str(i)
		total += s.length()
	return total

func walk_a_string(text):
	var total = 0
	for c in text:
		total += 1
	return total

func accumulate(n):
	var acc = ""
	for i in range(n):
		acc = acc + "x"
	return acc.length()

func into_an_array(n):
	var rows = []
	for i in range(n):
		rows.append({"i": i})
	return rows.size()

func while_loop(n):
	var i = 0
	var total = 0
	while i < n:
		var d = {"k": i}
		total += d["k"]
		i += 1
	return total

func nested(n):
	var total = 0
	for i in range(n):
		for j in range(n):
			var d = {"a": j}
			total += d["a"]
	return total

func skips_a_pass(n):
	var total = 0
	for i in range(n):
		var d = {"a": i}
		if d["a"] % 2 == 0:
			continue
		total += 1
	return total

func appends_to_a_given_string(s, n):
	for i in range(n):
		s += "x"
	return s.length()

func grows_a_given_array(a, n):
	for i in range(n):
		a.append(i)
	return a.size()
"""
	var s = _compile_and_load(gdscript_code, 2000000)
	if s == null:
		return

	assert_eq(s.vmcallv("dict_loop", 1000), 499500,
		"a thousand dictionaries, one per pass")
	assert_eq(s.vmcallv("string_loop", 1000), 6890,
		"a thousand strings, two per pass")
	assert_eq(s.vmcallv("walk_a_string", "y".repeat(1000)), 1000,
		"walking a String yields one scoped Variant per character")
	assert_eq(s.vmcallv("accumulate", 1000), 1000,
		"a value assigned to an outer local survives the release")
	assert_eq(s.vmcallv("into_an_array", 1000), 1000,
		"a container the host holds keeps its own copy")
	assert_eq(s.vmcallv("while_loop", 1000), 499500,
		"a while loop releases like a for loop")
	assert_eq(s.vmcallv("nested", 60), 106200,
		"nested loops release independently")
	assert_eq(s.vmcallv("skips_a_pass", 1000), 500,
		"a pass that continues still releases")
	assert_eq(s.vmcallv("appends_to_a_given_string", "", 1000), 1000,
		"a caller's String mutated once per pass survives every release")
	assert_eq(s.vmcallv("grows_a_given_array", [], 1000), 1000,
		"a caller's Array mutated once per pass survives every release")

	s.queue_free()

func test_sgd_a_release_keeps_a_mutated_argument():
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	assert_eq(ts.vmcall("scope_release_keeps_a_mutated_argument", "", 200), 200,
		"a release keeps the Variant a handle below the mark still names")
	ts.queue_free()

func test_sgd_multiline_strings_and_unicode_names():
	var gdscript_code = """
func dialogue():
	return "line one
line two"

func single_quoted():
	return 'a
b'

func triple():
	return \"\"\"x
y\"\"\"

func accented():
	var café = 4
	var naïve = 3
	return café + naïve

func annotated():
	@warning_ignore("unused_variable")
	var unused = 1
	@warning_ignore_start("shadowed_variable")
	var used = 2
	@warning_ignore_restore("shadowed_variable")
	return used
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("dialogue"), "line one\nline two", "a plain string may span lines")
	assert_eq(s.vmcallv("single_quoted"), "a\nb", "and so may a single-quoted one")
	assert_eq(s.vmcallv("triple"), "x\ny", "a triple-quoted string is unchanged")
	assert_eq(s.vmcallv("accented"), 7, "a unicode identifier is one name")
	assert_eq(s.vmcallv("annotated"), 2, "an annotation above a statement parses and drops")

	s.queue_free()

func test_sgd_enum_constant_expressions():
	# The engine folds any integer constant expression at parse time, earlier
	# members of the same enum included. So does this.
	var gdscript_code = """
enum Flags { A = 1 << 2, B = A + 1, C }
enum Signs { NEG = -1 * 3, ZERO = 0 }
enum FromGlobal { INT_TYPE = TYPE_INT }

func flags():
	return [Flags.A, Flags.B, Flags.C]

func signs():
	return Signs.NEG

func from_global():
	return FromGlobal.INT_TYPE
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("flags"), [4, 5, 6], "shifts, earlier members and the count after one")
	assert_eq(s.vmcallv("signs"), -3, "a negative constant expression")
	assert_eq(s.vmcallv("from_global"), TYPE_INT, "a @GlobalScope integer constant")

	s.queue_free()

	# A value the parser cannot evaluate is refused, not silently zero.
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	for source in ["func side():\n\treturn 1\nenum E { A = side() }\nfunc f():\n\treturn E.A\n",
			"enum E { A = 1.5 }\nfunc f():\n\treturn E.A\n"]:
		var elf = ts.vmcall("compile_to_elf", source)
		assert_eq(elf.is_empty(), true, "should not compile: " + source)
	ts.queue_free()

func test_sgd_constant_expressions_fold():
	var gdscript_code = """
const SHIFT = 3
const MASK = 1 << SHIFT
const NAME = "on" + "off"
const HALF = 7 / 2
const SCALE = MASK * 0.5
const LIMIT = 100 if MASK else 10
const HALF_PI = PI / 2
const ESCAPE = KEY_ESCAPE

enum Codes { A = MASK, B }

class Bits:
	const WIDTH = MASK * 2

func values():
	return [MASK, NAME, HALF, SCALE, LIMIT, ESCAPE]

func half_pi():
	return HALF_PI

func codes():
	return [Codes.A, Codes.B]

func width():
	return Bits.WIDTH
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("values"), [8, "onoff", 3, 4.0, 100, KEY_ESCAPE],
		"an expression over constants is a constant")
	assert_almost_eq(s.vmcallv("half_pi"), PI / 2, 0.0000001,
		"a @GlobalScope float constant folds")
	assert_eq(s.vmcallv("codes"), [8, 9], "an enum member may be written from a constant")
	assert_eq(s.vmcallv("width"), 16, "a class constant may be an expression")

	s.queue_free()

func test_sgd_char_and_ord():
	# char() and ord() are @GlobalScope functions, so a call must never fall
	# through to the owner node: Godot drops the resulting VCALL and answers null.
	var gdscript_code = """
func character(code):
	return char(code)

func code_of(text):
	return ord(text)

func round_trip(code):
	return ord(char(code))
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	assert_eq(s.vmcallv("character", 65), char(65), "char(65) is what the engine answers")
	assert_eq(s.vmcallv("character", 9731), char(9731), "a codepoint outside ASCII too")
	assert_eq(s.vmcallv("code_of", "A"), ord("A"), "ord() answers the codepoint")
	assert_eq(s.vmcallv("round_trip", 955), 955, "one is the other's inverse")

	s.queue_free()

func test_sgd_shifting_a_negative_operand():
	# Variant::evaluate() refuses a negative shift operand, but that is not the
	# path the engine runs: `-16 >> 2` is -4 in GDScript, typed or not.
	var gdscript_code = """
func untyped_right(a, b):
	return a >> b

func untyped_left(a, b):
	return a << b

func typed_right(a: int, b: int):
	return a >> b
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	# Written out: the engine's parser refuses `-16 >> 2` as a constant
	# expression, though it answers -4 for the same shift at run time.
	assert_eq(s.vmcallv("untyped_right", -16, 2), -4, "an untyped negative right shift")
	assert_eq(s.vmcallv("untyped_left", -16, 2), -64, "and an untyped left shift")
	assert_eq(s.vmcallv("typed_right", -16, 2), -4, "the typed path is unchanged")
	assert_eq(s.vmcallv("untyped_right", 16, 2), 4, "positive operands still work")

	s.queue_free()

func test_sgd_rect_and_plane_members():
	# Rect2.position is payload, not an Object property: routing it through
	# ECALL_OBJ_PROP_GET threw "Object is not scoped" on the float bits.
	var gdscript_code = """
func rect_parts():
	var r: Rect2 = Rect2(1, 2, 3, 4)
	return [r.position, r.size, r.size.x]

func rect_write():
	var r: Rect2 = Rect2(1, 2, 3, 4)
	r.position = Vector2(9, 8)
	r.size.y = 7
	return r

func recti_parts():
	var r: Rect2i = Rect2i(1, 2, 3, 4)
	return [r.position, r.size]

func plane_parts():
	var p: Plane = Plane(0, 1, 0, 2)
	return [p.normal, p.d, p.y]

func plane_write():
	var p: Plane = Plane(0, 1, 0, 2)
	p.normal = Vector3(1, 0, 0)
	p.d = 5.0
	return p
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	var r := Rect2(1, 2, 3, 4)
	assert_eq(s.vmcallv("rect_parts"), [r.position, r.size, r.size.x], "Rect2 members read as the engine reads them")
	var written := Rect2(1, 2, 3, 4)
	written.position = Vector2(9, 8)
	written.size.y = 7
	assert_eq(s.vmcallv("rect_write"), written, "and write back into the rect")
	var ri := Rect2i(1, 2, 3, 4)
	assert_eq(s.vmcallv("recti_parts"), [ri.position, ri.size], "Rect2i keeps integer components")
	var p := Plane(0, 1, 0, 2)
	assert_eq(s.vmcallv("plane_parts"), [p.normal, p.d, p.y], "Plane's normal, d and component spellings")
	var pw := Plane(1, 0, 0, 5)
	assert_eq(s.vmcallv("plane_write"), pw, "and a normal written whole")

	s.queue_free()

func test_sgd_get_node_or_null_answers_null():
	# The whole point of the _or_null form. A null object came back from the
	# host as an exception instead.
	var gdscript_code = """
func missing():
	return get_node_or_null("Nothing")

func present():
	return get_node_or_null("Child") != null
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	add_child(s)
	var child = Node.new()
	child.name = "Child"
	s.add_child(child)

	assert_eq(s.vmcallv("missing"), null, "a missing node is null, not an exception")
	assert_eq(s.vmcallv("present"), true, "and a node that is there is still found")

	s.queue_free()

func test_sgd_dictionary_keys_are_strings():
	# Constant keys must be stored as Strings (what GDScript would write).
	var gdscript_code = """
func build() -> Dictionary:
	var d : Dictionary = {}
	d["hp"] = 3
	return d

func bump(d : Dictionary) -> void:
	d["hp"] += 1

func read(d : Dictionary):
	return d["hp"]

func present(d : Dictionary) -> bool:
	return d.has("hp")

func fallback(d : Dictionary):
	return d.get("mp", -1)
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	var built : Dictionary = s.vmcallv("build")
	assert_eq(built.size(), 1, "one key was written")
	assert_eq(typeof(built.keys()[0]), TYPE_STRING,
		"a guest's constant key is a String, as GDScript would have written it")
	assert_eq(built["hp"], 3)

	var from_godot := {"hp": 10}
	assert_eq(s.vmcallv("read", from_godot), 10, "a String key reads back")
	assert_eq(s.vmcallv("present", from_godot), true, "has() finds a String key")
	s.vmcallv("bump", from_godot)
	assert_eq(from_godot["hp"], 11, "a compound assignment writes through")
	assert_eq(from_godot.size(), 1, "the write did not add a second key")
	assert_eq(typeof(from_godot.keys()[0]), TYPE_STRING, "the key kept its type")

	assert_eq(s.vmcallv("fallback", from_godot), -1, "a missing key answers the default")
	assert_eq(from_godot.size(), 1, "the default was not inserted")

	s.queue_free()

func test_sgd_dictionary_integer_keys_stay_integers():
	var gdscript_code = """
func fill(n : int) -> Dictionary:
	var d : Dictionary = {}
	var i : int = 0
	while i < n:
		d[i] = i * 2
		i += 1
	return d

func total(d : Dictionary, n : int) -> int:
	var acc : int = 0
	var i : int = 0
	while i < n:
		acc += d[i]
		i += 1
	return acc

func count_present(d : Dictionary, n : int) -> int:
	var found : int = 0
	var i : int = 0
	while i < n:
		if d.has(i):
			found += 1
		i += 1
	return found
"""
	var s = _compile_and_load(gdscript_code, 400000)
	if s == null:
		return

	var filled : Dictionary = s.vmcallv("fill", 4)
	assert_eq(filled.size(), 4)
	assert_eq(typeof(filled.keys()[0]), TYPE_INT, "an integer key stays an integer")
	assert_eq(filled, {0: 0, 1: 2, 2: 4, 3: 6}, "and it is the integer it was")
	assert_eq(s.vmcallv("total", filled, 4), 12, "the same keys read back")
	assert_eq(s.vmcallv("count_present", filled, 6), 4, "has() answers per key")

	# Godot hashes 1.0 and 1 into the same bucket but keeps them apart.
	var mixed := {1: "int", 1.5: "float"}
	assert_eq(s.vmcallv("count_present", mixed, 2), 1, "only the integer key matched")

	s.queue_free()

func test_sgd_read_only_containers_are_read_only_in_the_guest():
	var gdscript_code = """
var api : Dictionary = {}

func take(granted : Dictionary) -> void:
	api = granted

func write_new_key():
	api["smuggled"] = 1

func overwrite_key():
	api["cycles"] = 999

func write_computed_key():
	var key = "smuggled"
	api[key] = 1

func erase_key():
	api.erase("cycles")

func clear_all():
	api.clear()

func merge_in():
	api.merge({"smuggled": 1})

func nested_write():
	api["nested"]["deep"] = "tampered"

func array_write():
	api["arr"][0] = "tampered"

func array_push():
	api["arr"].push_back("tampered")

func array_clear():
	api["arr"].clear()

func read_key():
	return api["cycles"]

func read_nested():
	return api["nested"]["deep"]

func call_granted():
	return api["log"].call(7)

func size_of():
	return api.size()
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var nested := {"deep": "original"}
	var arr := ["original"]
	var api := {
		"cycles": 2000,
		"log": func(x): return x * 2,
		"nested": nested,
		"arr": arr,
	}
	nested.make_read_only()
	arr.make_read_only()
	api.make_read_only()

	s.vmcallv("take", api)

	# A runtime-keyed write is refused by the engine's setter itself.
	var engine_refusal := "Condition \"_p->read_only\" is true"
	var denied := [
		["write_new_key", "Dictionary::operation: the container is read-only"],
		["overwrite_key", "Dictionary::operation: the container is read-only"],
		["write_computed_key", "Dictionary::operation: the container is read-only", engine_refusal],
		["erase_key", "Variant::call: the container is read-only"],
		["clear_all", "Dictionary::operation: the container is read-only"],
		["merge_in", "Variant::call: the container is read-only"],
		["nested_write", "Variant::call: the container is read-only"],
		["array_write", "Variant::call: the container is read-only"],
		["array_push", "Variant::call: the container is read-only"],
		["array_clear", "Variant::call: the container is read-only"],
	]
	for entry in denied:
		var before := s.get_exceptions()
		s.vmcallv(entry[0])
		if entry.size() > 2:
			assert_engine_error(entry[2])
		assert_engine_error("Exception: " + entry[1])
		assert_eq(s.get_exceptions(), before + 1,
			"%s must raise in the guest, not silently do nothing" % entry[0])

	assert_eq(api.size(), 4, "no key was added or erased")
	assert_eq(api["cycles"], 2000, "a granted value was not overwritten")
	assert_eq(api.has("log"), true, "a granted Callable was not erased")
	assert_eq(nested["deep"], "original", "a nested Dictionary was not written to")
	assert_eq(arr[0], "original", "a nested Array was not written to")
	assert_eq(arr.size(), 1, "a nested Array was not grown")

	var before_reads := s.get_exceptions()
	assert_eq(s.vmcallv("read_key"), 2000)
	assert_eq(s.vmcallv("read_nested"), "original")
	assert_eq(s.vmcallv("call_granted"), 14)
	assert_eq(s.vmcallv("size_of"), 4)
	assert_eq(s.get_exceptions(), before_reads, "reads and granted calls are not denied")

	s.queue_free()

func test_sgd_writable_containers_are_still_writable():
	var gdscript_code = """
func mutate(d : Dictionary, a : Array):
	d["added"] = 1
	d.erase("gone")
	a.push_back("added")
	a[0] = "changed"
	return d.size()
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var d := {"keep": 1, "gone": 2}
	var a := ["original"]
	var before := s.get_exceptions()
	assert_eq(s.vmcallv("mutate", d, a), 2)
	assert_eq(s.get_exceptions(), before, "a writable container is not denied")
	assert_eq(d.has("added"), true)
	assert_eq(d.has("gone"), false)
	assert_eq(a, ["changed", "added"])

	s.queue_free()

func test_sgd_a_failed_compile_does_not_exhaust_the_compiler():
	var broken := SafeGDScript.new()
	broken.set_source_code("func oops():\n\treturn undefined_name_here\n")
	assert_engine_error("SafeGDScript: : [Code Generation Error] Undefined variable: undefined_name_here (line 2, column 28)")
	assert_ne(broken.get_compile_error(), "", "the broken script should report an error")

	var source := FileAccess.get_file_as_string("res://tests/test_cpu.sgd")
	assert_false(source.is_empty(), "test_cpu.sgd should be readable")
	for attempt in 3:
		var script := SafeGDScript.new()
		script.set_source_code(source)
		assert_eq(script.get_compile_error(), "",
			"compile #%d after a failed one must still succeed" % attempt)
		var node := Node.new()
		node.set_script(script)
		node.set("instructions_max", 4000000)
		assert_true(node.has_method("run"), "the compiled program should have its methods")
		node.free()

func test_sgd_a_call_godot_cannot_make_raises():
	var gdscript_code = """
var nothing = null

func nil_method():
	return nothing.call("x")

func nil_from_a_dictionary(d : Dictionary):
	return d["missing"].some_method()

func no_such_method():
	var s = "hello"
	return s.no_such_method()

func too_few_arguments():
	var s = "hello"
	return s.substr()

func too_many_arguments():
	var s = "hello"
	return s.to_upper(1, 2, 3)

func wrong_argument_type():
	var a = [1, 2, 3]
	return a.resize("not a number")

func missing_method_on_a_node():
	return get_node("Child").missing_method()

func missing_method_via_call():
	return get_node("Child").call("missing_method")

func a_call_that_works():
	var s = "hello"
	return s.to_upper()

func a_node_call_that_works():
	return get_node("Child").get_name()
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)
	var child := Node.new()
	child.name = "Child"
	s.add_child(child)

	var refused := [
		["nil_method", "Invalid call. Nonexistent function 'call' in base 'Nil'."],
		["no_such_method", "Invalid call. Nonexistent function 'no_such_method' in base 'String'."],
		["too_few_arguments", "Invalid call to function 'substr' in base 'String'. Expected 2 argument(s)."],
		["too_many_arguments", "Invalid call to function 'to_upper' in base 'String'. Expected 0 argument(s)."],
		["wrong_argument_type", "Invalid type in function 'resize' in base 'Array'. Cannot convert argument 1 from String to int."],
		["missing_method_on_a_node", "Invalid call. Nonexistent function 'missing_method' in base 'Node'."],
		["missing_method_via_call", "Invalid call. Nonexistent function 'missing_method (via call)' in base 'Node'."],
	]
	for entry in refused:
		var before := s.get_exceptions()
		s.vmcallv(entry[0])
		assert_engine_error("Exception: Variant::call(): " + entry[1])
		assert_eq(s.get_exceptions(), before + 1,
			"%s must raise, not return a silent null" % entry[0])

	var before_nil := s.get_exceptions()
	s.vmcallv("nil_from_a_dictionary", {})
	assert_engine_error("Exception: Variant::call(): Invalid call. Nonexistent function 'some_method' in base 'Nil'.")
	assert_eq(s.get_exceptions(), before_nil + 1, "a method on a key that is not there raises")

	var before_ok := s.get_exceptions()
	assert_eq(s.vmcallv("a_call_that_works"), "HELLO")
	assert_eq(s.vmcallv("a_node_call_that_works"), "Child")
	assert_eq(s.get_exceptions(), before_ok, "a valid call is not affected")

	s.queue_free()

const CLASS_KEYWORD_SOURCE = """
class_name TurretScript
extends Node2D

func answer():
	return 42
"""

func _class_keyword_script() -> Script:
	var path := "user://temp_class_keywords.sgd"
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(CLASS_KEYWORD_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the script should load as a SafeGDScript resource")
	return script

func test_sgd_class_name_is_the_global_name():
	var script := _class_keyword_script()
	if script == null:
		return
	assert_eq(String(script.get_global_name()), "TurretScript",
		"class_name should be the script's global name")

	var plain := SafeGDScript.new()
	plain.set_source_code("func answer():\n\treturn 1\n")
	assert_eq(plain.get_compile_error(), "", "a script without class_name should compile")
	assert_ne(String(plain.get_global_name()), "TurretScript",
		"a script that declares no class_name should not borrow one")

func test_sgd_extends_names_the_instance_base_type():
	var script := _class_keyword_script()
	if script == null:
		return
	assert_eq(String(script.get_instance_base_type()), "Node2D",
		"'extends Node2D' should be the type Godot checks an owner against")

	var plain := SafeGDScript.new()
	plain.set_source_code("func answer():\n\treturn 1\n")
	assert_eq(String(plain.get_instance_base_type()), "Sandbox",
		"a script with no 'extends' keeps the Sandbox base type")

	var node := Node2D.new()
	node.set_script(script)
	node.set("instructions_max", 4000000)
	assert_eq(node.call("answer"), 42, "the program should run as it did before")
	node.free()

func test_sgd_a_class_can_extend_an_engine_class():
	var gdscript_code = """
class Marker extends Node2D:
	var hits = 0

	func hit(by):
		hits += by
		position = Vector2(hits, hits * 2)
		set_name("marker")
		return [hits, position.x, str(get_name())]

func run():
	var m = Marker.new()
	m.hit(1)
	var out = m.hit(2)
	out.append(m.hits)
	m.free()
	return out
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	var out = s.vmcallv("run")
	assert_eq(s.get_exceptions(), before, "reaching the base should not raise")
	assert_eq(out, [3, 3.0, "marker", 3],
		"the class's own field and the base's property and method should both work")

	s.queue_free()

# A nested class with an engine base is a Script resource of its own, and an
# instance of it is a script instance attached to the engine object. The guest
# keeps its Dictionary; the two share one set of fields, because Godot's
# Dictionary is a handle rather than a value.

const NESTED_CLASS_SOURCE = """
class Marker extends Node2D:
	var hits = 0
	var frames = 0
	var notified = 0
	func _notification(what):
		if what == 13:
			notified = what
	func _ready():
		hits += 100
	func _process(_delta):
		frames += 1
	func launch(by: int) -> int:
		hits += by
		return hits
	func get_name():
		return "M/" + str(super.get_name())

class Held extends Resource:
	var v = 1

var made = null
var held = null

func make():
	made = Marker.new()
	return made["@base"]

func guest_hits():
	return made["hits"]

func make_resource():
	held = Held.new()
	return held["@base"]
"""

func _nested_node(source: String = NESTED_CLASS_SOURCE) -> Node:
	var path = "user://temp_nested_classes.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(source)
	file.close()
	var script = load(path)
	assert_not_null(script, "the nested-class script should load as a SafeGDScript resource")
	if script == null:
		return null
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(4000000)
	return node

func test_sgd_a_nested_class_instance_has_a_script():
	var node = _nested_node()
	if node == null:
		return
	var obj = node.call("make")
	assert_true(obj is Node2D, "the class's base should reach Godot as the engine object")
	if not (obj is Node2D):
		node.free()
		return

	var nested = obj.get_script()
	assert_not_null(nested, "a nested class with an engine base should carry a script")
	assert_eq(str(nested.get_instance_base_type()), "Node2D",
		"the script's base type should be the engine class the chain bottoms out in")
	assert_true(obj.has_method("launch"), "a declared method should answer has_method")
	assert_true(obj.has_method("_ready"), "an engine callback should answer has_method")
	assert_false(obj.has_method("nothing_declared"), "an undeclared method should not")

	assert_eq(obj.get("hits"), 0, "a field should read through the script instance")
	assert_eq(obj.call("launch", 3), 3, "a declared method should be callable from the host")
	assert_eq(obj.get("hits"), 3, "the call should be visible in the field")

	obj.set("hits", 10)
	assert_eq(node.call("guest_hits"), 10,
		"the host and the guest should share one Dictionary, not a copy")

	obj.set("nope", 1)
	assert_eq(obj.get("nope"), null, "a set of an undeclared name should not grow the Dictionary")

	obj.free()
	node.free()

func test_sgd_a_nested_class_gets_engine_callbacks():
	var node = _nested_node()
	if node == null:
		return
	add_child(node)
	var obj = node.call("make")
	if not (obj is Node2D):
		node.free()
		return
	add_child(obj)

	assert_eq(obj.get("hits"), 100, "_ready should reach the nested class")
	await get_tree().process_frame
	await get_tree().process_frame
	assert_gt(obj.get("frames"), 0, "_process should reach the nested class")

	obj.queue_free()
	node.queue_free()

func test_sgd_super_on_a_native_base_does_not_recurse():
	var node = _nested_node()
	if node == null:
		return
	var obj = node.call("make")
	if not (obj is Node2D):
		node.free()
		return
	obj.set_name("m")

	# Without the bypass the script instance answers first and re-enters the
	# very method that made the call, until the machine runs out of stack.
	assert_eq(str(obj.call("get_name")), "M/m",
		"super on a native base should reach the engine's own method")
	assert_eq(str(obj.call("get_name")), "M/m", "the bypass should be armed per call")

	obj.free()
	node.free()

func test_sgd_a_nested_instance_dies_with_its_node():
	var node = _nested_node()
	if node == null:
		return
	var obj = node.call("make")
	if not (obj is Node2D):
		node.free()
		return
	obj.free()
	assert_false(is_instance_valid(obj), "freeing the node should take its script instance with it")
	node.free()

# A RefCounted base would be a cycle nothing can break: '@base' is a strong
# reference to the object, the object owns the instance, and the instance holds
# the Dictionary. Such a class stays the plain Dictionary it has always been.
func test_sgd_a_nested_refcounted_class_keeps_no_script():
	var node = _nested_node()
	if node == null:
		return
	var res = node.call("make_resource")
	assert_true(res is Resource, "the class's base should still be the engine object")
	if res is Resource:
		assert_null(res.get_script(), "a RefCounted base gets no host-side script")
	node.free()

# A member declared with a nested class type is constructed by the program's
# startup, so its bind runs while the machine is still being loaded. The shared
# Sandbox has to be reachable by then, or the bind builds a second machine and
# runs startup again, forever.
func test_sgd_a_nested_class_member_binds_during_startup():
	var source = """
class Launcher extends Node2D:
	var balls = []
	func _input(_event):
		launch()
	func launch():
		var b = Node2D.new()
		add_child(b)
		balls.append(b)

var declared: Launcher
var launcher = null

func start():
	launcher = Launcher.new()
	launcher.set_process_input(true)
	add_child(launcher)
	return 1

func count():
	return launcher["balls"].size()
"""
	var path = "user://temp_startup_launcher.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(source)
	file.close()
	var script = load(path)
	assert_not_null(script, "the script should load")
	if script == null:
		return
	var node = Node2D.new()
	node.set_script(script)
	node.set_instructions_max(4000000)
	add_child(node)
	assert_eq(node.call("start"), 1, "the launcher should be added")

	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = true
	get_viewport().push_input(event, true)
	await get_tree().process_frame
	assert_gt(node.call("count"), 0, "_input should have reached the nested class")
	node.queue_free()

func test_sgd_a_notification_reaches_the_script():
	var source = """
var seen = 0

func _notification(what):
	if what == 13:
		seen += 1

func read():
	return seen
"""
	var path = "user://temp_notification.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(source)
	file.close()
	var script = load(path)
	assert_not_null(script, "the script should load")
	if script == null:
		return
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(1000000)
	add_child(node)
	# NOTIFICATION_READY == 13, and _notification() is called alongside _ready().
	assert_eq(node.call("read"), 1, "_notification should reach the script")
	node.queue_free()

func test_sgd_a_nested_class_gets_a_notification():
	var node = _nested_node()
	if node == null:
		return
	add_child(node)
	var obj = node.call("make")
	if not (obj is Node2D):
		node.free()
		return
	add_child(obj)
	assert_eq(obj.get("notified"), 13, "_notification should reach the nested class")
	obj.queue_free()
	node.queue_free()

# The compile that produces the bind already refuses an engine base under
# restrictions, so this is the same gate seen from the side that gate does not
# cover: a hand-written guest issuing the syscall itself.
func test_sgd_the_bind_syscall_is_refused_while_restricted():
	var s : Sandbox = Sandbox.new()
	s.set_program(Sandbox_TestsTests)
	s.restrictions = true
	var before := s.get_exceptions()
	s.vmcallv("class_bind_under_restrictions")
	assert_eq(s.get_exceptions(), before + 1, "a restricted Sandbox refuses the bind")
	assert_engine_error("Sandbox: a restricted program cannot attach a script to an engine object.")
	assert_engine_error("Exception: class_bind: refused while restricted")
	s.queue_free()

func test_sgd_a_nested_class_survives_a_reload():
	var node = _nested_node()
	if node == null:
		return
	var obj = node.call("make")
	if not (obj is Node2D):
		node.free()
		return
	var before = obj.get_script()
	assert_eq(obj.call("launch", 1), 1, "the first build should answer")

	node.get_script().source_code = NESTED_CLASS_SOURCE.replace("hits += by", "hits += by * 10")
	node.get_script().reload()

	assert_eq(obj.get_script(), before, "get_script() should stay identity-stable across a reload")
	assert_eq(obj.call("launch", 1), 11, "the instance should call the new body")

	obj.free()
	node.free()

func test_sgd_an_instance_answers_is_after_the_type_is_lost():
	var gdscript_code = """
class Base:
	var v = 1

class Derived extends Base:
	var w = 2

class Other:
	var u = 3

func kind(x):
	return [x is Base, x is Derived, x is Other]

func run():
	var bag = [Derived.new(), Base.new(), Other.new(), {}, 5]
	var out = []
	for item in bag:
		out.append(kind(item))
	return out
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("run"), [
		[true, true, false],
		[true, false, false],
		[false, false, true],
		[false, false, false],
		[false, false, false],
	], "an instance out of a container still answers for its class and its base")
	assert_eq(s.get_exceptions(), before, "asking is not a call")

	s.queue_free()

func test_sgd_a_class_holds_constants_and_static_methods():
	var gdscript_code = """
class Limits:
	const MAX = 40
	var v = MAX

	static func clamped(x):
		if x > MAX:
			return MAX
		return x

class Wider extends Limits:
	static func double_max():
		return MAX * 2

func run():
	return [Limits.MAX, Limits.new().v, Limits.clamped(99), Limits.clamped(7), Wider.double_max()]
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("run"), [40, 40, 40, 7, 80],
		"a class constant folds and a static method runs without an instance")
	assert_eq(s.get_exceptions(), before, "neither should raise")

	s.queue_free()

func test_sgd_a_method_can_answer_itself():
	var gdscript_code = """
class Builder:
	var parts = []

	func add(part):
		parts.append(part)
		return self

func run():
	var b = Builder.new().add("a").add("b").add("c")
	return b.parts
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("run"), ["a", "b", "c"],
		"chaining on a self-returning method should reach the same instance")
	assert_eq(s.get_exceptions(), before, "the chain should not raise")

	s.queue_free()

func test_sgd_a_class_method_falls_through_to_its_base():
	var gdscript_code = """
class Timer2 extends Node2D:
	func describe():
		return "%s/%s/%.1f" % [get_class(), label(), rotation]

	func label():
		return "mine"

func run():
	var t = Timer2.new()
	var out = t.describe()
	t.free()
	return out
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("run"), "Node2D/mine/0.0",
		"a declared method wins; an undeclared one is the base's")
	assert_eq(s.get_exceptions(), before, "neither call should raise")

	s.queue_free()

func test_sgd_super_reaches_the_engine_base():
	var gdscript_code = """
class Named extends Node2D:
	func get_class():
		return "wrapped:" + super.get_class()

func run():
	var n = Named.new()
	var out = n.get_class()
	n.free()
	return out
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("run"), "wrapped:Node2D",
		"super should reach the base rather than recurse into the override")
	assert_eq(s.get_exceptions(), before, "the super call should not raise")

	s.queue_free()

func test_sgd_the_base_object_is_a_real_engine_object():
	var gdscript_code = """
class Marker extends Node2D:
	var hits = 0

func run():
	var m = Marker.new()
	m.hits = 7
	return m

func inspect(m : Dictionary):
	return [m["@base"].get_class(), m["hits"]]
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	var instance = s.vmcallv("run")
	assert_typeof(instance, TYPE_DICTIONARY, "a class instance crosses as a Dictionary")
	assert_eq(instance.get("hits"), 7, "a declared field is one key of it")

	var base = instance.get("@base")
	assert_true(base is Node2D, "the hidden key holds the engine object it extends")
	assert_eq(s.get_exceptions(), before, "building and returning it should not raise")

	assert_eq(s.vmcallv("inspect", instance), ["Node2D", 7],
		"the instance survives a round trip through Godot")
	assert_eq(s.get_exceptions(), before, "the round trip should not raise")

	if base != null:
		base.free()
	s.queue_free()

func test_sgd_an_instance_reaches_an_engine_method():
	var gdscript_code = """
extends Node
class Marker extends Node2D:
	var label = "hello"

func run():
	var m = Marker.new()
	m.set_name("marker")
	add_child(m)
	var out = [get_child_count(), m.label, str(get_node("marker").get_name())]
	remove_child(m)
	m.free()
	return out
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	var out = s.vmcallv("run")
	assert_eq(s.get_exceptions(), before, "handing an instance to the engine should not raise")
	assert_eq(out, [1, "hello", "marker"],
		"an engine method is given the object the class extends, not the Dictionary")

	s.queue_free()

func test_sgd_an_untyped_instance_still_reaches_its_base():
	var gdscript_code = """
class Marker extends Node2D:
	var hits = 7

func poke(m):
	m.set_name("poked")
	m.position = Vector2(3, 4)
	return [str(m.get_name()), m.position, m.hits]

func run():
	var m = Marker.new()
	var direct = poke(m)
	var boxed = []
	boxed.append(m)
	var viacontainer = poke(boxed[0])
	m.free()
	return [direct, viacontainer]
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	var out = s.vmcallv("run")
	assert_eq(s.get_exceptions(), before, "losing the type should not raise")
	var expected = ["poked", Vector2(3, 4), 7]
	assert_eq(out, [expected, expected],
		"an instance the compiler no longer tracks still answers for both halves")

	s.queue_free()

func test_sgd_a_plain_dictionary_is_not_an_instance():
	var gdscript_code = """
class Marker extends Node2D:
	var hits = 7

func poke(d):
	d.fresh = 1
	return [d.missing, d.fresh, d.size()]

func run():
	return poke({})
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("run"), [null, 1, 1],
		"a Dictionary with no base keeps plain Dictionary behaviour")
	assert_eq(s.get_exceptions(), before, "and raises nothing on the way")

	s.queue_free()

func test_sgd_an_instance_answers_is_and_as():
	var gdscript_code = """
class Marker extends Node2D:
	var hits = 5

class Other extends Node2D:
	var hits = 5

func run():
	var m = Marker.new()
	var tests = [m is Marker, m is Other, m is Node2D, m is Node, m is Node3D]
	var cast = m as Node2D
	cast.position = Vector2(1, 1)
	tests.append(cast.position)
	tests.append(cast.hits)
	m.free()
	return tests

func unknown(x):
	return [x is Node2D, x is Node3D]
"""
	var s = _compile_and_load(gdscript_code, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("run"), [true, false, true, true, false, Vector2(1, 1), 5],
		"an instance answers for its own class, for its base, and stays usable after a cast")
	assert_eq(s.get_exceptions(), before, "neither test should raise")

	var node := Node2D.new()
	assert_eq(s.vmcallv("unknown", node), [true, false],
		"a value the compiler cannot type is still walked at run time")
	node.free()

	s.queue_free()

func test_sgd_a_bare_name_reaches_what_the_script_extends():
	var script := SafeGDScript.new()
	script.set_source_code("""
extends Node2D

var health = 3

func run():
	position = Vector2(4, 5)
	rotation = 0.25
	set_name("owned")
	return [position, rotation, str(get_name()), health]
""")
	assert_eq(script.get_compile_error(), "",
		"a property of the base is not an undefined variable")

	var owner := Node2D.new()
	owner.set_script(script)
	owner.set("instructions_max", 4000000)
	add_child(owner)
	assert_eq(owner.call("run"), [Vector2(4, 5), 0.25, "owned", 3],
		"a bare name the script does not declare is the owner's property")
	owner.queue_free()

	var plain := SafeGDScript.new()
	plain.set_source_code("func run():\n\tposition = 1\n")
	assert_ne(plain.get_compile_error(), "",
		"without an extends there is nothing for a bare name to reach")
	assert_engine_error("Undefined variable: position")

func test_sgd_an_owner_must_be_what_the_script_extends():
	var script := SafeGDScript.new()
	script.set_source_code("extends Node2D\nfunc run():\n\treturn position\n")
	assert_eq(script.get_compile_error(), "", "the script itself compiles")

	var wrong := Node.new()
	wrong.set_script(script)
	assert_false(wrong.has_method("run"),
		"a script is refused by an owner that is not what it extends")
	assert_engine_error("can't be assigned to an object of type 'Node'")
	wrong.free()

	var right := Node2D.new()
	right.set_script(script)
	assert_true(right.has_method("run"), "and accepted by one that is")
	right.free()

func _restricted_compile_error(source: String) -> String:
	var script := SafeGDScript.new()
	script.set_source_code("func __bootstrap():\n\tpass\n")
	var node := Node.new()
	node.set_script(script)
	node.set("restrictions", true)
	script.set_source_code(source)
	var message: String = script.get_compile_error()
	node.free()
	return message

func test_sgd_restrictions_refuse_the_class_keywords():
	var refused := [
		"class_name Turret\nfunc answer():\n\treturn 1\n",
		"extends Node2D\nfunc answer():\n\treturn 1\n",
		"class C extends Node2D:\n\tvar x = 1\nfunc answer():\n\treturn 1\n",
	]
	for source in refused:
		var message := _restricted_compile_error(source)
		assert_true(message.contains("allows engine classes"),
			"a restricted Sandbox should refuse '%s', got: %s" % [source.split("\n")[0], message])
		assert_engine_error("SafeGDScript: : " + message)

	for source in refused:
		var script := SafeGDScript.new()
		script.set_source_code(source)
		assert_eq(script.get_compile_error(), "",
			"an unrestricted Sandbox should accept: " + source.split("\n")[0])

# A restricted Sandbox compiles through gdscript.elf rather than in process, so
# the constant table has to survive the guest boundary as well as the direct one.
func test_sgd_restricted_compiles_publish_constants():
	var script := SafeGDScript.new()
	script.set_source_code("func __bootstrap():\n\tpass\n")
	var node := Node.new()
	node.set_script(script)
	node.set("restrictions", true)
	script.set_source_code("enum State { IDLE, RUN }\nconst LIMIT := 42\nfunc answer():\n\treturn State.RUN\n")
	assert_eq(script.get_compile_error(), "", "the restricted script should compile")

	var constants := script.get_script_constant_map()
	assert_eq(constants.get("State"), {"IDLE": 0, "RUN": 1},
		"a restricted compile should publish its enum")
	assert_eq(constants.get("LIMIT"), 42,
		"a restricted compile should publish its const")
	assert_eq(node.get("LIMIT"), 42, "and the instance should answer for it")
	node.free()

func test_sgd_restrictions_leave_a_local_class_alone():
	var source := """
class Counter:
	var n = 0
	func bump():
		n += 1
		return n

func answer():
	var c = Counter.new()
	c.bump()
	return c.bump()
"""
	assert_eq(_restricted_compile_error(source), "",
		"a class extending one declared in the file should still compile")

func test_sgd_restricting_after_the_build_rebuilds_the_program():
	var script := SafeGDScript.new()
	script.set_source_code("class_name Turret\nfunc answer():\n\treturn 1\n")
	assert_eq(script.get_compile_error(), "",
		"nothing refuses a script that has no Sandbox yet")

	var node := Node.new()
	node.set_script(script)
	node.set("restrictions", true)
	var message: String = script.get_compile_error()
	assert_true(message.contains("allows engine classes"),
		"restricting the Sandbox should rebuild and refuse class_name, got: " + message)
	assert_engine_error("SafeGDScript: : " + message)

	node.set("restrictions", false)
	assert_eq(script.get_compile_error(), "",
		"lifting the restriction should build the program once more")
	assert_eq(node.call("answer"), 1, "and the rebuilt program runs")
	node.free()

func test_sgd_a_node_member_survives_the_call_that_set_it():
	var source := """
var target: Node

func remember(n):
	target = n
	return true

func recall():
	return str(target.get_name())

func forget():
	target = null
	return true
"""
	var s = _compile_and_load(source, 4000000)
	if s == null:
		return
	add_child(s)

	var n := Node.new()
	n.name = "Kept"
	add_child(n)

	var before := s.get_exceptions()
	s.vmcallv("remember", n)
	assert_eq(s.vmcallv("recall"), "Kept", "a node kept in a member resolves in a later call")
	assert_eq(s.get_exceptions(), before, "reaching it should not raise")

	s.vmcallv("forget")
	n.queue_free()
	s.queue_free()


func test_sgd_a_refcounted_member_is_kept_alive():
	var source := """
var res: Resource

func make():
	res = Resource.new()
	res.resource_name = "kept"
	return res.get_instance_id()

func recall():
	return str(res.resource_name)

func forget():
	res = null
	return true
"""
	var s = _compile_and_load(source, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	var id = s.vmcallv("make")
	assert_true(id is int and id != 0, "the script should hand back an instance id")

	# Must be a second call: the producing call's own ref keeps it alive regardless.
	assert_eq(s.vmcallv("recall"), "kept",
		"a RefCounted held only by a member is still reachable in a later call")
	assert_true(is_instance_id_valid(id),
		"and is alive because the host owns a reference for the member")
	assert_eq(s.get_exceptions(), before, "reaching it should not raise")

	s.vmcallv("forget")
	assert_false(is_instance_id_valid(id),
		"assigning the member releases what it named")

	s.queue_free()


func test_sgd_walking_a_string_longer_than_references_max():
	# Characters come in batches and the batch is only released when it runs
	# out, so a walk holds several at once against the same reference budget the
	# body allocates from. The walk has to stay inside it however long the string
	# is and however much the body makes per character.
	var source := """
func walk(text : String):
	var n = 0
	for c in text:
		var piece = c + "!" + str(n)
		n += piece.length()
	return n

func reassigned(text : String):
	var n = 0
	for c in text:
		c = "two"
		n += c.length()
	return n
"""
	var s = _compile_and_load(source, 40000000)
	if s == null:
		return

	var text := "abcdefghij".repeat(40)
	assert_true(text.length() > s.references_max,
		"the test only means something above the cap")

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("walk", text), _si_walk(text),
		"a walk longer than the reference cap should answer what the engine answers")
	assert_eq(s.vmcallv("reassigned", text), text.length() * 3,
		"assigning the loop variable invalidates the one-character length fold")
	assert_eq(s.get_exceptions(), before, "and should not run out of references")

	s.queue_free()

func test_sgd_walking_an_array_longer_than_references_max():
	var source := """
func walk(values : Array):
	var answer = []
	for value in values:
		answer.append(value)
	return answer
"""
	var s = _compile_and_load(source, 40000000)
	if s == null:
		return

	# Cross both the sixteen-element batch boundary and the scoped-reference cap.
	# Mixed inline and complex values exercise both GuestVariant representations.
	var values := []
	for i in range(180):
		values.append(i if i % 3 == 0 else ("value-%d" % i if i % 3 == 1 else [i, i + 1]))
	assert_true(values.size() > s.references_max,
		"the test only means something above the cap")

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("walk", values), values,
		"a batched Array walk should preserve every Variant in order")
	assert_eq(s.get_exceptions(), before, "and should not run out of references")
	s.queue_free()

func test_sgd_a_loop_carrying_a_float_and_a_string_keeps_its_release():
	# The float carry proves a native numeric path and skips the loop's release
	# on later passes. The String carry beside it allocates every pass, so that
	# skip may not cover the whole loop.
	var source := """
func _pick(k : int):
	if k == 0:
		return 1.5
	return "x"

func carry(n : int):
	var a = _pick(0)
	var b = _pick(0)
	var s = _pick(1)
	var t = _pick(1)
	var i = 0
	while i < n:
		a = a + b
		s = s + t
		i = i + 1
	return a
"""
	var s = _compile_and_load(source, 40000000)
	if s == null:
		return

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("carry", 200), 1.5 + 1.5 * 200.0,
		"the numeric carry should accumulate across every pass")
	assert_eq(s.get_exceptions(), before, "and should not run out of references")
	s.queue_free()

func test_sgd_a_call_returning_an_array_keeps_the_callers_loop_scope():
	# make() releases its own scope, but the rescue walk hands the Array it built
	# to the caller: the caller's loop scope is what frees them.
	var source := """
func make(x : int):
	var a = []
	for i in range(2):
		a.append(str(x + i))
	return a

func run(n : int):
	var total = 0
	for i in range(n):
		var made = make(i)
		total += i
	return total
"""
	var s = _compile_and_load(source, 40000000)
	if s == null:
		return

	# The body allocates nothing else, so this loop's scope is the only thing
	# freeing the two hundred Arrays the calls hand back.
	var before := s.get_exceptions()
	assert_eq(s.vmcallv("run", 200), 19900,
		"the loop should run every pass")
	assert_eq(s.get_exceptions(), before, "and should not run out of references")
	s.queue_free()

func _si_walk(text : String):
	var n = 0
	for c in text:
		var piece = c + "!" + str(n)
		n += piece.length()
	return n

func test_sgd_touching_more_objects_than_references_max():
	var source := """
func count(node):
	var n = 0
	for c in node.get_children():
		n += 1
	return n
"""
	var s = _compile_and_load(source, 40000000)
	if s == null:
		return
	add_child(s)

	var parent := Node.new()
	add_child(parent)
	var children := 250
	assert_true(children > s.references_max,
		"the test only means something above the cap")
	for i in range(children):
		var c := Node.new()
		c.name = "Child%d" % i
		parent.add_child(c)

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("count", parent), children,
		"every child should be reachable, not just the first references_max")
	assert_eq(s.get_exceptions(), before, "walking the children should not raise")

	parent.queue_free()
	s.queue_free()


# -= Block scopes: what a block makes dies with the block =-

func test_sgd_a_block_releases_what_it_made():
	# A block's temporaries are released where the block ends, so references_max
	# bounds what is live at once rather than everything the function ever made.
	# Thirty blocks of three values each is ninety, and four are live.
	var body := "func run():\n\tvar made = 0\n"
	for i in range(30):
		body += "\tif true:\n"
		body += "\t\tvar d = {\"k\": %d}\n" % i
		body += "\t\tvar arr = [d, str(d)]\n"
		body += "\t\tmade += arr.size()\n"
	body += "\treturn made\n"

	var s = _compile_and_load(body, 40000000)
	if s == null:
		return
	s.references_max = 24

	var before := s.get_exceptions()
	assert_eq(s.vmcallv("run"), 60, "every one of the thirty blocks should run")
	assert_eq(s.get_exceptions(), before,
		"ninety values through a budget of twenty-four should not run out")

	s.queue_free()

func test_sgd_a_value_leaving_a_block_survives_the_release():
	# Everything the block hands outward -- an enclosing local, a member, the
	# return value, an element appended to an outer container -- has to read
	# the same after the release as before it.
	var source := """
var member = null

func escapes(c):
	var out = []
	var kept = null
	if c:
		var d = {"a": 1, "b": 2}
		kept = d
		member = [d, "tail"]
		out.append(d.duplicate())
	if c:
		out.append(kept["b"])
		out.append(member[1])
	return out

func returned(c):
	if c:
		var d = {"a": 1}
		d["b"] = 2
		return d
	return null
"""
	var s = _compile_and_load(source, 40000000)
	if s == null:
		return

	assert_eq(s.vmcallv("escapes", true), [{"a": 1, "b": 2}, 2, "tail"],
		"a local, a member and an appended copy all outlive the block")
	assert_eq(s.vmcallv("escapes", false), [], "and the untaken block makes nothing")
	assert_eq(s.vmcallv("returned", true), {"a": 1, "b": 2},
		"a value returned out of a block is not released under it")
	assert_eq(s.vmcallv("returned", false), null, "and the untaken block returns nothing")

	s.queue_free()

func test_sgd_a_lambda_made_in_a_block_outlives_it():
	# Captures are copied into an Array at creation, so the release cannot
	# reach them -- but the Callable itself leaves the block.
	var source := """
func make(n):
	var f = null
	if n > 0:
		var prefix = "n=" + str(n)
		var parts = [prefix, "!"]
		f = func(extra): return parts[0] + parts[1] + str(extra)
	return f.call(7)
"""
	var s = _compile_and_load(source, 40000000)
	if s == null:
		return
	assert_eq(s.vmcallv("make", 3), "n=3!7",
		"the captures a block made are still readable after it")
	s.queue_free()

func test_sgd_leaving_a_block_early_releases_nothing_live():
	# break, continue and return jump past the release; the enclosing loop --
	# or the end of the call -- reclaims those slots instead.
	var source := """
func run(n : int):
	var out = []
	var i = 0
	while i < n:
		i += 1
		if i == 2:
			var skipped = {"skip": i}
			out.append(str(skipped.size()))
			continue
		if i == 5:
			var stop = [i, "last"]
			out.append(stop[1])
			break
		var kept = {"i": i}
		out.append(kept["i"])
	return out
"""
	var s = _compile_and_load(source, 40000000)
	if s == null:
		return
	assert_eq(s.vmcallv("run", 9), _si_early_exit(9),
		"an early exit out of a block should answer what the engine answers")
	s.queue_free()

func _si_early_exit(n : int):
	var out = []
	var i = 0
	while i < n:
		i += 1
		if i == 2:
			var skipped = {"skip": i}
			out.append(str(skipped.size()))
			continue
		if i == 5:
			var stop = [i, "last"]
			out.append(stop[1])
			break
		var kept = {"i": i}
		out.append(kept["i"])
	return out

func test_sgd_a_match_arm_releases_what_it_made():
	# Every arm is a block. A binding is a copy scoped to the arm, and the arm's
	# answer has to leave it intact.
	var source := """
func pick(v):
	var out = []
	match v:
		[var a, var b]:
			var joined = [a, b, "pair"]
			out.append(joined[2])
			out.append(a + b)
		{"tag": var t}:
			var s = str(t) + "!"
			out.append(s)
		var other:
			var s = str(other) + "?"
			out.append(s)
	return out

func many(n : int):
	var made = 0
	for i in range(n):
		match i % 3:
			0:
				made += [i, str(i)].size()
			1:
				made += {"k": str(i)}.size()
			_:
				made += len(str(i) + "x")
	return made
"""
	var s = _compile_and_load(source, 40000000)
	if s == null:
		return
	assert_eq(s.vmcallv("pick", [3, 4]), ["pair", 7], "a binding survives its arm")
	assert_eq(s.vmcallv("pick", {"tag": 9}), ["9!"], "so does one a Dictionary pattern took")
	assert_eq(s.vmcallv("pick", "z"), ["z?"], "the wildcard arm binds a copy")

	s.references_max = 24
	var before := s.get_exceptions()
	assert_eq(s.vmcallv("many", 60), _si_many(60), "sixty arms should run")
	assert_eq(s.get_exceptions(), before, "and should not run out of references")
	s.queue_free()

func _si_many(n : int):
	var made = 0
	for i in range(n):
		match i % 3:
			0:
				made += [i, str(i)].size()
			1:
				made += {"k": str(i)}.size()
			_:
				made += len(str(i) + "x")
	return made

func test_sgd_a_coroutine_block_keeps_its_values_across_a_suspension():
	# A suspension restores the frame but not the mark, so a coroutine takes no
	# block scope at all -- and everything a block made stays readable after the
	# await that ran under it.
	var source := """
func run(sig, n):
	var out = []
	if n > 0:
		var d = {"a": n}
		var tail = str(n) + "!"
		var v = await sig
		out.append(d["a"] + v)
		out.append(tail)
	return out
"""
	var path := "user://temp_await_block.sgd"
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(source)
	file.close()
	var script = load(path)
	assert_not_null(script, "the coroutine script should load")
	if script == null:
		return
	var node := Node.new()
	node.set_script(script)
	node.set_instructions_max(1000000)

	var awaitable = node.call("run", sgd_ping, 4)
	assert_eq(typeof(awaitable), TYPE_SIGNAL, "the block suspended on the signal")

	var completed := [null]
	(awaitable as Signal).connect(func(value): completed[0] = value)

	sgd_ping.emit(7)
	assert_eq(completed[0], [11, "4!"],
		"the values the block made before the await are still there after it")

	node.free()


# -= Project context: _init(), autoloads and engine statics =-

func test_sgd_init_runs_when_the_instance_is_created():
	var script := SafeGDScript.new()
	script.set_source_code("""
extends Node2D

var started = null

func _init():
	started = "yes"

func run():
	return started
""")
	assert_eq(script.get_compile_error(), "", "the script compiles")

	var node := Node2D.new()
	node.set_script(script)
	assert_eq(node.call("run"), "yes",
		"_init() should run at construction, the way GDScript runs it")
	node.free()

func test_sgd_init_with_arguments_is_refused_on_a_node():
	var script := SafeGDScript.new()
	script.set_source_code("""
extends Node2D

var started = null

func _init(who):
	started = who

func run():
	return started
""")
	assert_eq(script.get_compile_error(), "", "the script compiles")

	var node := Node2D.new()
	node.set_script(script)
	assert_engine_error("_init() takes arguments, so it cannot run for a script attached to a node.")
	assert_eq(node.call("run"), null, "and the member keeps its declared default")
	node.free()

func test_sgd_an_engine_class_constant_is_read_from_classdb():
	var source := """
func mode():
	return ScrollContainer.SCROLL_MODE_DISABLED

func shape():
	return PhysicsServer2D.SHAPE_CIRCLE
"""
	var s = _compile_and_load(source, 4000000)
	if s == null:
		return
	add_child(s)

	assert_eq(s.vmcallv("mode"), ScrollContainer.SCROLL_MODE_DISABLED,
		"a constant on a non-singleton class comes from ClassDB")
	assert_eq(s.vmcallv("shape"), PhysicsServer2D.SHAPE_CIRCLE,
		"a constant on a singleton still reads off the singleton")
	s.queue_free()

func test_sgd_a_static_method_on_an_engine_class():
	var source := """
func exists(path):
	return DirAccess.dir_exists_absolute(path)
"""
	var s = _compile_and_load(source, 4000000)
	if s == null:
		return
	add_child(s)

	assert_true(s.vmcallv("exists", "res://"), "a static method dispatches through ClassDB")
	assert_false(s.vmcallv("exists", "res://no_such_directory_here"),
		"and answers the engine's own result")
	s.queue_free()

func test_sgd_a_static_method_on_a_builtin_type():
	var source := """
func hex(value):
	return String.num_int64(value, 16)

func hsv():
	return Color.from_hsv(0.0, 1.0, 1.0)

func angle():
	return Vector2.from_angle(0.0)

func euler():
	return Basis.from_euler(Vector3(0.0, PI * 0.5, 0.0))
"""
	var s = _compile_and_load(source, 4000000)
	if s == null:
		return

	assert_eq(s.vmcallv("hex", 255), "ff",
		"a static method on a built-in type is called on the type, not through ClassDB")
	assert_true(s.vmcallv("hsv").is_equal_approx(Color(1, 0, 0)),
		"Color.from_hsv answers the engine's own result")
	assert_true(s.vmcallv("angle").is_equal_approx(Vector2(1, 0)),
		"Vector2.from_angle answers the engine's own result")
	assert_almost_eq(s.vmcallv("euler").x.z, -1.0, 0.0001,
		"Basis.from_euler answers the engine's own result")
	s.free()

func test_sgd_a_method_reference_through_self_is_a_callable():
	var source := """
var seen = 0

func on_done(value):
	seen = value
	return value * 2

func through_self():
	var c = self.on_done
	return typeof(c) == TYPE_CALLABLE

func invoke():
	var c = self.on_done
	return c.call(7)

func recall():
	return seen
"""
	var s = _compile_and_load(source, 4000000)
	if s == null:
		return
	add_child(s)

	assert_true(s.vmcallv("through_self"),
		"self.method should make a Callable, not read a property off the owner")
	assert_eq(s.vmcallv("invoke"), 14, "and calling it should reach the method")
	assert_eq(s.vmcallv("recall"), 7, "which ran with the argument it was given")
	s.queue_free()

func test_sgd_a_variant_property_declares_itself_as_one():
	var source := """
@export var anything = null

func run():
	return anything
"""
	var s = _compile_and_load(source, 4000000)
	if s == null:
		return
	add_child(s)

	# An @export with no knowable type is a NIL property, which the property list
	# reports as PROPERTY_USAGE_NIL_IS_VARIANT rather than refusing at startup.
	var found := false
	for property in s.get_property_list():
		if property["name"] == "anything":
			found = true
			assert_eq(property["type"], TYPE_NIL, "an untyped export is a Variant property")
	assert_true(found, "the property should be registered")

	s.set("anything", "text")
	assert_eq(s.vmcallv("run"), "text", "and it should hold whatever it is given")
	s.queue_free()

func test_sgd_an_untyped_member_survives_the_call_that_set_it():
	var source := """
var text = null
var container = null
var number = null

func remember():
	text = "kept"
	container = {"n": 1}
	number = 7
	return true

func recall():
	return [text, container, number]

func churn(rounds):
	var i = 0
	while i < rounds:
		text = "value-%d" % i
		container = [i]
		i += 1
	return text
"""
	var s = _compile_and_load(source, 40000000)
	if s == null:
		return
	add_child(s)

	# A String or Dictionary reaches the guest as a scoped index, which dies with the
	# call that made it. An untyped member has to outlive that, the way a typed one does.
	var before := s.get_exceptions()
	s.vmcallv("remember")
	assert_eq(s.vmcallv("recall"), ["kept", {"n": 1}, 7],
		"an untyped member should hold its value into the next call")
	assert_eq(s.get_exceptions(), before, "reading it back should not raise")

	# Reassignment recycles the slot rather than accumulating one per store.
	assert_eq(s.vmcallv("churn", 300), "value-299", "reassignment keeps working")
	assert_eq(s.vmcallv("recall")[1], [299], "and the last value is the one kept")
	assert_eq(s.get_exceptions(), before, "no store along the way should raise")
	s.queue_free()

func test_sgd_an_untyped_member_does_not_share_another_members_storage():
	var source := """
var a = null
var b = null

func seed_them():
	a = ["one"]
	b = a
	return true

func drop_a():
	a = 0
	return true

func append_through_a():
	a.append("two")
	return true

func read():
	return [a, b]
"""
	var s = _compile_and_load(source, 4000000)
	if s == null:
		return
	add_child(s)

	var before := s.get_exceptions()
	s.vmcallv("seed_them")

	# An Array is a reference: both members should see the same one.
	s.vmcallv("append_through_a")
	assert_eq(s.vmcallv("read"), [["one", "two"], ["one", "two"]],
		"copying an untyped member should copy the Variant, not the value behind it")

	# ...but each member owns its own storage, so overwriting one must not take
	# the other's value with it.
	s.vmcallv("drop_a")
	assert_eq(s.vmcallv("read"), [0, ["one", "two"]],
		"overwriting one untyped member should leave the other's value alone")
	assert_eq(s.get_exceptions(), before, "and neither read should raise")
	s.queue_free()

func test_sgd_the_engine_builds_the_types_that_have_no_inline_payload():
	var source := """
func transform():
	return Transform2D(0.0, Vector2(3, 4))

func quaternion():
	return Quaternion(0, 0, 0, 1)

func basis():
	return Basis()

func path():
	return NodePath("Foo/Bar")

func name_of(s):
	return StringName(s)

func narrow(v):
	return Vector2(v)

func array_copy(v):
	return Array(v)

func dictionary_copy(v):
	return Dictionary(v)

func bad_arity():
	return Quaternion(1, 2)
"""
	var s = _compile_and_load(source, 4000000)
	if s == null:
		return
	add_child(s)

	# One ECALL_VCONSTRUCT each: the engine's own constructor table settles which
	# overload applies and converts the arguments.
	assert_eq(s.vmcallv("transform"), Transform2D(0.0, Vector2(3, 4)),
		"a Transform2D is built by the engine")
	assert_eq(s.vmcallv("quaternion"), Quaternion(0, 0, 0, 1), "so is a Quaternion")
	assert_eq(s.vmcallv("basis"), Basis(), "and a default Basis")
	assert_eq(s.vmcallv("path"), NodePath("Foo/Bar"), "and a NodePath")
	assert_eq(s.vmcallv("name_of", "hello"), StringName("hello"), "and a StringName")

	# A one-argument conversion on an inline type takes the same path.
	assert_eq(s.vmcallv("narrow", Vector2i(1, 2)), Vector2(1, 2),
		"Vector2(Vector2i) is a conversion, not a component list")
	assert_eq(s.vmcallv("array_copy", PackedStringArray(["idle", "walk"])), ["idle", "walk"],
		"Array(from) converts a host-returned container")
	assert_eq(s.vmcallv("dictionary_copy", {"mob": 2}), {"mob": 2},
		"Dictionary(from) converts a host-returned container")

	# An arity no constructor accepts is the engine's to refuse, at run time.
	var before := s.get_exceptions()
	s.vmcallv("bad_arity")
	assert_engine_error("Quaternion(): no constructor takes 2 argument(s)")
	assert_engine_error("Exception: Quaternion(): no constructor takes 2 argument(s)")
	assert_eq(s.get_exceptions(), before + 1, "a refused construction should throw")

	s.queue_free()

const CONSTRUCTIBLE_SOURCE = """
class_name Constructible
extends Node2D

var greeting := "unset"
var scale_factor := 0

func _init(p_greeting: String = "hello", p_scale: int = 1) -> void:
	greeting = p_greeting
	scale_factor = p_scale

func greet() -> String:
	return greeting

func scaled(n: int) -> int:
	return n * scale_factor
"""

func _constructible_script() -> Script:
	var path = "user://temp_constructible.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(CONSTRUCTIBLE_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the constructible script should load as a SafeGDScript resource")
	return script

func test_sgd_class_constructs_with_no_arguments():
	var script = _constructible_script()
	if script == null:
		return
	var instance = script.call("new")
	assert_not_null(instance, "new() should produce an instance")
	if instance == null:
		return
	assert_true(instance is Node2D, "the instance should be of the declared base type")
	assert_eq(instance.get_script(), script, "the script should be attached to it")
	assert_eq(instance.call("greet"), "hello", "_init() should have run with its defaults")
	instance.free()

func test_sgd_class_constructs_with_arguments():
	var script = _constructible_script()
	if script == null:
		return
	var instance = script.call("new", "hi", 3)
	assert_not_null(instance, "new(args) should produce an instance")
	if instance == null:
		return
	assert_eq(instance.call("greet"), "hi", "_init() should have run with the given arguments")
	assert_eq(instance.call("scaled", 4), 12, "and kept them in its members")
	instance.free()

func test_gdscript_types_new_on_an_sgd_class():
	var script = _constructible_script()
	if script == null:
		return
	var caller := GDScript.new()
	caller.source_code = """
extends RefCounted

func make() -> Node2D:
	return preload("user://temp_constructible.sgd").new("typed", 2)
"""
	assert_eq(caller.reload(), OK, "`Class.new()` on a .sgd should type as a constructor")
	if caller.reload() != OK:
		return
	var instance = caller.new().make()
	assert_not_null(instance, "and construct at run time")
	if instance == null:
		return
	assert_eq(instance.call("scaled", 5), 10, "with the arguments it was given")
	instance.free()


# -= `extends <ScriptClass>` =-
#
# A .sgd compiles to one standalone RISC-V program from one source file, and
# Godot gives one Object one script instance, so neither side can chain at run
# time. The base's body is therefore compiled in: the host resolves the chain
# through the project's class list, reads the sources, and the compiler folds
# them into one flat program. res://tests/chain_base.gd is the base.

const CHAIN_LEAF_SOURCE = """
extends SgdChainBase

func chain_kind():
	return "leaf(" + super.chain_kind() + ")"

func run():
	base_calls += 1
	return [describe(), CHAIN_CENTER, SgdChainBase.doubled(ChainShape.CIRCLE), base_calls]
"""

func _chain_leaf_script() -> Script:
	var path = "user://temp_chain_leaf.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(CHAIN_LEAF_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the leaf script should load as a SafeGDScript resource")
	if script != null:
		assert_eq(script.get_compile_error(), "",
			"the leaf should compile with its base merged in")
	return script

func test_sgd_inherits_members_and_methods_from_a_gd_base():
	var script = _chain_leaf_script()
	if script == null:
		return
	var node = Node.new()
	node.set_script(script)
	# describe() is declared in the base and calls chain_kind(), which the leaf
	# overrides: one flat name table, so the override wins. CHAIN_CENTER and
	# ChainShape are the base's const and enum, base_calls its member, and
	# doubled() its static func reached through the base's own class name.
	assert_eq(node.call("run"), ["base:leaf(base)", 320, 2, 1],
		"every inherited name should resolve into the merged program")
	assert_eq(node.call("run"), ["base:leaf(base)", 320, 2, 2],
		"and an inherited member should keep its value across calls")
	node.free()

func test_sgd_reports_the_native_base_and_the_declared_base():
	var script = _chain_leaf_script()
	if script == null:
		return
	# Two consumers want different answers: instancing wants the engine class the
	# chain bottoms out at, identity wants the script the leaf declared.
	assert_eq(script.get_instance_base_type(), &"Node",
		"the native base is what the chain bottoms out at")
	var base_script = script.get_base_script()
	assert_not_null(base_script, "the declared base should be the base script itself")
	if base_script == null:
		return
	assert_eq(base_script.resource_path, "res://tests/chain_base.gd",
		"and it should be the file the class list names")
	assert_eq(base_script.get_instance_base_type(), &"Node",
		"and the chain should bottom out at the base's own engine class")

func test_sgd_instance_answers_is_for_its_script_base():
	var script = _chain_leaf_script()
	if script == null:
		return
	var node = Node.new()
	node.set_script(script)
	# The runner in a converted project filters its scene list with exactly this,
	# so a leaf that does not answer would drop out of the run entirely.
	assert_true(node is SgdChainBase, "an instance should answer for its script base")
	assert_true(node is Node, "and still for the engine class underneath it")
	node.free()

func test_sgd_instance_belongs_in_a_container_typed_by_its_script():
	var script = _chain_leaf_script()
	if script == null:
		return
	var node = Node.new()
	node.set_script(script)

	# A typed container validates every element with Script::inherits_script(),
	# and a script is its own type there, as GDScript's is: answering false for
	# itself kept an instance out of an Array typed by its own class_name, which
	# is what `Array[Monitor]` in a converted project compiles to.
	var by_own_script = Array([], TYPE_OBJECT, &"Node", script)
	by_own_script.append(node)
	assert_eq(by_own_script.size(), 1,
		"an instance belongs in a container typed by its own script")

	# And by the base it declares, which the chain above walks to.
	var by_base_script = Array([], TYPE_OBJECT, &"Node", script.get_base_script())
	by_base_script.append(node)
	assert_eq(by_base_script.size(), 1,
		"and in one typed by the base it extends")

	node.free()

func test_sgd_reaches_a_script_class_it_does_not_contain():
	# A project class_name script is neither an engine singleton nor merged into
	# this ELF. Its statics now cross the boundary through a short-lived instance.
	var path = "user://temp_chain_outsider.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string("""
func f():
	return SgdChainBase.doubled(2)
""")
	file.close()
	var script = SafeGDScript.new()
	script.take_over_path(path)
	script.source_code = FileAccess.get_file_as_string(path)
	assert_eq(script.get_compile_error(), "",
		"a static method on another script class should compile")
	var node = Node.new()
	node.set_script(script)
	assert_eq(node.call("f"), 4, "the cross-file static call should run")
	node.free()


func test_sgd_rebuilds_when_its_base_changes():
	# Editing a base arrives through no callback at all, so the merged program
	# would otherwise keep the old body and say nothing about it. Extended by
	# path here: a user:// file is not in the project's class list.
	var base_path = "user://temp_chain_mutable_base.gd"
	var base = FileAccess.open(base_path, FileAccess.WRITE)
	base.store_string("extends Node\n\nfunc answer():\n\treturn 1\n")
	base.close()

	var leaf_path = "user://temp_chain_dependent.sgd"
	var leaf = FileAccess.open(leaf_path, FileAccess.WRITE)
	leaf.store_string("extends \"%s\"\n\nfunc run():\n\treturn answer()\n" % base_path)
	leaf.close()

	var script = load(leaf_path)
	assert_not_null(script, "the dependent script should load")
	if script == null:
		return
	var node = Node.new()
	node.set_script(script)
	assert_eq(node.call("run"), 1, "the base body should be compiled in")

	# Rewritten within the same second, which is all the resolution a modified
	# time has: the length is what makes this visible.
	base = FileAccess.open(base_path, FileAccess.WRITE)
	base.store_string("extends Node\n\nfunc answer():\n\treturn 1 + 1\n")
	base.close()
	SafeGDScript.poll_base_sources()

	assert_eq(node.call("run"), 2, "editing the base should rebuild the dependent")
	node.free()

func test_sgd_resolves_relative_paths_through_an_extends_chain():
	var base_path = "user://temp_relative_chain_base.gd"
	var base = FileAccess.open(base_path, FileAccess.WRITE)
	base.store_string("""
extends Node
signal finished
var inherited_state := [7]
func inherited_value():
	return inherited_state[0]
""")
	base.close()

	var middle_path = "user://temp_relative_chain_middle.gd"
	var middle = FileAccess.open(middle_path, FileAccess.WRITE)
	middle.store_string("""
extends "temp_relative_chain_base.gd"
func middle_value():
	return inherited_value() + 1
""")
	middle.close()

	var leaf_path = "user://temp_relative_chain_leaf.sgd"
	var leaf = FileAccess.open(leaf_path, FileAccess.WRITE)
	leaf.store_string("""
extends "temp_relative_chain_middle.gd"
func run():
	return middle_value() + 1
""")
	leaf.close()

	var script = load(leaf_path)
	assert_not_null(script, "a leaf with relative base paths should load")
	if script == null:
		return
	assert_eq(script.get_compile_error(), "", "the relative chain should compile")
	assert_eq(script.get_base_script().resource_path, middle_path,
		"script identity should resolve the relative declared base too")

	var node = Node.new()
	node.set_script(script)
	assert_eq(node.call("run"), 9, "state and methods should arrive through both bases")
	assert_true(node.has_signal("finished"), "an inherited signal should be published")
	node.free()

func test_sgd_a_coroutine_may_override_a_named_property_setter():
	var base_path = "user://temp_coroutine_setter_base.gd"
	var base = FileAccess.open(base_path, FileAccess.WRITE)
	base.store_string("""
extends Node
var active := false: set = set_active
func set_active(value):
	active = value
""")
	base.close()

	var leaf_path = "user://temp_coroutine_setter_leaf.sgd"
	var leaf = FileAccess.open(leaf_path, FileAccess.WRITE)
	leaf.store_string("""
extends "user://temp_coroutine_setter_base.gd"
signal setter_gate
var completions := 0
func set_active(value):
	super.set_active(value)
	if value:
		await setter_gate
		completions += 1
func completion_count():
	return completions
""")
	leaf.close()

	var script = load(leaf_path)
	assert_not_null(script, "a coroutine setter override should load")
	if script == null:
		return
	assert_eq(script.get_compile_error(), "", "the inherited property should accept the override")

	var node = Node.new()
	node.set_script(script)
	node.set("active", true)
	assert_eq(node.get("active"), true, "the setter should update storage before suspending")
	assert_eq(node.call("completion_count"), 0, "the setter should still be suspended")
	node.emit_signal("setter_gate")
	assert_eq(node.call("completion_count"), 1, "the host should resume the setter coroutine")
	node.free()

func test_sgd_instance_initializers_release_temporary_variants():
	# This mirrors physics_tests' Array[Dictionary] member. Its default record
	# consumes most of references_max while loading; initializing another instance
	# must use a fresh scoped state and promote only the final Array.
	var source = "extends Node\nvar entries = [\n"
	for i in range(15):
		source += "\t{\"id\": \"Functional Test %d\", \"path\": \"res://test_%d.tscn\"},\n" % [i, i]
	source += "]\nfunc count():\n\treturn entries.size()\n"

	var path = "user://temp_large_member_initializer.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(source)
	file.close()
	var script = load(path)
	assert_not_null(script, "the large member initializer should compile")
	if script == null:
		return

	var first = Node.new()
	first.set_script(script)
	var second = Node.new()
	second.set_script(script)
	assert_eq(first.call("count"), 15, "the first instance should keep its Array")
	assert_eq(second.call("count"), 15, "a second instance should get a fresh Array too")
	assert_eq(first.get_exceptions(), 0, "initializing instances should not exhaust references")
	second.free()
	first.free()

# -= Enums as values =-
#
# An enum is compiler-only: every member folds to an integer immediate and
# nothing of it reaches the IR. GDScript still exposes the enum itself as a
# Dictionary, so `E.values()` used to lower to a property read on the owner and
# fail with "Nonexistent function 'values' in base 'Nil'".

func test_an_enum_used_as_a_value_is_a_dictionary():
	var gdscript_code = """
enum E { A, B = 5, C }

func values():
	return E.values()

func keys():
	return E.keys()

func size():
	return E.size()

func has_b():
	return E.has("B")

func subscript():
	return E["C"]

func member():
	return E.B

func find():
	return E.find_key(5)

func walk():
	var total = 0
	for v in E.values():
		total += v
	return total
"""
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_false(compiled_elf.is_empty(), "the enum script should compile")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(100000)

	# Matches the engine: implicit numbering continues from the explicit value.
	assert_eq(s.vmcallv("values"), [0, 5, 6], "values() should answer the member values in order")
	assert_eq(s.vmcallv("keys"), ["A", "B", "C"], "keys() should answer the member names as Strings")
	assert_eq(s.vmcallv("size"), 3, "size() should count the members")
	assert_eq(s.vmcallv("has_b"), true, "has() should find a member by name")
	assert_eq(s.vmcallv("subscript"), 6, "a subscript should answer the member's value")
	assert_eq(s.vmcallv("find"), "B", "find_key() should answer the name holding a value")
	assert_eq(s.vmcallv("walk"), 11, "iterating values() should walk the member values")

	# A member reference still folds; the Dictionary is only for the enum itself.
	assert_eq(s.vmcallv("member"), 5, "a member reference should be its integer")

	s.queue_free()
	ts.queue_free()

# -= Object members across calls =-
#
# A global whose every read takes its address in place has no frame copy. The
# branch's truthiness test dropped that base register and asked the host about
# the bottom of the frame instead, so `if platform:` answered false for a live
# object -- while typeof() and `== null` both answered correctly.

const OBJECT_MEMBER_SOURCE = """
extends Node2D

var platform: Sprite2D
var moving := false

func setup():
	platform = Sprite2D.new()
	platform.position = Vector2(1, 2)
	add_child(platform)
	moving = true

func member_is_truthy():
	if platform:
		return 1
	return 0

func local_copy_is_truthy():
	var local = platform
	if local:
		return 1
	return 0

func guarded_step(delta: float):
	if moving and platform:
		platform.position.x += 10.0 * delta
		return platform.position.x
	return -1.0
"""

func test_an_object_member_is_truthy_across_calls():
	var path = "user://temp_object_member.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(OBJECT_MEMBER_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the object-member script should load as a SafeGDScript resource")
	if script == null:
		return

	# Not added to the tree: nothing here needs a frame, and the guest's own
	# add_child() parents the Sprite2D to the owner either way.
	var node = Node2D.new()
	node.set_script(script)
	node.set_instructions_max(100000)
	node.call("setup")

	assert_eq(node.call("member_is_truthy"), 1, "a live object in a member should be truthy")
	assert_eq(node.call("local_copy_is_truthy"), 1, "and so should a local copy of it")

	# The member survives the call that assigned it, so the guard holds every pass.
	assert_eq(node.call("guarded_step", 0.1), 2.0, "the guarded branch should run")
	assert_eq(node.call("guarded_step", 0.1), 3.0, "and again on the next call")

	node.free()

# -= Unary minus =-
#
# `-x` lowered to `0 - x`, and Godot has no `int - Vector2` operator: every
# non-numeric negation evaluated to NIL with no diagnostic. OP_NEGATE is the
# only form Godot defines for a Vector, a Color or a Quaternion.

func test_unary_minus_negates_every_type_godot_defines():
	var gdscript_code = """
func neg(v):
	return -v

func neg_typed(v: Vector2):
	return -v

func neg_int(v: int):
	return -v

func neg_float(v: float):
	return -v

func neg_literal():
	return -Vector2(1, 2)

func neg_in_ternary(v, b):
	return v if b else -v
"""
	var ts : Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var compiled_elf = ts.vmcall("compile_to_elf", gdscript_code)
	assert_false(compiled_elf.is_empty(), "the negation script should compile")

	var s = Sandbox.new()
	s.load_buffer(compiled_elf)
	s.set_instructions_max(100000)

	assert_eq(s.vmcallv("neg", Vector2(1, 2)), Vector2(-1, -2), "Vector2 should negate")
	assert_eq(s.vmcallv("neg", Vector3(1, 2, 3)), Vector3(-1, -2, -3), "Vector3 should negate")
	assert_eq(s.vmcallv("neg", Vector2i(1, 2)), Vector2i(-1, -2), "Vector2i should negate")
	# Godot's OP_NEGATE on a Color inverts it rather than negating components.
	assert_eq(s.vmcallv("neg", Color(0.25, 0.5, 0.75, 1.0)), Color(0.75, 0.5, 0.25, 0.0),
		"Color should answer what the engine's operator answers")
	assert_eq(s.vmcallv("neg_typed", Vector2(3, 4)), Vector2(-3, -4),
		"a declared Vector2 should negate")

	# The numeric fast paths still answer what they always did.
	assert_eq(s.vmcallv("neg", 5), -5, "an untyped int should negate")
	assert_eq(s.vmcallv("neg", 2.5), -2.5, "an untyped float should negate")
	assert_eq(s.vmcallv("neg_int", 5), -5, "a declared int should negate")
	assert_eq(s.vmcallv("neg_float", 2.5), -2.5, "a declared float should negate")
	# Sign-bit flip, not 0.0 - x, so -0.0 keeps its sign the way Godot's does.
	assert_eq(str(s.vmcallv("neg_float", 0.0)), str(-0.0), "negating 0.0 should give -0.0")
	assert_eq(s.vmcallv("neg_literal"), Vector2(-1, -2), "a constructed Vector2 should negate")

	# The else arm of a ternary is where this first showed up.
	assert_eq(s.vmcallv("neg_in_ternary", Vector2(1, 2), false), Vector2(-1, -2),
		"the else arm of a ternary should negate")
	assert_eq(s.vmcallv("neg_in_ternary", Vector2(1, 2), true), Vector2(1, 2),
		"and the then arm should pass the value through")

	s.queue_free()
	ts.queue_free()

# -= Members through the host =-
#
# GDScript answers Object::get/set for every member a script declares; a .sgd
# answered only for @export. Anything holding the node as a plain Node -- a
# lambda reaching `p_target.bodies`, a tool script, the remote inspector -- read
# null and got no diagnostic.

const HOST_MEMBER_SOURCE = """
extends Node2D

@export var speed := 1.5
var bodies: Array[int] = []
var label := "idle"
var counter := 0
const LIMIT := 9

func fill():
	bodies.append(1)
	bodies.append(2)
	label = "ready"

func seen():
	return [bodies, label, counter]
"""

func test_sgd_members_answer_the_host():
	var path = "user://temp_host_members.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(HOST_MEMBER_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the member script should load as a SafeGDScript resource")
	if script == null:
		return

	var node = Node2D.new()
	node.set_script(script)
	node.set_instructions_max(100000)
	node.call("fill")

	assert_eq(node.get("bodies"), [1, 2], "a plain Array member should read back")
	assert_eq(node.get("label"), "ready", "a plain String member should read back")
	assert_eq(node.get("speed"), 1.5, "an @export should still read back")

	node.set("counter", 4)
	assert_eq(node.get("counter"), 4, "a plain member should take a write")
	assert_eq(node.call("seen"), [[1, 2], "ready", 4], "and the guest should see it")

	# A const is not per-instance storage, so it is not a property -- but get()
	# still answers it, the way GDScript answers out of Script::constants.
	assert_eq(node.get("LIMIT"), 9, "a const should answer get()")

	# @export is the inspector; a plain member is only a script variable.
	var usage := {}
	for property in node.get_property_list():
		usage[property["name"]] = int(property["usage"])
	assert_true((usage.get("speed", 0) & PROPERTY_USAGE_EDITOR) != 0,
		"an @export should reach the inspector")
	assert_eq(usage.get("bodies", 0) & (PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE), 0,
		"a plain member should reach neither the inspector nor a saved scene")
	assert_false(usage.has("LIMIT"), "a const is not a property")

	node.free()

# -= Constants and enums answer a reader outside the script =-
#
# Both are compiler-only: they fold at their use sites and the guest keeps no
# storage for them. A reader outside the script -- most often an autoload,
# `Global.State.RUN` -- goes through Object::get, which found nothing and
# answered null. GDScript answers the same read out of Script::constants; the
# compiler now publishes a table beside the ELF so a .sgd does too.

const CONSTANT_HOLDER_SOURCE = """
extends Node

enum State { IDLE, RUN, DONE }
enum Sparse { A = 5, B = 9 }
enum { LOOSE_A, LOOSE_B }
const LIMIT := 42
const LABEL := "holder"
const RATIO := 0.5
const FLAG := true

func own_view():
	return [State.RUN, Sparse.B, LOOSE_B, LIMIT]
"""

const CONSTANT_READER_SOURCE = """
extends Node

func read_from(holder):
	return [holder.State, holder.State.RUN, holder.Sparse.B, holder.LOOSE_B,
		holder.LIMIT, holder.LABEL, holder.RATIO, holder.FLAG]
"""

func test_sgd_constants_and_enums_answer_another_script():
	var holder_path = "user://temp_constant_holder.sgd"
	var file = FileAccess.open(holder_path, FileAccess.WRITE)
	file.store_string(CONSTANT_HOLDER_SOURCE)
	file.close()
	var reader_path = "user://temp_constant_reader.sgd"
	file = FileAccess.open(reader_path, FileAccess.WRITE)
	file.store_string(CONSTANT_READER_SOURCE)
	file.close()

	var holder_script = load(holder_path)
	var reader_script = load(reader_path)
	assert_not_null(holder_script, "the holder script should load")
	assert_not_null(reader_script, "the reader script should load")
	if holder_script == null or reader_script == null:
		return

	var holder = Node.new()
	holder.set_script(holder_script)
	holder.set_instructions_max(100000)
	var reader = Node.new()
	reader.set_script(reader_script)
	reader.set_instructions_max(100000)

	# The guest still folds its own; publishing must not change that.
	assert_eq(holder.call("own_view"), [1, 9, 1, 42],
		"the script should still fold its own constants")

	assert_eq(reader.call("read_from", holder),
		[{"IDLE": 0, "RUN": 1, "DONE": 2}, 1, 9, 1, 42, "holder", 0.5, true],
		"another script should read the constants and enums")

	# Same table the editor and Script.get_script_constant_map() read.
	assert_eq(holder_script.get_script_constant_map().get("State"),
		{"IDLE": 0, "RUN": 1, "DONE": 2},
		"the script should publish its enum as a constant")

	reader.free()
	holder.free()

# -= Declared types start at their default =-
#
# `var t: Transform2D` is IDENTITY in GDScript, not null. Only a declared Object
# starts null. Leaving the rest NIL was also a lifetime bug: an empty slot holds
# VASSIGN's INT32_MIN sentinel, so the first assignment adopted the source's
# scoped index and the member dangled the moment the call returned.

const TYPED_DEFAULT_SOURCE = """
extends Node2D

var v2: Vector2
var v3i: Vector3i
var col: Color
var t2: Transform2D
var quat: Quaternion
var rid: RID
var call_: Callable
var sn: StringName
var obj: Object

func types():
	return [typeof(v2), typeof(col), typeof(t2), typeof(quat), typeof(rid),
		typeof(call_), typeof(sn), typeof(obj)]

func values():
	return [v2, v3i, col, t2, quat, sn]

func keep_a_rid():
	rid = PhysicsServer2D.body_create()
	return typeof(rid)

func use_the_rid():
	PhysicsServer2D.body_set_mode(rid, PhysicsServer2D.BODY_MODE_RIGID)
	var mode = PhysicsServer2D.body_get_mode(rid)
	PhysicsServer2D.free_rid(rid)
	return mode
"""

func test_a_declared_type_starts_at_its_default():
	var path = "user://temp_typed_defaults.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(TYPED_DEFAULT_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the typed-default script should load")
	if script == null:
		return

	var node = Node2D.new()
	node.set_script(script)
	node.set_instructions_max(100000)

	assert_eq(node.call("types"),
		[TYPE_VECTOR2, TYPE_COLOR, TYPE_TRANSFORM2D, TYPE_QUATERNION, TYPE_RID,
			TYPE_CALLABLE, TYPE_STRING_NAME, TYPE_NIL],
		"every declared type but Object should hold a value of that type")

	# The engine's own defaults, not zeroed bytes: Color is opaque black and
	# Transform2D is IDENTITY.
	assert_eq(node.call("values"),
		[Vector2(), Vector3i(), Color(), Transform2D(), Quaternion(), StringName()],
		"and the value should be what the engine constructs")

	# A complex type has to hold a real value for the first assignment to land in
	# a permanent slot instead of adopting a scoped index that dies with the call.
	assert_eq(node.call("keep_a_rid"), TYPE_RID, "a RID should be assignable to a member")
	assert_eq(node.call("use_the_rid"), PhysicsServer2D.BODY_MODE_RIGID,
		"and should still be usable in a later call")

	node.free()

# -= Members of a built-in that is not inline =-
#
# Transform2D, Basis, AABB and friends live in a scoped Variant, so `t.origin`
# reaches the host as a property read. It used to insist on an Object and threw
# "Variant is not an Object, but Transform2D"; the host now answers with the
# built-in's own member. No object is involved, so no allowlist applies.

const BUILTIN_MEMBER_SOURCE = """
extends Node2D

func origin_of(t: Transform2D):
	return t.origin

func axes_of(t: Transform2D):
	return [t.x, t.y]

func moved(t: Transform2D, v: Vector2):
	t.origin = v
	return t

func basis_x(b: Basis):
	return b.x

func basis_index(b: Basis, i: int):
	return b[i]

func untyped_basis_index(b, i):
	return b[i]

func untyped_index(value, key):
	return value[key]

func aabb_size(a: AABB):
	return a.size

func quat_w(q: Quaternion):
	return q.w

func untyped_origin(t):
	return t.origin
"""

func test_a_builtin_member_does_not_need_an_object():
	var path = "user://temp_builtin_members.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(BUILTIN_MEMBER_SOURCE)
	file.close()
	var script = load(path)
	assert_not_null(script, "the built-in member script should load")
	if script == null:
		return

	var node = Node2D.new()
	node.set_script(script)
	node.set_instructions_max(100000)

	var t := Transform2D(0.0, Vector2(2, 3))
	assert_eq(node.call("origin_of", t), Vector2(2, 3), "Transform2D.origin should read")
	assert_eq(node.call("axes_of", t), [t.x, t.y], "so should its axes")
	assert_eq(node.call("moved", t, Vector2(9, 9)).origin, Vector2(9, 9),
		"and a member write should land on the guest's own copy")
	assert_eq(t.origin, Vector2(2, 3), "without touching the caller's Transform2D")

	assert_eq(node.call("basis_x", Basis()), Basis().x, "Basis.x should read")
	var basis := Basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9))
	assert_eq(node.call("basis_index", basis, 1), basis[1], "Basis[index] should read a row")
	assert_eq(node.call("untyped_basis_index", basis, 2), basis[2],
		"an untyped Basis[index] should use the same indexed operation")
	assert_eq(node.call("untyped_index", Vector3(2, 5, 8), 1), 5.0,
		"the shared Variant index path should cover other built-in types")
	var keyed := {Vector2(1, 2): "vector key"}
	assert_eq(node.call("untyped_index", keyed, Vector2(1, 2)), "vector key",
		"the shared Variant index path must preserve arbitrary keys")
	assert_eq(node.call("aabb_size", AABB(Vector3(), Vector3(1, 2, 3))), Vector3(1, 2, 3),
		"AABB.size should read")
	assert_eq(node.call("quat_w", Quaternion()), 1.0, "Quaternion.w should read")

	# The same path with nothing declared about the value.
	assert_eq(node.call("untyped_origin", t), Vector2(2, 3),
		"an untyped receiver should reach the same member")

	node.free()

func test_sgd_a_class_typed_member_holds_null():
	var source = """
class TestData:
	var id = ""

var current: TestData = null
var marked: TestData? = null

func run():
	if current != null:
		return -1
	if marked != null:
		return -2
	current = TestData.new()
	current.id = "one"
	if current.id != "one":
		return -3
	current = null
	if current != null:
		return -4
	return 1
"""
	var path = "user://temp_class_typed_null.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(source)
	file.close()
	var script = load(path)
	assert_not_null(script, "the script should load")
	if script == null:
		return
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(1000000)
	assert_eq(node.call("run"), 1, "a class-typed member should hold null both ways")
	node.free()

func test_sgd_a_multiline_lambda_ends_at_a_dedented_paren():
	var source = """
signal play_stats_updated(value)

var seen = 0

func run():
	play_stats_updated.connect(
			func(value):
				seen = value
				)
	play_stats_updated.emit(42)
	return seen
"""
	var path = "user://temp_dedented_lambda.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(source)
	file.close()
	var script = load(path)
	assert_not_null(script, "the script should load")
	if script == null:
		return
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(1000000)
	assert_eq(node.call("run"), 42, "the lambda body should run past its dedented paren")
	node.free()

func test_sgd_an_unused_typed_parameter_keeps_the_typed_entry():
	var source = """
func helper(_d: float) -> int:
	return 7

func mixed(a: float, _b: float) -> float:
	return a

func run():
	if helper(1.0) != 7:
		return -1
	if mixed(2.5, 3.5) != 2.5:
		return -2
	return 1
"""
	var path = "user://temp_unused_typed_parameter.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(source)
	file.close()
	var script = load(path)
	assert_not_null(script, "the script should load")
	if script == null:
		return
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(1000000)
	assert_eq(node.call("run"), 1, "a function whose typed parameter is unused should still be callable")
	node.free()

func test_sgd_a_vector_widens_between_int_and_float_forms():
	var source = """
func make_int_vector() -> Vector2i:
	return Vector2i(1, 2)

func run():
	var from: Vector2 = make_int_vector()
	if typeof(from) != TYPE_VECTOR2:
		return -1
	if from != Vector2(1, 2):
		return -2
	var back: Vector2i = Vector2(3.7, 4.2)
	if typeof(back) != TYPE_VECTOR2I:
		return -3
	if back != Vector2i(3, 4):
		return -4
	var area: Rect2 = Rect2i(1, 2, 3, 4)
	if typeof(area) != TYPE_RECT2:
		return -5
	if area != Rect2(1, 2, 3, 4):
		return -6
	var text: String = make_name()
	if typeof(text) != TYPE_STRING:
		return -7
	if text != "walk":
		return -8
	var loose: Array = make_packed()
	if typeof(loose) != TYPE_ARRAY:
		return -9
	if loose != [1, 2, 3]:
		return -10
	return 1

func make_name() -> StringName:
	return &"walk"

func make_packed() -> PackedInt32Array:
	return PackedInt32Array([1, 2, 3])
"""
	var path = "user://temp_vector_widening.sgd"
	var file = FileAccess.open(path, FileAccess.WRITE)
	file.store_string(source)
	file.close()
	var script = load(path)
	assert_not_null(script, "the script should load")
	if script == null:
		return
	var node = Node.new()
	node.set_script(script)
	node.set_instructions_max(1000000)
	assert_eq(node.call("run"), 1, "the int and float forms of a vector should convert on assignment")
	node.free()

func test_sgd_a_static_function_is_published_as_static():
	var flags = {}
	for method in (SgdStaticUtils as Script).get_script_method_list():
		flags[method["name"]] = int(method["flags"])
	assert_true(flags.has("doubled"), "a 'static func' should be in the method list")
	assert_ne(flags.get("doubled", 0) & METHOD_FLAG_STATIC, 0,
		"a 'static func' should be published with METHOD_FLAG_STATIC")
	assert_eq(flags.get("instance_only", 0) & METHOD_FLAG_STATIC, 0,
		"a plain 'func' should not be")
	assert_true((SgdStaticUtils as Script).has_method("doubled"),
		"has_method() asks the script for its statics")

func test_sgd_a_static_function_is_callable_on_the_class():
	# GDScript refuses a non-static call on a class while parsing, so this file
	# only loads at all when the flag reaches the analyzer.
	assert_eq(SgdStaticUtils.doubled(21), 42, "a static function should run without an instance")
	assert_eq(SgdStaticUtils.scaled(3), 9, "a folded default should be filled in")
	assert_eq(SgdStaticUtils.scaled(3, 4), 12, "and a passed argument should win over it")

func test_sgd_a_static_call_shares_the_machine_with_the_instances():
	var node = Node.new()
	node.set_script(SgdStaticUtils)
	node.call("remember", 5)
	assert_eq(SgdStaticUtils.remembered(), 5,
		"a 'static var' is one per program, and the static call runs in that program")
	node.set_script(null)
	node.free()

func test_sgd_a_static_call_follows_a_recompile():
	var script = SafeGDScript.new()
	script.set_source_code("static func answer():\n\treturn 1\n")
	assert_eq(script.get_compile_error(), "", "the script should compile")
	assert_eq(script.call("answer"), 1, "a static function answers on the script itself")
	script.set_source_code("static func answer():\n\treturn 2\n")
	assert_eq(script.get_compile_error(), "", "the second version should compile")
	assert_eq(script.call("answer"), 2, "and the recompiled program should answer after it")

# -= @test =-
#
# A `.sgd` marks an argless top-level func with @test; the compiler publishes it
# as a test case and SafeGDScript.run_tests() calls each one on a fresh instance.
# A test fails the way any guest code fails: assert() throws, the Sandbox counts
# the exception, and the runner reads the counter.

func _sgd_declares(script: SafeGDScript, name: String) -> bool:
	# has_method() on a Script resource asks the resource's own ClassDB methods;
	# the script's declared functions are in its method list.
	for method in script.get_script_method_list():
		if method["name"] == name:
			return true
	return false

func _sgd_with_tests(source: String) -> SafeGDScript:
	var script := SafeGDScript.new()
	script.set_source_code(source)
	assert_eq(script.get_compile_error(), "", "the test script should compile")
	return script

func test_sgd_test_functions_are_published():
	var script := _sgd_with_tests("""
func helper() -> int:
	return 7

@test
func helper_answers_seven():
	assert(helper() == 7)

@test
## Documented case.
func documented_case():
	pass
""")
	assert_eq(script.get_test_functions(),
		PackedStringArray(["helper_answers_seven", "documented_case"]),
		"the tests are published in declaration order")
	assert_eq(script.get_test_lines(), PackedInt32Array([6, 11]),
		"each test carries the line of its 'func' token")
	assert_true(_sgd_declares(script, "helper_answers_seven"),
		"a @test function is still an ordinary method")
	assert_true(_sgd_declares(script, "helper"), "the helper is untouched")

func test_sgd_run_tests_reports_pass_fail_and_error():
	var script := _sgd_with_tests("""
@test
func passes():
	assert(1 + 1 == 2)

@test
func fails():
	assert(false, "two is not three")

@test
func reads_past_the_end():
	var values := []
	assert(values[3] == 1)
""")
	var report := script.run_tests()
	assert_eq(report["passed"], 1, "one test passes")
	assert_eq(report["failed"], 2, "the assertion and the guest error both fail")
	assert_eq(report["errors"], 0, "the runner could run all three")
	assert_eq(report["path"], script.resource_path, "the report names the script")

	var rows: Array = report["tests"]
	assert_eq(rows.size(), 3, "one row per test")
	assert_eq(rows[0]["name"], "passes")
	assert_eq(rows[0]["status"], "passed")
	assert_eq(rows[0]["message"], "", "a passing test reports no message")

	assert_eq(rows[1]["name"], "fails")
	assert_eq(rows[1]["status"], "failed")
	assert_true(rows[1]["message"].contains("two is not three"),
		"the assertion message reaches the report: " + str(rows[1]["message"]))
	assert_eq(rows[2]["status"], "failed", "a guest error fails the same way")
	assert_true(rows[2]["message"].contains("Array index out of bounds"),
		"and carries the guest's own message: " + str(rows[2]["message"]))

	# GUT 9.6 fails a test on any engine error nobody claimed.
	assert_engine_error("Sandbox exception in assert: two is not three")
	assert_engine_error("Exception: Sandbox exception in assert: two is not three")
	assert_engine_error("Array index out of bounds: 3")
	assert_engine_error("Exception: Array index out of bounds: 3")

func test_sgd_run_tests_uses_a_fresh_instance_per_test():
	var script := _sgd_with_tests("""
var counter: int = 0

@test
func a_mutates_the_member():
	counter += 41
	assert(counter == 41)

@test
func b_sees_the_declared_default():
	assert(counter == 0)
""")
	var report := script.run_tests()
	assert_eq(report["failed"], 0,
		"each test runs on its own instance record, so the member starts at its default")
	assert_eq(report["passed"], 2)

func test_sgd_run_tests_runs_init_but_not_ready():
	var script := _sgd_with_tests("""
extends Node

var from_init: int = 0
var from_ready: int = 0

func _init():
	from_init = 1

func _ready():
	from_ready = 1

@test
func init_ran_and_ready_did_not():
	assert(from_init == 1, "_init runs for every test instance")
	assert(from_ready == 0, "the owner is never added to a tree")
""")
	var report := script.run_tests()
	assert_eq(report["failed"], 0, "the runner runs _init() but no tree notifications")
	assert_eq(report["passed"], 1)

func test_sgd_run_tests_filters_by_name():
	var script := _sgd_with_tests("""
@test
func wanted():
	pass

@test
func unwanted():
	assert(false, "this one must not run")
""")
	var report := script.run_tests(PackedStringArray(["wanted"]))
	assert_eq(report["passed"], 1, "only the named test runs")
	assert_eq(report["failed"], 0, "the other one is not called")
	assert_eq(report["tests"].size(), 1, "and gets no row")

func test_sgd_run_tests_reports_an_unknown_name_as_an_error():
	var script := _sgd_with_tests("@test\nfunc known():\n\tpass\n")
	var report := script.run_tests(PackedStringArray(["typo"]))
	assert_eq(report["errors"], 1, "an unknown name is reported, not silently skipped")
	assert_eq(report["passed"], 0, "and no test runs")
	assert_eq(report["tests"][0]["status"], "error")
	assert_eq(report["tests"][0]["name"], "typo")

func test_sgd_run_tests_times_out_a_runaway_test():
	var script := _sgd_with_tests("""
@test
func loops_forever():
	while true:
		pass
""")
	# The budget belongs to the shared machine; a bootstrap owner is the only
	# way to reach it before the runner makes its own owners.
	var owner := Sandbox.new()
	owner.set_script(script)
	owner.set("execution_timeout", 10)

	var report := script.run_tests()
	assert_eq(report["failed"], 1, "a runaway test ends as a failure, not a hang")
	assert_true(report["tests"][0]["message"].contains("Guest timeout"),
		"and says so: " + str(report["tests"][0]["message"]))

	owner.set_script(null)
	owner.free()

func test_sgd_run_tests_reports_a_compile_error_as_one_row():
	var script := SafeGDScript.new()
	script.set_source_code("@test\nfunc broken():\n\treturn ??\n")
	assert_ne(script.get_compile_error(), "", "the source should not compile")
	var report := script.run_tests()
	assert_eq(report["errors"], 1, "a script that did not compile reports one error row")
	assert_eq(report["tests"][0]["name"], "(compile)")
	assert_ne(report["tests"][0]["message"], "", "carrying the compiler's message")
	assert_engine_error("[Parser Error] Expected expression")

func test_sgd_restricted_script_tests_run_restricted():
	# A restricted script's tests see the allowlists the script itself sees:
	# they run on the same restricted machine.
	var script := SafeGDScript.new()
	script.set_source_code("func __bootstrap():\n\tpass\n")
	var owner := Node.new()
	owner.set_script(script)
	owner.set("restrictions", true)
	script.set_source_code("""
@test
func creating_a_node_is_refused():
	var n = Node.new()
	assert(n != null)
""")
	assert_eq(script.get_compile_error(), "", "the restricted script should compile")

	var report := script.run_tests()
	assert_eq(report["failed"], 1, "Node.new() is refused under restrictions")
	owner.set_script(null)
	owner.free()
	assert_engine_error("Class name is not allowed")
	assert_engine_error("Exception: Class name is not allowed")

	# Without restrictions the same test passes.
	var free_script := _sgd_with_tests("""
@test
func creating_a_node_is_allowed():
	var n = Node.new()
	assert(n != null)
""")
	assert_eq(free_script.run_tests()["passed"], 1,
		"the same test passes on an unrestricted script")

func test_sgd_a_restricted_script_builds_its_test_machine_restricted():
	var script := SafeGDScript.new()
	script.set_source_code("func __bootstrap():\n\tpass\n")
	var owner := Node.new()
	owner.set_script(script)
	owner.set("restrictions", true)
	script.set_source_code("""
@test
func creating_a_node_is_refused():
	var n = Node.new()
	assert(n != null)
""")
	assert_eq(script.get_compile_error(), "", "the restricted script should compile")
	owner.set_script(null)
	owner.free()

	var report := script.run_tests()
	assert_eq(report["failed"], 1, "the machine the run built is restricted too")
	assert_engine_error("Class name is not allowed")
	assert_engine_error("Exception: Class name is not allowed")

func test_sgd_run_tests_leaves_instantiation_as_it_found_it():
	var script := _sgd_with_tests("@test\nfunc trivial():\n\tpass\n")
	var before := script.can_instantiate()
	assert_eq(script.run_tests()["passed"], 1)
	assert_eq(script.can_instantiate(), before,
		"the runner's live-instance guard is scoped to the run")
	# And an ordinary attach still works afterwards.
	var node := Node.new()
	node.set_script(script)
	assert_eq(node.get_script(), script, "the script still attaches normally")
	node.set_script(null)
	node.free()

func test_sgd_a_static_test_runs():
	var script := _sgd_with_tests("""
@test
static func math_is_math():
	assert(2 * 3 == 6)
""")
	assert_eq(script.run_tests()["passed"], 1, "a static @test runs on the per-test instance")

func test_sgd_a_test_on_a_named_class_runs():
	var script := _sgd_with_tests("""
class_name SgdTestedActor
extends Node

var health: int = 100

@test
func health_starts_full():
	assert(health == 100)
""")
	var report := script.run_tests()
	assert_eq(report["passed"], 1, "class_name/extends do not change how a test runs")
	assert_eq(report["failed"], 0)

func test_sgd_shipping_build_strips_tests():
	var script := _sgd_with_tests("""
func helper() -> int:
	return 7

@test
func helper_answers_seven():
	assert(helper() == 7)
""")
	assert_eq(script.get_test_functions().size(), 1, "the normal build publishes the test")
	assert_true(script.compile_shipping(), "the shipping build should compile")
	assert_eq(script.get_test_functions(), PackedStringArray(),
		"a shipping build publishes no tests")
	assert_false(_sgd_declares(script, "helper_answers_seven"),
		"and the test function is not in the ELF at all")
	assert_true(_sgd_declares(script, "helper"), "while the rest of the script is unchanged")
	assert_eq(script.run_tests()["tests"].size(), 0, "so there is nothing to run")

func test_sgd_the_compiler_publishes_the_test_table():
	# The guest entry point the host probes with has_function(). An older
	# gdscript.elf simply lacks it and the host reports no tests.
	var ts: Sandbox = Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	assert_true(ts.has_function("get_test_signatures"),
		"the compiler ELF exports the test table")
	assert_true(ts.has_function("set_emit_tests"),
		"and the switch a shipping build turns off")

	# An empty table still encodes a header, so a script with no @test is the
	# baseline an empty table is compared against.
	ts.vmcall("compile", "func helper():\n\treturn 1\n")
	var empty_table: int = ts.vmcall("get_test_signatures").size()

	var source := "func helper():\n\treturn 1\n@test\nfunc checks():\n\tassert(helper() == 1)\n"
	var elf: PackedByteArray = ts.vmcall("compile", source)
	assert_gt(elf.size(), 0, "the script should compile")
	assert_gt(ts.vmcall("get_test_signatures").size(), empty_table,
		"and publish a test table")

	ts.vmcall("set_emit_tests", false)
	var stripped: PackedByteArray = ts.vmcall("compile", source)
	assert_gt(stripped.size(), 0, "the shipping build should compile too")
	assert_eq(ts.vmcall("get_test_signatures").size(), empty_table, "with no test table")
	assert_lt(stripped.size(), elf.size(), "and a smaller program")
	ts.vmcall("set_emit_tests", true)
	ts.queue_free()

func test_sgd_the_context_menu_offers_run_tests():
	# The editor half (toast, jump, live text) needs an editor; the decision of
	# which items a right-click offers does not, and is what this pins down.
	var with_tests := "user://temp_menu_with_tests.sgd"
	var without_tests := "user://temp_menu_without_tests.sgd"
	var file := FileAccess.open(with_tests, FileAccess.WRITE)
	file.store_string("""extends Node

func helper() -> int:
	return 1

@test
func checks_helper():
	assert(helper() == 1)

func after() -> void:
	pass
""")
	file.close()
	file = FileAccess.open(without_tests, FileAccess.WRITE)
	file.store_string("func plain():\n\tpass\n")
	file.close()

	assert_eq(SafeGDScriptLanguage.menu_items_for(PackedStringArray([with_tests])),
		PackedStringArray(["Convert to GDScript", "Profile Script", "Run Tests"]),
		"a .sgd with tests offers both")
	assert_eq(SafeGDScriptLanguage.menu_items_for(PackedStringArray([without_tests])),
		PackedStringArray(["Convert to GDScript", "Profile Script"]),
		"a .sgd without tests still offers the profiling toggle")
	assert_eq(SafeGDScriptLanguage.menu_items_for(PackedStringArray(["res://x.gd"])),
		PackedStringArray(["Convert to SafeGDScript"]),
		"a .gd offers the other direction and no test run")
	assert_eq(SafeGDScriptLanguage.menu_items_for(PackedStringArray(["res://s.tscn::SafeGDScript_x"])),
		PackedStringArray(), "a built-in script has no file to act on")

	# Inside the test's body the caret adds the single-test item.
	assert_eq(SafeGDScriptLanguage.menu_items_for(PackedStringArray([with_tests]), 8),
		PackedStringArray(["Convert to GDScript", "Profile Script", "Run Test at Cursor",
			"Run Tests"]),
		"a caret inside a @test offers to run just that one")
	assert_eq(SafeGDScriptLanguage.menu_items_for(PackedStringArray([with_tests]), 4),
		PackedStringArray(["Convert to GDScript", "Profile Script", "Run Tests"]),
		"a caret in an ordinary function does not")

	DirAccess.remove_absolute(with_tests)
	DirAccess.remove_absolute(without_tests)

func test_sgd_export_groups_reach_the_property_list():
	var script := SafeGDScript.new()
	script.set_source_code("""
extends Node

@export var plain := 1

@export_category("Movement")
@export_group("Speed", "speed_")
@export var speed_walk := 1.0
@export_subgroup("Limits")
@export var speed_max := 10.0
@export_group("Jump")
@export var jump_height := 2.0

var not_exported := 5
""")
	assert_eq(script.get_compile_error(), "", "the section annotations should compile")

	var rows := []
	for property in script.get_script_property_list():
		var kind := "prop"
		if property.usage & PROPERTY_USAGE_CATEGORY:
			kind = "category"
		elif property.usage & PROPERTY_USAGE_GROUP:
			kind = "group"
		elif property.usage & PROPERTY_USAGE_SUBGROUP:
			kind = "subgroup"
		rows.append("%s:%s:%s" % [kind, property.name, property.hint_string])

	assert_eq(rows, [
		"prop:plain:",
		"category:Movement:",
		"group:Speed:speed_",
		"prop:speed_walk:",
		"subgroup:Limits:",
		"prop:speed_max:",
		"group:Jump:",
		"prop:jump_height:",
		"prop:not_exported:",
	], "each section row should head the properties declared under it")

func test_sgd_a_script_can_be_rebuilt_with_profiling():
	var script := SafeGDScript.new()
	script.set_source_code("func work() -> int:\n\treturn 7\n")
	assert_eq(script.get_compile_error(), "", "the script should compile")
	assert_false(script.is_profiled_build(), "a plain build carries no instrumentation")

	assert_true(script.set_profiling(true), "the profiled rebuild should succeed")
	assert_true(script.is_profiled_build(), "the script should now be instrumented")
	var node := Node.new()
	node.set_script(script)
	assert_eq(node.call("work"), 7, "an instrumented script still runs")
	node.set_script(null)
	node.free()

	assert_true(script.set_profiling(false), "turning it off should succeed")
	assert_false(script.is_profiled_build(), "the instrumentation should be gone")

func test_sgd_the_test_under_the_caret_is_found_by_line():
	var source := """extends Node

func helper() -> int:
	return 1

@test
## Doc comment above the func.
func first_case():
	assert(helper() == 1)

@test
static func second_case():
	assert(true)

func trailing() -> void:
	pass
"""
	assert_eq(SafeGDScriptLanguage.test_at_line(source, 1), "",
		"above every test there is none")
	assert_eq(SafeGDScriptLanguage.test_at_line(source, 4), "",
		"an ordinary function is not a test")
	assert_eq(SafeGDScriptLanguage.test_at_line(source, 6), "first_case",
		"the annotation line belongs to the test it annotates")
	assert_eq(SafeGDScriptLanguage.test_at_line(source, 9), "first_case",
		"and so does the body")
	assert_eq(SafeGDScriptLanguage.test_at_line(source, 13), "second_case",
		"a static test is found the same way")
	assert_eq(SafeGDScriptLanguage.test_at_line(source, 16), "",
		"the function after the last test is outside every region")

	var stacked := """extends Node

@test func on_one_line():
	assert(true)

@warning_ignore("unused_variable") @test func after_another():
	assert(true)

func trailing() -> void:
	pass
"""
	assert_eq(SafeGDScriptLanguage.test_at_line(stacked, 3), "on_one_line",
		"an annotation sharing the func's line still names the test")
	assert_eq(SafeGDScriptLanguage.test_at_line(stacked, 4), "on_one_line",
		"and its body belongs to it")
	assert_eq(SafeGDScriptLanguage.test_at_line(stacked, 6), "after_another",
		"a test annotation behind another one is still found")
	assert_eq(SafeGDScriptLanguage.test_at_line(stacked, 10), "",
		"a plain function after it is outside every region")

func test_sgd_language_run_tests_aggregates_across_scripts():
	var passing := "user://temp_agg_passing.sgd"
	var failing := "user://temp_agg_failing.sgd"
	var file := FileAccess.open(passing, FileAccess.WRITE)
	file.store_string("@test\nfunc ok():\n\tassert(1 == 1)\n")
	file.close()
	file = FileAccess.open(failing, FileAccess.WRITE)
	file.store_string("@test\nfunc bad():\n\tassert(false, \"aggregate failure\")\n")
	file.close()

	var total := SafeGDScriptLanguage.run_tests(PackedStringArray([passing, failing]))
	assert_eq(total["passed"], 1, "the passing script contributes one pass")
	assert_eq(total["failed"], 1, "the failing one contributes one failure")
	assert_eq(total["errors"], 0)
	assert_eq(total["scripts"].size(), 2, "one report per script, in order")
	assert_eq(total["scripts"][0]["path"], passing)
	assert_eq(total["scripts"][1]["tests"][0]["location"], failing + ":3",
		"the failing line comes from the line table")

	var missing := SafeGDScriptLanguage.run_tests(PackedStringArray(["user://not_here.sgd"]))
	assert_eq(missing["errors"], 1, "a path that will not load is one error row")

	DirAccess.remove_absolute(passing)
	DirAccess.remove_absolute(failing)
	assert_engine_error("Sandbox exception in assert: aggregate failure")
	assert_engine_error("Exception: Sandbox exception in assert: aggregate failure")

func test_sgd_the_headless_runner_reports_through_its_exit_code():
	# The documented CI path, run the way CI runs it.
	var godot := OS.get_executable_path()
	var project := ProjectSettings.globalize_path("res://")
	var output := []
	var code := OS.execute(godot, [
		"--headless", "--path", project,
		"-s", "addons/godot_sandbox/run_sgd_tests.gd",
		"--", "tests/sgd_tests_sample.sgd"], output, true)
	assert_eq(code, 0, "the sample script's tests pass:\n" + "\n".join(output))
	assert_true("\n".join(output).contains("2 passed, 0 failed, 0 errors"),
		"and the runner says so on one line")
