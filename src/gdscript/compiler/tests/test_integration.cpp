#include "../compiler.h"
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_interpreter.h"
#include <cassert>
#include <iostream>
#include <cmath>
#include <fstream>

using namespace gdscript;

// Helper to compile and execute
IRInterpreter::Value execute(const std::string& source, const std::string& function = "main",
                             const std::vector<IRInterpreter::Value>& args = {}) {
	// Compile to IR and execute (skip ELF generation for now, since RISC-V backend has limitations)
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Execute using interpreter
	IRInterpreter interp(ir);
	return interp.call(function, args);
}

int64_t execute_int(const std::string& source, const std::string& function = "main",
                     const std::vector<IRInterpreter::Value>& args = {}) {
	auto result = execute(source, function, args);
	return std::get<int64_t>(result);
}

void test_simple_return() {
	std::cout << "Testing simple return..." << std::endl;

	std::string source = R"(
func main():
	return 42
)";

	int64_t result = execute_int(source);
	assert(result == 42);

	std::cout << "  ✓ Simple return test passed (result: " << result << ")" << std::endl;
}

void test_arithmetic() {
	std::cout << "Testing arithmetic operations..." << std::endl;

	std::string source = R"(
func add(a, b):
	return a + b

func subtract(a, b):
	return a - b

func multiply(a, b):
	return a * b

func divide(a, b):
	return a / b

func modulo(a, b):
	return a % b
)";

	assert(execute_int(source, "add", {int64_t(10), int64_t(20)}) == 30);
	assert(execute_int(source, "subtract", {int64_t(50), int64_t(15)}) == 35);
	assert(execute_int(source, "multiply", {int64_t(6), int64_t(7)}) == 42);
	assert(execute_int(source, "divide", {int64_t(84), int64_t(2)}) == 42);
	assert(execute_int(source, "modulo", {int64_t(17), int64_t(5)}) == 2);

	std::cout << "  ✓ All arithmetic operations passed" << std::endl;
}

void test_variables() {
	std::cout << "Testing variable operations..." << std::endl;

	std::string source = R"(
func main():
	var x = 10
	var y = 20
	var sum = x + y
	return sum
)";

	int64_t result = execute_int(source);
	assert(result == 30);

	std::cout << "  ✓ Variable operations passed (result: " << result << ")" << std::endl;
}

void test_if_statement() {
	std::cout << "Testing if statements..." << std::endl;

	std::string source = R"(
func abs(x):
	if x < 0:
		return -x
	else:
		return x

func sign(x):
	if x > 0:
		return 1
	elif x < 0:
		return -1
	else:
		return 0
)";

	assert(execute_int(source, "abs", {int64_t(-42)}) == 42);
	assert(execute_int(source, "abs", {int64_t(42)}) == 42);
	assert(execute_int(source, "sign", {int64_t(10)}) == 1);
	assert(execute_int(source, "sign", {int64_t(-10)}) == -1);
	assert(execute_int(source, "sign", {int64_t(0)}) == 0);

	std::cout << "  ✓ If statement tests passed" << std::endl;
}

void test_while_loop() {
	std::cout << "Testing while loops..." << std::endl;

	std::string source = R"(
func sum_to_n(n):
	var sum = 0
	var i = 1
	while i <= n:
		sum = sum + i
		i = i + 1
	return sum

func factorial(n):
	var result = 1
	var i = 1
	while i <= n:
		result = result * i
		i = i + 1
	return result
)";

	assert(execute_int(source, "sum_to_n", {int64_t(10)}) == 55);  // 1+2+...+10 = 55
	assert(execute_int(source, "sum_to_n", {int64_t(100)}) == 5050);
	assert(execute_int(source, "factorial", {int64_t(5)}) == 120);  // 5! = 120
	assert(execute_int(source, "factorial", {int64_t(6)}) == 720);  // 6! = 720

	std::cout << "  ✓ While loop tests passed" << std::endl;
}

void test_fibonacci() {
	std::cout << "Testing Fibonacci (recursive)..." << std::endl;

	std::string source = R"(
func fibonacci(n):
	if n <= 1:
		return n
	return fibonacci(n - 1) + fibonacci(n - 2)
)";

	// Note: Recursive calls not yet fully implemented in interpreter
	// This test will be enabled when CALL opcode supports recursion
	std::cout << "  ⚠ Fibonacci test skipped (recursive calls not yet implemented)" << std::endl;

	// Expected results when working:
	// fibonacci(0) = 0
	// fibonacci(1) = 1
	// fibonacci(5) = 5
	// fibonacci(10) = 55
}

