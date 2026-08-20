#pragma once
// A shared corpus of small GDScript programs.
//
// The corpus exists so that the checks which need programs rather than
// assertions -- optimization invariance (test_opt_invariance), and the
// interpreter/backend differential run (test_differential) -- all see the same
// set, and so that adding a program benefits every one of them at once.
//
// Every program defines `test()` and returns a value the IR interpreter can
// represent: an integer, a float, a bool or a string. Programs needing the host
// Variant API belong in the Godot integration tests instead.
#include <string>
#include <vector>

namespace gdscript_test {

struct CorpusProgram {
	const char* name;
	const char* source;
};

inline const std::vector<CorpusProgram>& corpus() {
	static const std::vector<CorpusProgram> programs = {
		{ "int_arithmetic", R"(
func test():
	var a = 7
	var b = 3
	return a * b + a - b * 2
)" },
		{ "int_division_and_modulo", R"(
func test():
	var a = 17
	var b = 5
	return a / b * 100 + a % b
)" },
		{ "negative_and_unary", R"(
func test():
	var a = -12
	var b = -a
	return -(a + b) + b
)" },
		{ "float_arithmetic", R"(
func test():
	var a = 1.5
	var b = 0.25
	return a * b + a / b - b
)" },
		{ "mixed_int_float", R"(
func test():
	var i = 3
	var f = 0.5
	return i + f * i - 1
)" },
		{ "float_division_is_float", R"(
func test():
	var a = 7.0
	return a / 2.0
)" },
		{ "bitwise", R"(
func test():
	var a = 0b1011
	var b = 6
	return (a & b) | (a ^ b) | (a << 2) | (a >> 1)
)" },
		{ "bitwise_not", R"(
func test():
	var a = 5
	return ~a + (~0)
)" },
		{ "comparisons", R"(
func test():
	var a = 4
	var b = 9
	var total = 0
	if a < b:
		total = total + 1
	if a <= b:
		total = total + 2
	if a > b:
		total = total + 4
	if a >= b:
		total = total + 8
	if a == b:
		total = total + 16
	if a != b:
		total = total + 32
	return total
)" },
		{ "float_comparison", R"(
func test():
	var a = 0.1 + 0.2
	if a == 0.3:
		return 1
	if a > 0.3:
		return 2
	return 3
)" },
		{ "logical_and_or", R"(
func test():
	var a = 1
	var b = 0
	var total = 0
	if a and not b:
		total = total + 1
	if b or a:
		total = total + 2
	if not a and not b:
		total = total + 4
	return total
)" },
		{ "short_circuit_side_effect", R"(
var calls = 0

func bump():
	calls = calls + 1
	return 1

func test():
	var a = 0
	if a and bump():
		calls = calls + 100
	if a or bump():
		calls = calls + 1000
	return calls
)" },
		{ "not_truthiness", R"(
func test():
	var zero = 0
	var one = 1
	var f = 0.0
	var total = 0
	if not zero:
		total = total + 1
	if not one:
		total = total + 2
	if not f:
		total = total + 4
	return total
)" },
		{ "if_elif_else", R"(
func classify(n):
	if n < 0:
		return -1
	elif n == 0:
		return 0
	elif n < 10:
		return 1
	else:
		return 2

func test():
	return classify(-5) * 1000 + classify(0) * 100 + classify(5) * 10 + classify(50)
)" },
		{ "while_accumulate", R"(
func test():
	var i = 0
	var sum = 0
	while i < 10:
		sum = sum + i * i
		i = i + 1
	return sum
)" },
		{ "while_break_continue", R"(
func test():
	var i = 0
	var sum = 0
	while true:
		i = i + 1
		if i > 20:
			break
		if i % 3 == 0:
			continue
		sum = sum + i
	return sum
)" },
		{ "nested_loops", R"(
func test():
	var i = 0
	var total = 0
	while i < 5:
		var j = 0
		while j < 5:
			total = total + i * j
			j = j + 1
		i = i + 1
	return total
)" },
		{ "for_range", R"(
func test():
	var sum = 0
	for i in range(10):
		sum = sum + i
	return sum
)" },
		{ "for_range_start_end_step", R"(
func test():
	var sum = 0
	for i in range(2, 20, 3):
		sum = sum + i
	return sum
)" },
		{ "loop_invariant_expression", R"(
func test():
	var a = 6
	var b = 7
	var i = 0
	var total = 0
	while i < 8:
		total = total + a * b
		i = i + 1
	return total
)" },
		{ "recursion_fib", R"(
func fib(n):
	if n < 2:
		return n
	return fib(n - 1) + fib(n - 2)

func test():
	return fib(15)
)" },
		{ "call_result_not_a_constant", R"(
func side():
	return 7

func test():
	var x = 1
	x = side()
	return x + 1
)" },
		{ "call_between_constant_and_use", R"(
func other():
	return 3

func test():
	var a = 10
	var b = other()
	return a + b
)" },
		{ "globals_read_write", R"(
var counter = 5

func bump():
	counter = counter + 1
	return counter

func test():
	bump()
	bump()
	return counter * 10
)" },
		{ "global_const", R"(
const LIMIT = 12

func test():
	var sum = 0
	var i = 0
	while i < LIMIT:
		sum = sum + i
		i = i + 1
	return sum
)" },
		{ "local_shadows_global", R"(
var counter = 10

func test():
	var counter = 1
	counter = counter + 1
	return counter
)" },
		{ "parameter_shadows_global", R"(
var value = 100

func inner(value):
	return value + 1

func test():
	return inner(1)
)" },
		{ "typed_locals", R"(
func test():
	var a: int = 5
	var b: float = 2.0
	var c = a + b
	return c
)" },
		{ "string_concat", R"(
func test():
	var a = "hello"
	var b = " world"
	return a + b
)" },
		{ "string_compare", R"(
func test():
	var a = "abc"
	var b = "abd"
	if a == b:
		return 1
	if a < b:
		return 2
	return 3
)" },
		{ "ternary", R"(
func test():
	var a = 4
	var b = 9
	var m = a if a > b else b
	return m * 10 + (1 if a < b else 0)
)" },
		{ "deep_expression", R"(
func test():
	var a = 2
	var b = 3
	var c = 4
	return ((a + b) * (c - a) + (b * c - a)) * (a + (b - c) * (a + b))
)" },
		{ "move_chain", R"(
func test():
	var a = 3
	var b = a
	var c = b
	var d = c
	a = 9
	return b + c + d + a
)" },
		{ "dead_store_then_reuse", R"(
func test():
	var a = 1
	a = 2
	a = 3
	var b = a
	a = 4
	return a + b
)" },
		{ "reassign_in_loop", R"(
func test():
	var x = 0
	var i = 0
	while i < 6:
		var y = x
		x = y + i
		i = i + 1
	return x
)" },
		{ "match_statement", R"(
func pick(n):
	match n:
		1:
			return 10
		2:
			return 20
		_:
			return 30

func test():
	return pick(1) + pick(2) + pick(7)
)" },
		{ "compound_assign", R"(
func test():
	var a = 5
	a += 3
	a -= 1
	a *= 4
	a /= 2
	return a
)" },
		{ "many_locals", R"(
func test():
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
	return a + b * c - d + e * f - g + h * i - j
)" },
		{ "nested_block_shadowing", R"(
func test():
	var a = 1
	var total = 0
	if a == 1:
		var a = 5
		total = total + a
	total = total + a
	return total
)" },
		{ "float_accumulation_loop", R"(
func test():
	var x = 0.0
	var i = 0
	while i < 10:
		x = x + 0.5
		i = i + 1
	return x
)" },
		{ "int_to_float_promotion_in_loop", R"(
func test():
	var acc = 0
	var i = 1
	while i < 5:
		acc = acc + i / 2.0
		i = i + 1
	return acc
)" },
		{ "boolean_returned_directly", R"(
func test():
	var a = 3
	return a > 2 and a < 10
)" },
		{ "chained_calls", R"(
func inc(n):
	return n + 1

func double(n):
	return n * 2

func test():
	return double(inc(double(inc(3))))
)" },
		{ "default_arguments", R"(
func add(a, b = 10):
	return a + b

func test():
	return add(1) + add(1, 2)
)" },
		{ "conditional_invariant_in_loop", R"(
func test():
	var taken = 0
	var i = 4
	while i > 0:
		i = i - 1
		if true or false:
			taken = taken + 1
	return taken
)" },
		{ "conditional_assignment_in_loop", R"(
func test():
	var x = 9
	var i = 3
	while i > 0:
		i = i - 1
		if i == 1:
			x = 1
		else:
			x = 0
	return x
)" },
		// Enough locals that the upper Variant slots are past the 12-bit
		// immediate a load or store offset fits in. The wide path used to
		// compute the address in the register holding the value.
		{ "large_stack_frame", R"(
func test():
	var total = 0
	var i = 0
	while i < 3:
		var a = i + 1
		var b = a + i
		var c = b + a
		var d = c + b
		var e = d + c
		var f = e + d
		var g = f + e
		var h = g + f
		var j = h + g
		var k = j + h
		total = total + a + b + c + d + e + f + g + h + j + k
		i = i + 1
	return total
)" },
		{ "early_return_in_loop", R"(
func find(limit):
	var i = 0
	while i < 100:
		if i * i > limit:
			return i
		i = i + 1
	return -1

func test():
	return find(50) * 100 + find(1000)
)" },

		// -= Global functions =-
		//
		// The interpreter evaluates these through globals.cpp and the machine
		// through the emitted code and the ECALL_UTILITY shim, which evaluates
		// through globals.cpp too -- so a disagreement is a disagreement about
		// the emitted code, which is the point.
		{ "global_int_forms", R"(
func test():
	var a = -7
	var b = 3
	return absi(a) + signi(a) + mini(a, b) + maxi(a, b) + clampi(a, -2, 2) + posmod(a, b) + wrapi(a, 0, 5)
)" },
		{ "global_numeric_dispatch_int", R"(
func test():
	var a = -9
	return abs(a) + sign(a) + floor(a) + ceil(a) + round(a) + min(a, 4) + max(a, 4) + clamp(a, -3, 3)
)" },
		{ "global_numeric_dispatch_float", R"(
func test():
	var a = -9.5
	return abs(a) + sign(a) + floor(a) + ceil(a) + round(a) + min(a, 4.0) + max(a, 4.0) + clamp(a, -3.0, 3.0)
)" },
		// The same call sites, with the argument type unknown at compile time:
		// the backend has to emit the run-time test and both forms.
		{ "global_numeric_dispatch_untyped", R"(
func pick(which):
	if which:
		return -9
	return -9.5

func measure(x):
	return abs(x) + sign(x) + floor(x) + ceil(x) + round(x) + min(x, 4) + max(x, 4)

func test():
	return measure(pick(true)) + measure(pick(false))
)" },
		// The type constructors, in the form the compiler can lower inline: an
		// argument it already knows is a number or a bool. int() of a String
		// is Godot's parse and goes to the host, which this harness has no
		// Variants for.
		{ "global_type_constructors", R"(
func test():
	var f = 2.9
	var i = 7
	var zero = 0.0
	var total = 0
	total = total + int(f) + int(-f) + int(i)
	total = total + int(float(i)) + int(float(f) * 2.0)
	if bool(f) and not bool(zero):
		total = total + 100
	if bool(i) and not bool(0):
		total = total + 1000
	return total
)" },
		{ "global_float_forms", R"(
func test():
	var a = -2.5
	return absf(a) + signf(a) + minf(a, 1.0) + maxf(a, 1.0) + clampf(a, -1.0, 1.0) + sqrt(9.0)
)" },
		{ "global_rounding", R"(
func test():
	var a = 2.6
	return floorf(a) + ceilf(a) + roundf(a) + floori(a) + ceili(a) + roundi(a) + snappedf(a, 0.5) + snappedi(a, 2)
)" },
		{ "global_transcendental", R"(
func test():
	return sin(1.0) + cos(1.0) + tan(0.5) + asin(0.5) + acos(0.5) + atan(0.5) + atan2(1.0, 2.0) + exp(1.0) + log(2.0) + pow(2.0, 10.0)
)" },
		{ "global_hyperbolic", R"(
func test():
	return sinh(0.5) + cosh(0.5) + tanh(0.5) + asinh(0.5) + acosh(1.5) + atanh(0.5)
)" },
		{ "global_interpolation", R"(
func test():
	return lerp(1.0, 5.0, 0.25) + inverse_lerp(1.0, 5.0, 2.0) + smoothstep(0.0, 1.0, 0.3) 		+ remap(5.0, 0.0, 10.0, 100.0, 200.0) + move_toward(1.0, 5.0, 0.5) 		+ cubic_interpolate(1.0, 2.0, 0.0, 3.0, 0.5) + bezier_interpolate(0.0, 1.0, 2.0, 3.0, 0.25) 		+ bezier_derivative(0.0, 1.0, 2.0, 3.0, 0.25)
)" },
		{ "global_angles", R"(
func test():
	return deg_to_rad(90.0) + rad_to_deg(1.0) + angle_difference(0.5, 3.0) + lerp_angle(0.5, 3.0, 0.25) 		+ rotate_toward(0.5, 3.0, 0.25) + pingpong(7.0, 3.0)
)" },
		{ "global_modulo", R"(
func test():
	return fmod(7.5, 2.0) + fposmod(-7.5, 2.0) + wrapf(7.5, 1.0, 5.0) + linear_to_db(0.5) + db_to_linear(-6.0)
)" },
		{ "global_predicates", R"(
func test():
	var total = 0
	if is_nan(0.0):
		total = total + 1
	if is_inf(1.0):
		total = total + 2
	if is_finite(1.0):
		total = total + 4
	if is_zero_approx(0.0000001):
		total = total + 8
	if is_equal_approx(1.0, 1.0):
		total = total + 16
	return total
)" },
		// An integer where a float is wanted is the one implicit conversion
		// GDScript performs, and it has to reach the host as a double.
		{ "global_int_argument_widens", R"(
func test():
	return sqrt(16) + pow(2, 8) + lerp(0, 10, 1)
)" },
		{ "global_variadic_min_max", R"(
func test():
	return min(5, 2, 9, 1) + max(5, 2, 9, 1) + min(5.0, 2.0, 9.0)
)" },
		// The integer forms with the argument type unknown at compile time: the
		// backend has to load a Variant it has not been told the type of.
		{ "global_int_forms_untyped", R"(
func measure(x):
	return absi(x) + signi(x) + mini(x, 2) + maxi(x, 2) + clampi(x, -1, 1)

func test():
	return measure(-5) + measure(5) + measure(2.7) + measure(true)
)" },
		{ "global_edge_cases", R"(
func test():
	# A zero divisor and an empty range both answer rather than trap.
	return posmod(7, 0) + wrapi(7, 3, 3) + snappedi(7.0, 0) + mini(-9223372036854775807, 1)
)" },
		{ "global_in_a_loop", R"(
func test():
	var total = 0.0
	var i = 0
	while i < 8:
		total = total + abs(sin(i)) * clampf(i, 0.0, 4.0)
		i = i + 1
	return total
)" },
	};
	return programs;
}

} // namespace gdscript_test
