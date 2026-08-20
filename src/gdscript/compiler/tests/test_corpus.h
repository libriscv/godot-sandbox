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
	};
	return programs;
}

} // namespace gdscript_test