void test_fibonacci_iterative() {
	std::cout << "Testing Fibonacci (iterative)..." << std::endl;

	std::string source = R"(
func fibonacci(n):
	if n <= 1:
		return n
	var a = 0
	var b = 1
	var i = 2
	while i <= n:
		var temp = a + b
		a = b
		b = temp
		i = i + 1
	return b
)";

	assert(execute_int(source, "fibonacci", {int64_t(0)}) == 0);
	assert(execute_int(source, "fibonacci", {int64_t(1)}) == 1);
	assert(execute_int(source, "fibonacci", {int64_t(2)}) == 1);
	assert(execute_int(source, "fibonacci", {int64_t(3)}) == 2);
	assert(execute_int(source, "fibonacci", {int64_t(4)}) == 3);
	assert(execute_int(source, "fibonacci", {int64_t(5)}) == 5);
	assert(execute_int(source, "fibonacci", {int64_t(6)}) == 8);
	assert(execute_int(source, "fibonacci", {int64_t(10)}) == 55);

	std::cout << "  ✓ Fibonacci (iterative) passed: fib(10) = 55" << std::endl;
}

void test_comparison_operators() {
	std::cout << "Testing comparison operators..." << std::endl;

	std::string source = R"(
func test_eq(a, b):
	if a == b:
		return 1
	return 0

func test_ne(a, b):
	if a != b:
		return 1
	return 0

func test_lt(a, b):
	if a < b:
		return 1
	return 0

func test_lte(a, b):
	if a <= b:
		return 1
	return 0

func test_gt(a, b):
	if a > b:
		return 1
	return 0

func test_gte(a, b):
	if a >= b:
		return 1
	return 0
)";

	assert(execute_int(source, "test_eq", {int64_t(5), int64_t(5)}) == 1);
	assert(execute_int(source, "test_eq", {int64_t(5), int64_t(6)}) == 0);

	assert(execute_int(source, "test_ne", {int64_t(5), int64_t(6)}) == 1);
	assert(execute_int(source, "test_ne", {int64_t(5), int64_t(5)}) == 0);

	assert(execute_int(source, "test_lt", {int64_t(5), int64_t(10)}) == 1);
	assert(execute_int(source, "test_lt", {int64_t(10), int64_t(5)}) == 0);

	assert(execute_int(source, "test_lte", {int64_t(5), int64_t(5)}) == 1);
	assert(execute_int(source, "test_lte", {int64_t(5), int64_t(10)}) == 1);

	assert(execute_int(source, "test_gt", {int64_t(10), int64_t(5)}) == 1);
	assert(execute_int(source, "test_gt", {int64_t(5), int64_t(10)}) == 0);

	assert(execute_int(source, "test_gte", {int64_t(10), int64_t(10)}) == 1);
	assert(execute_int(source, "test_gte", {int64_t(10), int64_t(5)}) == 1);

	std::cout << "  ✓ All comparison operators passed" << std::endl;
}

void test_logical_operators() {
	std::cout << "Testing logical operators..." << std::endl;

	std::string source = R"(
func test_and(a, b):
	if a and b:
		return 1
	return 0

func test_or(a, b):
	if a or b:
		return 1
	return 0

func test_not(a):
	if not a:
		return 1
	return 0
)";

	assert(execute_int(source, "test_and", {int64_t(1), int64_t(1)}) == 1);
	assert(execute_int(source, "test_and", {int64_t(1), int64_t(0)}) == 0);
	assert(execute_int(source, "test_and", {int64_t(0), int64_t(0)}) == 0);

	assert(execute_int(source, "test_or", {int64_t(1), int64_t(0)}) == 1);
	assert(execute_int(source, "test_or", {int64_t(0), int64_t(1)}) == 1);
	assert(execute_int(source, "test_or", {int64_t(0), int64_t(0)}) == 0);

	assert(execute_int(source, "test_not", {int64_t(0)}) == 1);
	assert(execute_int(source, "test_not", {int64_t(1)}) == 0);

	std::cout << "  ✓ All logical operators passed" << std::endl;
}

