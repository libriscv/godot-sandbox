extends GutTest

# Typed array windows: `for i in range(n)` over a typed array keeps a run of raw
# elements in the guest frame instead of an ecall per `a[i]`.

var Sandbox_TestsTests = load("res://tests/tests.elf")

func _compile(source: String) -> Sandbox:
	var ts := Sandbox.new()
	ts.set_program(Sandbox_TestsTests)
	ts.restrictions = true
	var elf: PackedByteArray = ts.vmcall("compile_to_elf", source)
	ts.queue_free()
	assert_false(elf.is_empty(), "the source should compile")
	var s := Sandbox.new()
	s.load_buffer(elf)
	return s

func _ints(n: int) -> PackedInt32Array:
	var a := PackedInt32Array()
	for i in n:
		a.append(i * 3 - 7)
	return a

func test_packed_reads_cross_window_boundaries():
	var s := _compile("""
func sum32(a: PackedInt32Array) -> int:
	var s := 0
	for i in range(a.size()):
		s += a[i]
	return s

func sum64(a: PackedInt64Array) -> int:
	var s := 0
	for i in a.size():
		s += a[i]
	return s

func sum_bytes(a: PackedByteArray) -> int:
	var s := 0
	for i in a.size():
		s += a[i]
	return s

func sum_floats(a: PackedFloat32Array) -> float:
	var s := 0.0
	for i in a.size():
		s += a[i]
	return s

func sum_doubles(a: PackedFloat64Array) -> float:
	var s := 0.0
	for i in a.size():
		s += a[i]
	return s
""")
	for n in [0, 1, 255, 256, 257, 1000]:
		var a := _ints(n)
		var expected := 0
		for v in a:
			expected += v
		assert_eq(s.vmcallv("sum32", a), expected, "PackedInt32Array of %d" % n)
		assert_eq(s.vmcallv("sum64", PackedInt64Array(Array(a))), expected, "PackedInt64Array of %d" % n)
		var floats := PackedFloat32Array(Array(a))
		assert_almost_eq(s.vmcallv("sum_floats", floats), float(expected), 0.001, "PackedFloat32Array of %d" % n)
		assert_almost_eq(s.vmcallv("sum_doubles", PackedFloat64Array(Array(a))), float(expected), 0.001,
			"PackedFloat64Array of %d" % n)
	var bytes := PackedByteArray()
	var byte_sum := 0
	for i in 600:
		bytes.append((i * 7) & 255)
		byte_sum += (i * 7) & 255
	assert_eq(s.vmcallv("sum_bytes", bytes), byte_sum, "PackedByteArray reads are unsigned")
	s.queue_free()

