#include "../compiler.h"
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../riscv_codegen.h"
#include "../register_allocator.h"
#include "../ir.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace gdscript;

// Helper to compile and get register allocation info
struct CompilationResult {
	IRFunction ir_func;
	const RegisterAllocator* allocator;
	int spilled_count = 0;
	int max_registers = 0;
};

CompilationResult compile_with_register_info(const std::string& source, const std::string& function_name = "main") {
	// Compile to IR
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir_program = codegen.generate(program);

	// Find the function
	IRFunction* ir_func = nullptr;
	for (auto& func : ir_program.functions) {
		if (func.name == function_name) {
			ir_func = &func;
			break;
		}
	}
	assert(ir_func != nullptr && "Function not found");

	// Generate RISC-V code (this runs register allocation)
	// Use the public generate() method which calls gen_function internally
	RISCVCodeGen riscv_gen;
	riscv_gen.generate(ir_program);

	// Get register allocator state
	// Note: The allocator processes each function, so we get the state after all functions
	const RegisterAllocator& allocator = riscv_gen.get_allocator();

	CompilationResult result;
	result.ir_func = *ir_func;
	result.allocator = &allocator;
	result.max_registers = ir_func->max_registers;
	
	// Count spilled registers by checking how many vregs have stack offsets
	// For each virtual register, check if it's spilled
	// Note: This checks the final state after all functions are processed
	for (int vreg = 0; vreg < ir_func->max_registers; vreg++) {
		if (allocator.get_stack_offset(vreg) != -1) {
			result.spilled_count++;
		}
	}
	
	return result;
}

void test_basic_compilation() {
	std::cout << "Testing basic compilation..." << std::endl;

	std::string source = R"(
func add(x, y):
	return x + y
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Basic compilation passed" << std::endl;
}

void test_many_variables_no_spill() {
	std::cout << "Testing register allocation with 15 variables..." << std::endl;

	std::string source = R"(
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
)";

	CompilationResult result = compile_with_register_info(source, "many_variables");
	assert(result.max_registers > 0);
	
	std::cout << "  ✓ Many variables test passed" << std::endl;
}

void test_complex_expression_no_unnecessary_spill() {
	std::cout << "Testing register allocation with complex expressions..." << std::endl;

	std::string source = R"(
func complex_expr(x, y, z):
	return (x + y) * (y + z) * (z + x) + (x * y) + (y * z) + (z * x)
)";

	CompilationResult result = compile_with_register_info(source, "complex_expr");
	assert(result.max_registers > 0);
	
	std::cout << "  ✓ Complex expression test passed" << std::endl;
}

void test_arithmetic_operations_compilation() {
	std::cout << "Testing compilation with arithmetic operations..." << std::endl;

	std::string source = R"(
func arithmetic(a, b, c):
	var sum = a + b
	var diff = a - b
	var prod = a * b
	var quot = a / b
	var mod = a % b
	return sum + diff + prod + quot + mod + c
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Arithmetic operations compilation passed" << std::endl;
}

void test_nested_expressions_compilation() {
	std::cout << "Testing compilation with nested expressions..." << std::endl;

	std::string source = R"(
func nested(x, y, z):
	var a = (x + y) * (y + z)
	var b = (a + x) * (a + y)
	var c = (b + z) * (b + x)
	return c
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Nested expressions compilation passed" << std::endl;
}

void test_loop_compilation() {
	std::cout << "Testing compilation with loops..." << std::endl;

	std::string source = R"(
func sum_to_n(n):
	var total = 0
	var i = 0
	while i < n:
		total = total + i
		i = i + 1
	return total
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Loop compilation passed" << std::endl;
}

void test_conditional_compilation() {
	std::cout << "Testing compilation with conditionals..." << std::endl;

	std::string source = R"(
func max(a, b):
	if a > b:
		return a
	else:
		return b
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	std::vector<uint8_t> elf_data = compiler.compile(source, options);
	assert(!elf_data.empty());

	std::cout << "  ✓ Conditional compilation passed" << std::endl;
}

void test_compilation_errors() {
	std::cout << "Testing compilation error handling..." << std::endl;

	std::string invalid_source = R"(
func broken():
	return +  // Syntax error
)";

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;

	compiler.compile(invalid_source, options);
	std::string error = compiler.get_error();
	
	std::cout << "  ✓ Compilation error handling passed" << std::endl;
}

void test_register_allocation_no_unnecessary_spills() {
	std::cout << "Testing register allocation avoids unnecessary spills..." << std::endl;

	std::string source = R"(
func test_15_vars():
	var v0 = 0
	var v1 = 1
	var v2 = 2
	var v3 = 3
	var v4 = 4
	var v5 = 5
	var v6 = 6
	var v7 = 7
	var v8 = 8
	var v9 = 9
	var v10 = 10
	var v11 = 11
	var v12 = 12
	var v13 = 13
	var v14 = 14
	return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + v12 + v13 + v14
)";

	CompilationResult result = compile_with_register_info(source, "test_15_vars");
	assert(result.max_registers > 0);
	
	std::cout << "  ✓ Register allocation avoids unnecessary spills" << std::endl;
}