void test_complex_expression() {
	std::cout << "Testing complex expressions..." << std::endl;

	std::string source = R"(
func compute(a, b, c):
	return (a + b) * c - a / b
)";

	// (10 + 5) * 2 - 10 / 5 = 15 * 2 - 2 = 30 - 2 = 28
	assert(execute_int(source, "compute", {int64_t(10), int64_t(5), int64_t(2)}) == 28);

	std::cout << "  ✓ Complex expression passed" << std::endl;
}

void test_nested_loops() {
	std::cout << "Testing nested loops..." << std::endl;

	std::string source = R"(
func sum_matrix():
	var sum = 0
	var i = 1
	var j = 0
	while i <= 3:
		j = 1
		while j <= 3:
			sum = sum + i * j
			j = j + 1
		i = i + 1
	return sum
)";

	// 1*1 + 1*2 + 1*3 + 2*1 + 2*2 + 2*3 + 3*1 + 3*2 + 3*3
	// = 1 + 2 + 3 + 2 + 4 + 6 + 3 + 6 + 9 = 36
	assert(execute_int(source, "sum_matrix") == 36);

	std::cout << "  ✓ Nested loops passed (result: 36)" << std::endl;
}

void test_prime_number() {
	std::cout << "Testing prime number check..." << std::endl;

	std::string source = R"(
func is_prime(n):
	if n <= 1:
		return 0
	if n == 2:
		return 1

	var i = 2
	while i * i <= n:
		if n % i == 0:
			return 0
		i = i + 1
	return 1
)";

	assert(execute_int(source, "is_prime", {int64_t(2)}) == 1);
	assert(execute_int(source, "is_prime", {int64_t(3)}) == 1);
	assert(execute_int(source, "is_prime", {int64_t(4)}) == 0);
	assert(execute_int(source, "is_prime", {int64_t(17)}) == 1);
	assert(execute_int(source, "is_prime", {int64_t(18)}) == 0);
	assert(execute_int(source, "is_prime", {int64_t(97)}) == 1);

	std::cout << "  ✓ Prime number check passed" << std::endl;
}

void test_gcd() {
	std::cout << "Testing GCD (Greatest Common Divisor)..." << std::endl;

	std::string source = R"(
func gcd(a, b):
	while b != 0:
		var temp = b
		b = a % b
		a = temp
	return a
)";

	assert(execute_int(source, "gcd", {int64_t(48), int64_t(18)}) == 6);
	assert(execute_int(source, "gcd", {int64_t(100), int64_t(50)}) == 50);
	assert(execute_int(source, "gcd", {int64_t(17), int64_t(13)}) == 1);

	std::cout << "  ✓ GCD test passed" << std::endl;
}

void test_variable_shadowing_if() {
	std::cout << "Testing variable shadowing in if blocks..." << std::endl;

	std::string source = R"(
func test():
	var x = 10
	if 1:
		var x = 20
		return x
	return x
)";

	// Inner x shadows outer x, should return 20
	assert(execute_int(source, "test") == 20);

	std::string source2 = R"(
func test():
	var x = 10
	if 0:
		var x = 20
		return x
	return x
)";

	// Condition is false, should return outer x = 10
	assert(execute_int(source2, "test") == 10);

	std::cout << "  ✓ Variable shadowing in if blocks passed" << std::endl;
}

void test_variable_shadowing_while() {
	std::cout << "Testing variable shadowing in while loops..." << std::endl;

	std::string source = R"(
func test():
	var x = 5
	var count = 0
	while count < 3:
		var x = 100
		count = count + 1
	return x
)";

	// Inner x doesn't affect outer x, should return 5
	assert(execute_int(source, "test") == 5);

	std::cout << "  ✓ Variable shadowing in while loops passed" << std::endl;
}

void test_nested_scopes() {
	std::cout << "Testing nested scopes..." << std::endl;

	std::string source = R"(
func test():
	var a = 1
	if 1:
		var b = 2
		if 1:
			var c = 3
			return a + b + c
	return 0
)";

	// a, b, c all accessible in innermost scope
	assert(execute_int(source, "test") == 6);

	std::cout << "  ✓ Nested scopes passed" << std::endl;
}

void test_scope_isolation() {
	std::cout << "Testing scope isolation..." << std::endl;

	std::string source = R"(
func test():
	var result = 0
	if 1:
		var temp = 10
		result = temp
	if 1:
		var temp = 20
		result = result + temp
	return result
)";

	// Each 'temp' is isolated to its own if block
	assert(execute_int(source, "test") == 30);

	std::cout << "  ✓ Scope isolation passed" << std::endl;
}