func test_packed_writes_are_written_back():
	var s := _compile("""
func scale(a: PackedFloat32Array, f: float) -> PackedFloat32Array:
	for i in a.size():
		a[i] = a[i] * f
	return a

func bump(a: PackedByteArray) -> PackedByteArray:
	for i in a.size():
		a[i] = a[i] + 200
	return a

func widen(a: PackedFloat64Array) -> PackedFloat64Array:
	for i in a.size():
		a[i] = i
	return a

func narrow(a: PackedInt32Array) -> PackedInt32Array:
	for i in a.size():
		a[i] = 2.75
	return a

func prefix(a: PackedInt64Array) -> PackedInt64Array:
	for i in range(1, a.size()):
		a[i] = a[i - 1] + a[i]
	return a

func set_first(a: PackedFloat32Array, v: float) -> void:
	a[0] = v
""")
	var a := PackedFloat32Array()
	for i in 700:
		a.append(i * 0.5)
	var scaled: PackedFloat32Array = s.vmcallv("scale", a, 2.0)
	assert_eq(scaled.size(), 700, "the size is unchanged")
	assert_almost_eq(scaled[0], 0.0, 0.0001, "first element")
	assert_almost_eq(scaled[255], 255.0, 0.0001, "last element of the first window")
	assert_almost_eq(scaled[256], 256.0, 0.0001, "first element of the second window")
	assert_almost_eq(scaled[699], 699.0, 0.0001, "last element")
	# The window writes where `a[i] = v` outside a loop writes, caller included.
	var direct := PackedFloat32Array([1.0])
	s.vmcallv("set_first", direct, 7.0)
	assert_almost_eq(a[699], 349.5 if direct[0] == 1.0 else 699.0, 0.0001,
		"the caller sees the window's writes exactly when it sees an ordinary store")

	var bytes := PackedByteArray([0, 55, 56, 255])
	assert_eq(s.vmcallv("bump", bytes), PackedByteArray([200, 255, 0, 199]), "byte stores truncate like Godot")
	assert_eq(s.vmcallv("widen", PackedFloat64Array([9, 9, 9])), PackedFloat64Array([0, 1, 2]),
		"an int widens into a float element")
	assert_eq(s.vmcallv("narrow", PackedInt32Array([1, 2])), PackedInt32Array([2, 2]),
		"a float into an int element converts as Godot does")
	var ones := PackedInt64Array()
	ones.resize(600)
	ones.fill(1)
	var sums: PackedInt64Array = s.vmcallv("prefix", ones)
	assert_eq(sums[255], 256, "prefix sum inside the first window")
	assert_eq(sums[256], 257, "prefix sum reads the previous window's write")
	assert_eq(sums[599], 600, "prefix sum at the end")
	s.queue_free()

func test_vector_and_color_elements():
	var s := _compile("""
func move(p: PackedVector2Array, d: Vector2) -> PackedVector2Array:
	for i in p.size():
		p[i] = p[i] + d * i
	return p

func lengths(p: PackedVector3Array) -> float:
	var s := 0.0
	for i in p.size():
		s += p[i].length()
	return s

func dim(c: PackedColorArray) -> PackedColorArray:
	for i in c.size():
		c[i].a = 0.5
	return c

func flip(v: PackedVector4Array) -> PackedVector4Array:
	for i in v.size():
		v[i] = -v[i]
	return v
""")
	var p := PackedVector2Array()
	for i in 300:
		p.append(Vector2(i, -i))
	var moved: PackedVector2Array = s.vmcallv("move", p, Vector2(1, 2))
	assert_eq(moved[0], Vector2(0, 0), "first Vector2")
	assert_eq(moved[299], Vector2(598, 299), "last Vector2")
	var p3 := PackedVector3Array()
	for i in 270:
		p3.append(Vector3(0, 3, 4))
	assert_almost_eq(s.vmcallv("lengths", p3), 270 * 5.0, 0.01, "Vector3 elements, 12 bytes each")
	var colors := PackedColorArray([Color.RED, Color.BLUE])
	assert_eq(s.vmcallv("dim", colors), PackedColorArray([Color(1, 0, 0, 0.5), Color(0, 0, 1, 0.5)]),
		"a member of an element is an element read and write")
	assert_eq(s.vmcallv("flip", PackedVector4Array([Vector4(1, 2, 3, 4)])),
		PackedVector4Array([Vector4(-1, -2, -3, -4)]), "Vector4 elements")
	s.queue_free()