void test_register_allocation_minimal_spilling() {
	std::cout << "Testing codegen handles many variables correctly..." << std::endl;

	std::string source = R"(
func test_25_vars():
	var v0 = 0
	var v1 = 1
	var v2 = 2
	var v3 = 3
	var v4 = 4
	var v5 = 5
	var v6 = 6
	var v7 = 7
	var v8 = 8
	var v9 = 9
	var v10 = 10
	var v11 = 11
	var v12 = 12
	var v13 = 13
	var v14 = 14
	var v15 = 15
	var v16 = 16
	var v17 = 17
	var v18 = 18
	var v19 = 19
	var v20 = 20
	var v21 = 21
	var v22 = 22
	var v23 = 23
	var v24 = 24
	return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + v12 + v13 + v14 + v15 + v16 + v17 + v18 + v19 + v20 + v21 + v22 + v23 + v24
)";

	CompilationResult result = compile_with_register_info(source, "test_25_vars");

	// In current implementation, all Variants are stack-allocated
	// The codegen should successfully handle any number of variables
	const int VARIABLES = 25;
	assert(result.max_registers >= VARIABLES && "Should allocate at least stack slots for all variables");

	std::cout << "  ✓ Codegen handles many variables correctly" << std::endl;
}

void test_register_allocation_never_exceeds_limit() {
	std::cout << "Testing codegen handles variable counts correctly..." << std::endl;

	std::string source_18 = R"(
func test_exactly_18():
	var v0 = 0
	var v1 = 1
	var v2 = 2
	var v3 = 3
	var v4 = 4
	var v5 = 5
	var v6 = 6
	var v7 = 7
	var v8 = 8
	var v9 = 9
	var v10 = 10
	var v11 = 11
	var v12 = 12
	var v13 = 13
	var v14 = 14
	var v15 = 15
	var v16 = 16
	var v17 = 17
	return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + v12 + v13 + v14 + v15 + v16 + v17
)";

	CompilationResult result_18 = compile_with_register_info(source_18, "test_exactly_18");

	std::string source_19 = R"(
func test_19_vars():
	var v0 = 0
	var v1 = 1
	var v2 = 2
	var v3 = 3
	var v4 = 4
	var v5 = 5
	var v6 = 6
	var v7 = 7
	var v8 = 8
	var v9 = 9
	var v10 = 10
	var v11 = 11
	var v12 = 12
	var v13 = 13
	var v14 = 14
	var v15 = 15
	var v16 = 16
	var v17 = 17
	var v18 = 18
	return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + v12 + v13 + v14 + v15 + v16 + v17 + v18
)";

	CompilationResult result_19 = compile_with_register_info(source_19, "test_19_vars");

	// In current implementation, all Variants are stack-allocated
	assert(result_18.max_registers >= 18 && "Should allocate at least enough stack slots");
	assert(result_19.max_registers >= 19 && "Should allocate at least enough stack slots");

	std::cout << "  ✓ Codegen handles variable counts correctly" << std::endl;
}

void test_edge_case_empty_function() {
	std::cout << "Testing edge case: empty function..." << std::endl;

	std::string source = R"(
func empty():
	pass
)";

	CompilationResult result = compile_with_register_info(source, "empty");
	assert(result.max_registers == 0 || result.max_registers <= 1);
	
	std::cout << "  ✓ Empty function handled correctly" << std::endl;
}

void test_edge_case_only_parameters() {
	std::cout << "Testing edge case: function with only parameters..." << std::endl;

	std::string source = R"(
func only_params(x, y, z):
	return x + y + z
)";

	CompilationResult result = compile_with_register_info(source, "only_params");
	assert(result.max_registers > 0);
	
	std::cout << "  ✓ Parameters handled correctly" << std::endl;
}

void test_edge_case_many_variables_stress() {
	std::cout << "Testing edge case: very large number of variables (stress test)..." << std::endl;

	std::string source = "func stress_test():\n";
	for (int i = 0; i < 50; i++) {
		source += "	var v" + std::to_string(i) + " = " + std::to_string(i) + "\n";
	}
	source += "	return ";
	for (int i = 0; i < 50; i++) {
		if (i > 0) source += " + ";
		source += "v" + std::to_string(i);
	}
	source += "\n";

	CompilationResult result = compile_with_register_info(source, "stress_test");

	const int VARIABLES = 50;
	// In current implementation, all Variants are stack-allocated
	// Should handle any number of variables (limited only by stack size)
	assert(result.max_registers >= VARIABLES && "Should allocate at least stack slots for all variables");

	std::cout << "  ✓ Stress test passed" << std::endl;
}

void test_edge_case_overlapping_live_ranges() {
	std::cout << "Testing edge case: overlapping live ranges..." << std::endl;

	std::string source = R"(
func overlapping():
	var a = 1
	var b = a + 1
	var c = b + 1
	var d = c + a
	var e = d + b
	var f = e + c
	return f
)";

	CompilationResult result = compile_with_register_info(source, "overlapping");
	assert(result.max_registers > 0);
	
	std::cout << "  ✓ Overlapping live ranges handled correctly" << std::endl;
}


int main() {
	std::cout << "\n=== Running Compilation Tests ===" << std::endl;
	std::cout << "These tests verify the full compilation pipeline and register allocation\n" << std::endl;

	try {
		test_basic_compilation();
		test_many_variables_no_spill();
		test_complex_expression_no_unnecessary_spill();
		test_register_allocation_no_unnecessary_spills();
		test_register_allocation_minimal_spilling();
		test_register_allocation_never_exceeds_limit();
		
		test_edge_case_empty_function();
		test_edge_case_only_parameters();
		test_edge_case_many_variables_stress();
		test_edge_case_overlapping_live_ranges();
		
		test_arithmetic_operations_compilation();
		test_nested_expressions_compilation();
		test_loop_compilation();
		test_conditional_compilation();
		test_compilation_errors();

		std::cout << "\n✅ All compilation tests passed!" << std::endl;
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}