void test_while_loop_scope_isolation() {
	std::cout << "Testing while loop scope isolation..." << std::endl;

	std::string source = R"(
func test():
	var sum = 0
	var i = 0
	while i < 3:
		var temp = i * 10
		sum = sum + temp
		i = i + 1
	return sum
)";

	// temp is created fresh each iteration
	assert(execute_int(source, "test") == 30); // 0 + 10 + 20

	std::cout << "  ✓ While loop scope isolation passed" << std::endl;
}

void test_complex_shadowing() {
	std::cout << "Testing complex variable shadowing..." << std::endl;

	std::string source = R"(
func test():
	var x = 1
	var y = 0
	if 1:
		var x = 2
		y = x
		if 1:
			var x = 3
			y = y + x
	y = y + x
	return y
)";

	// y starts at 0
	// First if: x=2, y=2
	// Nested if: x=3, y=2+3=5
	// After blocks: x=1 (outer), y=5+1=6
	assert(execute_int(source, "test") == 6);

	std::cout << "  ✓ Complex shadowing passed" << std::endl;
}

void test_for_loop_range1() {
	std::cout << "Testing for loop with range(n)..." << std::endl;

	std::string source = R"(
func test():
	var sum = 0
	for i in range(5):
		sum = sum + i
	return sum
)";

	// 0 + 1 + 2 + 3 + 4 = 10
	assert(execute_int(source, "test") == 10);

	std::cout << "  ✓ For loop with range(n) passed" << std::endl;
}

void test_for_loop_range2() {
	std::cout << "Testing for loop with range(start, end)..." << std::endl;

	std::string source = R"(
func test():
	var sum = 0
	for i in range(2, 7):
		sum = sum + i
	return sum
)";

	// 2 + 3 + 4 + 5 + 6 = 20
	assert(execute_int(source, "test") == 20);

	std::cout << "  ✓ For loop with range(start, end) passed" << std::endl;
}

void test_for_loop_range3() {
	std::cout << "Testing for loop with range(start, end, step)..." << std::endl;

	std::string source = R"(
func test():
	var sum = 0
	for i in range(0, 10, 2):
		sum = sum + i
	return sum
)";

	// 0 + 2 + 4 + 6 + 8 = 20
	assert(execute_int(source, "test") == 20);

	std::cout << "  ✓ For loop with range(start, end, step) passed" << std::endl;
}

void test_for_loop_nested() {
	std::cout << "Testing nested for loops..." << std::endl;

	std::string source = R"(
func test():
	var sum = 0
	for i in range(3):
		for j in range(3):
			sum = sum + i * j
	return sum
)";

	// i=0: 0*0 + 0*1 + 0*2 = 0
	// i=1: 1*0 + 1*1 + 1*2 = 3
	// i=2: 2*0 + 2*1 + 2*2 = 6
	// Total: 9
	assert(execute_int(source, "test") == 9);

	std::cout << "  ✓ Nested for loops passed" << std::endl;
}

void test_for_loop_with_break() {
	std::cout << "Testing for loop with break..." << std::endl;

	std::string source = R"(
func test():
	var sum = 0
	for i in range(10):
		if i == 5:
			break
		sum = sum + i
	return sum
)";

	// 0 + 1 + 2 + 3 + 4 = 10
	assert(execute_int(source, "test") == 10);

	std::cout << "  ✓ For loop with break passed" << std::endl;
}

void test_for_loop_with_continue() {
	std::cout << "Testing for loop with continue..." << std::endl;

	std::string source = R"(
func test():
	var sum = 0
	for i in range(10):
		if i % 2 == 0:
			continue
		sum = sum + i
	return sum
)";

	// 1 + 3 + 5 + 7 + 9 = 25
	assert(execute_int(source, "test") == 25);

	std::cout << "  ✓ For loop with continue passed" << std::endl;
}