func test_typed_arrays():
	var s := _compile("""
func total(a: Array[int]) -> int:
	var s := 0
	for i in a.size():
		s += a[i]
	return s

func average(a: Array[float]) -> float:
	var s := 0.0
	for i in a.size():
		s += a[i]
	return s / a.size()

func shift(a: Array[Vector3i], d: Vector3i) -> Array[Vector3i]:
	for i in a.size():
		a[i] += d
	return a

func negate(a: Array[bool]) -> Array[bool]:
	for i in a.size():
		a[i] = not a[i]
	return a

func squares(n: int) -> Array[int]:
	var a: Array[int] = []
	a.resize(n)
	for i in n:
		a[i] = i * i
	return a

func resized_sum(n: int) -> int:
	var a: Array[int] = []
	a.resize(n)
	var s := 0
	for i in n:
		s += a[i] + 1
	return s

func copy_into(a: Array[int], b: Array[int]) -> Array[int]:
	for i in a.size():
		a[i] = b[i] + 1
	return a
""")
	var ints: Array[int] = []
	for i in 400:
		ints.append(i)
	assert_eq(s.vmcallv("total", ints), 399 * 400 / 2, "Array[int]")
	assert_almost_eq(s.vmcallv("average", [1, 2.0, 3]), 2.0, 0.0001, "an int in Array[float] reads as a float")
	var shifted: Array = s.vmcallv("shift", [Vector3i(1, 1, 1), Vector3i(2, 2, 2)], Vector3i(1, 0, -1))
	assert_eq(shifted, [Vector3i(2, 1, 0), Vector3i(3, 2, 1)], "Array[Vector3i] writes reach the caller's Array")
	assert_eq(s.vmcallv("negate", [true, false]), [false, true], "Array[bool]")
	var squares: Array = s.vmcallv("squares", 300)
	assert_eq(squares.size(), 300, "resize() then fill a local Array[int]")
	assert_eq(squares[299], 299 * 299, "the fill reaches the second window")
	assert_eq(s.vmcallv("resized_sum", 300), 300, "the nulls resize() leaves read as Array[int]'s 0")
	var same := [1, 2, 3]
	assert_eq(s.vmcallv("copy_into", same, same), [2, 3, 4],
		"two names for one Array take no window, so each read sees the last write")

	var before := s.get_exceptions()
	assert_null(s.vmcallv("total", [1, "two", 3]), "an element that is not an int throws")
	assert_eq(s.get_exceptions(), before + 1, "and is reported as an exception")
	assert_engine_error("is a String")
	s.queue_free()

func test_negative_and_out_of_range_indices():
	var s := _compile("""
func reversed_sum(a: PackedInt32Array) -> Array:
	var weighted := 0
	for i in range(1, a.size() + 1):
		weighted = weighted * 3 + a[-i]
	return [weighted, a[-1]]

func set_last(a: PackedInt32Array) -> PackedInt32Array:
	for i in 1:
		a[-1] = 42
	return a

func past_end(a: PackedInt32Array) -> int:
	var s := 0
	for i in a.size() + 1:
		s += a[i]
	return s
""")
	var a := PackedInt32Array([1, 2, 3, 4, 5])
	var expected := 0
	for i in range(1, 6):
		expected = expected * 3 + a[-i]
	assert_eq(s.vmcallv("reversed_sum", a), [expected, 5], "negative indices count from the end")
	assert_eq(s.vmcallv("set_last", a), PackedInt32Array([1, 2, 3, 4, 42]), "a negative index store")

	var before := s.get_exceptions()
	assert_null(s.vmcallv("past_end", a), "reading past the end throws")
	assert_eq(s.get_exceptions(), before + 1, "and is reported as an exception")
	assert_engine_error("out of bounds")
	s.queue_free()

func test_every_exit_writes_back():
	var s := _compile("""
func until_negative(a: PackedInt32Array) -> PackedInt32Array:
	for i in a.size():
		if a[i] < 0:
			break
		a[i] = a[i] * 10
	return a

func first_big(a: PackedInt32Array, limit: int) -> Array:
	for i in a.size():
		a[i] += 1
		if a[i] > limit:
			return [i, a]
	return [-1, a]

func untyped_index(a: PackedInt32Array, order: Array) -> PackedInt32Array:
	for i in order.size():
		a[order[i]] = i
	return a
""")
	assert_eq(s.vmcallv("until_negative", PackedInt32Array([1, 2, -3, 4])), PackedInt32Array([10, 20, -3, 4]),
		"break leaves through the flush")
	var big: Array = s.vmcallv("first_big", PackedInt32Array([1, 5, 9, 2]), 5)
	assert_eq(big[0], 1, "return inside the loop")
	assert_eq(big[1], PackedInt32Array([2, 6, 9, 2]), "a returned array holds the writes made so far")
	assert_eq(s.vmcallv("untyped_index", PackedInt32Array([0, 0, 0]), [2, 0, 1]), PackedInt32Array([1, 2, 0]),
		"an untyped index is tested and still uses the window")
	assert_eq(s.vmcallv("untyped_index", PackedInt32Array([0, 0]), [1.0, 0.0]), PackedInt32Array([1, 0]),
		"a float index takes the ordinary assignment")
	s.queue_free()