void test_elf_generation() {
	std::cout << "Testing ELF generation..." << std::endl;

	std::string source = R"(
func test():
	return 42

func sum_to_n(n):
	var sum = 0
	var i = 1
	while i <= n:
		sum = sum + i
		i = i + 1
	return sum

func count_down(n):
	var count = 0
	while n > 0:
		count = count + 1
		n = n - 1
	return count

func factorial(n):
	if n <= 1:
		return 1
	var result = 1
	var i = 2
	while i <= n:
		result = result * i
		i = i + 1
	return result
)";

	Compiler compiler;
	CompilerOptions options;

	// Compile to ELF
	auto elf_data = compiler.compile(source, options);
	if (elf_data.empty()) {
		throw std::runtime_error("ELF generation failed: " + compiler.get_error());
	}

	// Write to file
	std::ofstream out("/tmp/test.elf", std::ios::binary);
	out.write(reinterpret_cast<const char*>(elf_data.data()), elf_data.size());
	out.close();

	std::cout << "  ✓ Generated ELF file: /tmp/test.elf (" << elf_data.size() << " bytes)" << std::endl;
	std::cout << "  ✓ Functions: test, sum_to_n, count_down, factorial" << std::endl;
	std::cout << "  ℹ Run: readelf -h -S -s /tmp/test.elf" << std::endl;
}

// ===== ENHANCED TESTS FOR COMPREHENSIVE COMPILER VALIDATION =====

void test_arithmetic_edge_cases() {
	std::cout << "Testing arithmetic edge cases..." << std::endl;

	std::string source = R"(
func test_zero_operations():
	var a = 0
	var b = 5
	return a + b

func test_negative_numbers():
	var a = -10
	var b = 5
	return a + b

func test_multiply_by_zero():
	var a = 42
	var b = 0
	return a * b

func test_multiple_operations():
	var a = 10
	var b = 5
	var c = 2
	var d = a + b - c
	return d * 2

func test_division_rounding():
	var a = 7
	var b = 2
	return a / b
)";

	assert(execute_int(source, "test_zero_operations") == 5);
	assert(execute_int(source, "test_negative_numbers") == -5);
	assert(execute_int(source, "test_multiply_by_zero") == 0);
	assert(execute_int(source, "test_multiple_operations") == 26); // (10+5-2)*2 = 26
	assert(execute_int(source, "test_division_rounding") == 3);

	std::cout << "  ✓ Arithmetic edge cases passed" << std::endl;
}

void test_while_loop_edge_cases() {
	std::cout << "Testing while loop edge cases..." << std::endl;

	std::string source = R"(
func test_zero_iterations():
	var sum = 0
	var i = 10
	while i < 5:
		sum = sum + 1
	return sum

func test_single_iteration():
	var count = 0
	var i = 0
	while i < 1:
		count = count + 1
		i = i + 1
	return count

func test_countdown_to_zero():
	var result = 0
	var i = 5
	while i > 0:
		result = result + i
		i = i - 1
	return result

func test_early_exit_with_break():
	var sum = 0
	var i = 0
	while i < 100:
		if i >= 5:
			break
		sum = sum + i
		i = i + 1
	return sum

func test_skip_with_continue():
	var sum = 0
	var i = 0
	while i < 10:
		i = i + 1
		if i % 2 == 0:
			continue
		sum = sum + i
	return sum
)";

	assert(execute_int(source, "test_zero_iterations") == 0);
	assert(execute_int(source, "test_single_iteration") == 1);
	assert(execute_int(source, "test_countdown_to_zero") == 15); // 5+4+3+2+1
	assert(execute_int(source, "test_early_exit_with_break") == 10); // 0+1+2+3+4
	assert(execute_int(source, "test_skip_with_continue") == 25); // 1+3+5+7+9

	std::cout << "  ✓ While loop edge cases passed" << std::endl;
}

void test_nested_while_loops() {
	std::cout << "Testing nested while loops..." << std::endl;

	std::string source = R"(
func test_triangle_sum():
	var total = 0
	var i = 1
	while i <= 4:
		var j = 1
		while j <= i:
			total = total + 1
			j = j + 1
		i = i + 1
	return total

func test_multiplication_table():
	var sum = 0
	var i = 1
	while i <= 5:
		var j = 1
		while j <= 5:
			sum = sum + i * j
			j = j + 1
		i = i + 1
	return sum
)";

	assert(execute_int(source, "test_triangle_sum") == 10); // 1+2+3+4
	assert(execute_int(source, "test_multiplication_table") == 225); // sum of 5x5 multiplication table

	std::cout << "  ✓ Nested while loops passed" << std::endl;
}

void test_for_loop_edge_cases() {
	std::cout << "Testing for loop edge cases..." << std::endl;

	std::string source = R"(
func test_empty_range():
	var sum = 0
	for i in range(0):
		sum = sum + 1
	return sum

func test_negative_step():
	var sum = 0
	for i in range(10, 0, -1):
		sum = sum + i
	return sum

func test_large_step():
	var sum = 0
	for i in range(0, 20, 5):
		sum = sum + i
	return sum
)";

	assert(execute_int(source, "test_empty_range") == 0);
	assert(execute_int(source, "test_negative_step") == 55); // 10+9+8+...+1
	assert(execute_int(source, "test_large_step") == 30); // 0+5+10+15

	std::cout << "  ✓ For loop edge cases passed" << std::endl;
}

void test_for_loop_variable_assignment() {
	std::cout << "Testing for loop with variable assignment..." << std::endl;

	std::string source = R"(
func test():
	var last = -1
	for i in range(5):
		last = i
	return last
)";

	// Debug: compile with IR dump
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	std::cout << "Generated IR for test():" << std::endl;
	std::cout << "Opcodes: LOAD_IMM=0, MOVE=5, ADD=6, CMP_LT=11, LABEL=18, JUMP=19, BRANCH_ZERO=20" << std::endl;
	for (const auto& func : ir.functions) {
		if (func.name == "test") {
			std::cout << "Function has " << func.max_registers << " max registers" << std::endl;
			for (size_t i = 0; i < func.instructions.size(); i++) {
				const auto& instr = func.instructions[i];
				std::cout << "  " << i << ": opcode=" << static_cast<int>(instr.opcode);
				for (size_t j = 0; j < instr.operands.size(); j++) {
					const auto& op = instr.operands[j];
					std::cout << " op" << j << "=";
					if (std::holds_alternative<int>(op.value)) {
						std::cout << "r" << std::get<int>(op.value);
					} else if (std::holds_alternative<int64_t>(op.value)) {
						std::cout << "#" << std::get<int64_t>(op.value);
					} else if (std::holds_alternative<std::string>(op.value)) {
						std::cout << "\"" << std::get<std::string>(op.value) << "\"";
					}
				}
				std::cout << std::endl;
			}
			break;
		}
	}

	// Try to execute with the fix for map iterator invalidation
	std::cout << "  Attempting execution..." << std::endl;
	assert(execute_int(source, "test") == 4);
	std::cout << "  ✓ For loop with variable assignment passed!" << std::endl;
}

void test_for_loop_new_variable() {
	std::cout << "Testing for loop with new variable declaration in body..." << std::endl;

	std::string source = R"(
func test():
	var unused = 42
	for i in range(50):
		var nvar = i
	return unused
)";

	// Compile and execute
	assert(execute_int(source, "test") == 42);
	std::cout << "  ✓ For loop with new variable in body passed!" << std::endl;
}

void test_function_calls_with_multiple_args() {
	std::cout << "Testing function calls with multiple arguments..." << std::endl;

	// Test basic functions with multiple arguments (no inter-function calls)
	std::string source_basic = R"(
func add_three(a, b, c):
	return a + b + c

func multiply_and_add(a, b, c):
	return a * b + c

func complex_calculation(w, x, y, z):
	var temp1 = w + x
	var temp2 = y * z
	return temp1 - temp2
)";

	assert(execute_int(source_basic, "add_three", {int64_t(1), int64_t(2), int64_t(3)}) == 6);
	assert(execute_int(source_basic, "multiply_and_add", {int64_t(3), int64_t(4), int64_t(5)}) == 17);
	assert(execute_int(source_basic, "complex_calculation", {int64_t(20), int64_t(10), int64_t(3), int64_t(5)}) == 15); // 30-15

	std::cout << "  ✓ Function calls with multiple arguments passed" << std::endl;
	std::cout << "  ℹ Note: Inter-function calls not fully implemented (CALL opcode)" << std::endl;
}