func test_nested_loops_and_several_arrays():
	var s := _compile("""
func matmul(a: PackedFloat64Array, b: PackedFloat64Array, n: int) -> PackedFloat64Array:
	var c := PackedFloat64Array()
	c.resize(n * n)
	for i in n:
		for j in n:
			var acc := 0.0
			for k in n:
				acc += a[i * n + k] * b[k * n + j]
			c[i * n + j] = acc
	return c
""")
	var n := 20
	var a := PackedFloat64Array()
	var b := PackedFloat64Array()
	for i in n * n:
		a.append(i % 7)
		b.append(i % 5 - 2)
	var c: PackedFloat64Array = s.vmcallv("matmul", a, b, n)
	for probe in [[0, 0], [7, 13], [19, 19]]:
		var expected := 0.0
		for k in n:
			expected += a[probe[0] * n + k] * b[k * n + probe[1]]
		assert_almost_eq(c[probe[0] * n + probe[1]], expected, 0.0001, "matmul element %s" % [probe])
	s.queue_free()

func test_loops_that_must_not_take_a_window():
	var s := _compile("""
func helper(a: PackedInt32Array) -> int:
	return a[0]

func with_call(a: PackedInt32Array) -> int:
	var s := 0
	for i in a.size():
		a[i] = i
		s += helper(a)
	return s

func with_append(a: PackedInt32Array) -> PackedInt32Array:
	for i in 3:
		a.append(a[i])
	return a

func with_reassign(a: PackedInt32Array) -> int:
	var s := 0
	for i in 3:
		s += a[i]
		a = PackedInt32Array([10, 20, 30])
	return s
""")
	assert_eq(s.vmcallv("with_call", PackedInt32Array([5, 5, 5])), 0, "a call sees the array as written")
	assert_eq(s.vmcallv("with_append", PackedInt32Array([1, 2, 3])), PackedInt32Array([1, 2, 3, 1, 2, 3]),
		"a method that changes the size")
	assert_eq(s.vmcallv("with_reassign", PackedInt32Array([1, 2, 3])), 1 + 20 + 30, "reassigning the variable")
	s.queue_free()

func test_sgd_member_arrays():
	var script := SafeGDScript.new()
	script.set_source_code("""
extends Node

var data: PackedInt32Array
var weights: Array[float] = [0.5, 1.5]

func fill(n: int) -> void:
	data.resize(n)
	for i in n:
		data[i] = i * i

func total() -> int:
	var s := 0
	for i in data.size():
		s += data[i]
	return s

func clear_until(limit: int) -> int:
	for i in data.size():
		if data[i] >= limit:
			return i
		data[i] = 0
	return -1

func scale_weights(f: float) -> void:
	for i in weights.size():
		weights[i] *= f
""")
	assert_eq(script.get_compile_error(), "", "the script should compile")
	var node := Node.new()
	node.set_script(script)
	node.call("fill", 300)
	var data: PackedInt32Array = node.get("data")
	assert_eq(data.size(), 300, "a member written in a loop is stored back")
	assert_eq(data[299], 299 * 299, "its last element")
	var expected := 0
	for i in 300:
		expected += i * i
	assert_eq(node.call("total"), expected, "a member read in a loop")
	assert_eq(node.call("clear_until", 100), 10, "return from inside the loop")
	data = node.get("data")
	assert_eq(data[9], 0, "writes before the return reach the member")
	assert_eq(data[10], 100, "elements after it are untouched")
	node.call("scale_weights", 2.0)
	assert_eq(node.get("weights"), [1.0, 3.0], "an Array[float] member")
	node.free()