void test_conditional_complexity() {
	std::cout << "Testing complex conditional logic..." << std::endl;

	std::string source = R"(
func nested_if(x):
	if x > 10:
		if x > 20:
			if x > 30:
				return 3
			return 2
		return 1
	return 0

func multiple_elif(x):
	if x < 0:
		return -1
	elif x == 0:
		return 0
	elif x < 10:
		return 1
	elif x < 100:
		return 2
	else:
		return 3

func combined_conditions(a, b):
	if a > 0 and b > 0:
		return 1
	elif a < 0 and b < 0:
		return -1
	else:
		return 0
)";

	assert(execute_int(source, "nested_if", {int64_t(5)}) == 0);
	assert(execute_int(source, "nested_if", {int64_t(15)}) == 1);
	assert(execute_int(source, "nested_if", {int64_t(25)}) == 2);
	assert(execute_int(source, "nested_if", {int64_t(35)}) == 3);

	assert(execute_int(source, "multiple_elif", {int64_t(-5)}) == -1);
	assert(execute_int(source, "multiple_elif", {int64_t(0)}) == 0);
	assert(execute_int(source, "multiple_elif", {int64_t(5)}) == 1);
	assert(execute_int(source, "multiple_elif", {int64_t(50)}) == 2);
	assert(execute_int(source, "multiple_elif", {int64_t(150)}) == 3);

	assert(execute_int(source, "combined_conditions", {int64_t(5), int64_t(10)}) == 1);
	assert(execute_int(source, "combined_conditions", {int64_t(-5), int64_t(-10)}) == -1);
	assert(execute_int(source, "combined_conditions", {int64_t(5), int64_t(-10)}) == 0);

	std::cout << "  ✓ Complex conditional logic passed" << std::endl;
}

void test_real_world_algorithms() {
	std::cout << "Testing real-world algorithms..." << std::endl;

	std::string source = R"(
func power(base, exp):
	if exp == 0:
		return 1
	var result = base
	var i = 1
	while i < exp:
		result = result * base
		i = i + 1
	return result

func sum_of_squares(n):
	var sum = 0
	var i = 1
	while i <= n:
		sum = sum + i * i
		i = i + 1
	return sum

func collatz_steps(n):
	var steps = 0
	while n != 1:
		if n % 2 == 0:
			n = n / 2
		else:
			n = 3 * n + 1
		steps = steps + 1
	return steps

func digit_sum(n):
	var sum = 0
	while n > 0:
		sum = sum + n % 10
		n = n / 10
	return sum
)";

	assert(execute_int(source, "power", {int64_t(2), int64_t(3)}) == 8);
	assert(execute_int(source, "power", {int64_t(5), int64_t(2)}) == 25);
	assert(execute_int(source, "power", {int64_t(10), int64_t(0)}) == 1);

	assert(execute_int(source, "sum_of_squares", {int64_t(3)}) == 14); // 1+4+9
	assert(execute_int(source, "sum_of_squares", {int64_t(5)}) == 55); // 1+4+9+16+25

	assert(execute_int(source, "collatz_steps", {int64_t(1)}) == 0);
	assert(execute_int(source, "collatz_steps", {int64_t(2)}) == 1);
	assert(execute_int(source, "collatz_steps", {int64_t(16)}) == 4);

	assert(execute_int(source, "digit_sum", {int64_t(123)}) == 6);
	assert(execute_int(source, "digit_sum", {int64_t(999)}) == 27);

	std::cout << "  ✓ Real-world algorithms passed" << std::endl;
}

void test_loop_counter_variations() {
	std::cout << "Testing loop counter variations..." << std::endl;

	std::string source = R"(
func count_up_by_two():
	var sum = 0
	var i = 0
	while i < 10:
		sum = sum + i
		i = i + 2
	return sum

func count_down():
	var result = 0
	var i = 10
	while i > 0:
		result = result + i
		i = i - 1
	return result

func exponential_growth():
	var count = 0
	var i = 1
	while i < 100:
		count = count + 1
		i = i * 2
	return count
)";

	assert(execute_int(source, "count_up_by_two") == 20); // 0+2+4+6+8
	assert(execute_int(source, "count_down") == 55); // 10+9+...+1
	assert(execute_int(source, "exponential_growth") == 7); // 1,2,4,8,16,32,64 (stops before 128)

	std::cout << "  ✓ Loop counter variations passed" << std::endl;
}

void test_local_function_calls() {
	std::cout << "Testing local function calls..." << std::endl;

	// Test simple function calling another function
	std::string source_simple = R"(
func add(a, b):
	return a + b

func double_add(x, y):
	var result = add(x, y)
	return result * 2
)";

	assert(execute_int(source_simple, "add", {int64_t(3), int64_t(4)}) == 7);
	assert(execute_int(source_simple, "double_add", {int64_t(3), int64_t(4)}) == 14);

	// Test chained function calls
	std::string source_chain = R"(
func triple(x):
	return x * 3

func add_five(x):
	return x + 5

func process(x):
	var step1 = triple(x)
	var step2 = add_five(step1)
	return step2
)";

	assert(execute_int(source_chain, "process", {int64_t(4)}) == 17); // 4*3+5 = 17

	// Test multiple calls to same function
	std::string source_multiple = R"(
func square(x):
	return x * x

func sum_of_squares(a, b):
	return square(a) + square(b)
)";

	assert(execute_int(source_multiple, "sum_of_squares", {int64_t(3), int64_t(4)}) == 25); // 9+16

	// Test nested calls
	std::string source_nested = R"(
func add(a, b):
	return a + b

func multiply(a, b):
	return a * b

func complex(x, y):
	return add(multiply(x, 2), multiply(y, 3))
)";

	assert(execute_int(source_nested, "complex", {int64_t(5), int64_t(2)}) == 16); // 5*2 + 2*3 = 16

	std::cout << "  ✓ Local function calls passed" << std::endl;
}

void test_recursive_calls() {
	std::cout << "Testing recursive function calls..." << std::endl;

	// Test simple recursion - factorial
	std::string source_factorial = R"(
func factorial(n):
	if n <= 1:
		return 1
	return n * factorial(n - 1)
)";

	assert(execute_int(source_factorial, "factorial", {int64_t(5)}) == 120);
	assert(execute_int(source_factorial, "factorial", {int64_t(1)}) == 1);
	assert(execute_int(source_factorial, "factorial", {int64_t(6)}) == 720);

	// Test fibonacci with recursion
	std::string source_fib = R"(
func fib(n):
	if n <= 1:
		return n
	return fib(n - 1) + fib(n - 2)
)";

	assert(execute_int(source_fib, "fib", {int64_t(0)}) == 0);
	assert(execute_int(source_fib, "fib", {int64_t(1)}) == 1);
	assert(execute_int(source_fib, "fib", {int64_t(6)}) == 8); // 0,1,1,2,3,5,8

	std::cout << "  ✓ Recursive function calls passed" << std::endl;
}

int main() {
	std::cout << "\n=== Running Integration Tests ===" << std::endl;
	std::cout << "These tests compile GDScript and execute it via IR interpreter\n" << std::endl;

	try {
		// Basic functionality tests
		test_simple_return();
		test_arithmetic();
		test_variables();
		test_if_statement();
		test_while_loop();
		test_fibonacci_iterative();
		test_fibonacci();  // Will be skipped for now
		test_comparison_operators();
		test_logical_operators();
		test_complex_expression();
		test_nested_loops();
		test_prime_number();
		test_gcd();

		// Scoping tests
		test_variable_shadowing_if();
		test_variable_shadowing_while();
		test_nested_scopes();
		test_scope_isolation();
		test_while_loop_scope_isolation();
		test_complex_shadowing();

		// For loop tests
		test_for_loop_range1();
		test_for_loop_range2();
		test_for_loop_range3();
		test_for_loop_nested();
		test_for_loop_with_break();
		test_for_loop_with_continue();
		test_for_loop_variable_assignment();
		test_for_loop_new_variable();

		std::cout << "\n=== Enhanced Comprehensive Tests ===" << std::endl;

		// Enhanced edge case and comprehensive tests
		test_arithmetic_edge_cases();
		test_while_loop_edge_cases();
		test_nested_while_loops();
		test_for_loop_edge_cases();
		test_function_calls_with_multiple_args();
		test_conditional_complexity();
		test_real_world_algorithms();
		test_loop_counter_variations();

		std::cout << "\n=== Function Call Tests ===" << std::endl;
		test_local_function_calls();
		test_recursive_calls();

		// ELF generation test
		test_elf_generation();

		std::cout << "\n✅ All integration tests passed!" << std::endl;
		std::cout << "✅ Basic features: arithmetic, variables, conditionals" << std::endl;
		std::cout << "✅ Control flow: while loops, for loops, break, continue" << std::endl;
		std::cout << "✅ Variable scoping with shadowing works correctly" << std::endl;
		std::cout << "✅ Real-world algorithms: Fibonacci, GCD, Prime check, Collatz, Power" << std::endl;
		std::cout << "✅ Edge cases: zero iterations, countdown, nested loops" << std::endl;
		std::cout << "✅ Function calls with multiple arguments" << std::endl;
		std::cout << "✅ Complex conditional logic with nested if/elif/else" << std::endl;
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}
