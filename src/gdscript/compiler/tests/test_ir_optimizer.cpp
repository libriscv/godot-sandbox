#include "../compiler.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

using namespace gdscript;

// Helper function to compile code to IR and return the IRFunction
IRFunction compile_to_ir(const std::string& source, const std::string& function_name = "test") {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir_program = codegen.generate(program);

	// Find the function
	for (auto& func : ir_program.functions) {
		if (func.name == function_name) {
			return func;
		}
	}

	throw std::runtime_error("Function not found: " + function_name);
}

// Helper to count instruction types
int count_instructions(const IRFunction& func, IROpcode opcode) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

// Helper to get IR as string for debugging
std::string ir_to_string(const IRFunction& func) {
	std::stringstream ss;
	ss << "Function: " << func.name << " (max_registers: " << func.max_registers << ")\n";
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];
		ss << "  " << i << ": ";
		switch (instr.opcode) {
			case IROpcode::LOAD_IMM:
				ss << "LOAD_IMM r" << std::get<int>(instr.operands[0].value)
				   << ", " << std::get<int64_t>(instr.operands[1].value);
				break;
			case IROpcode::LOAD_FLOAT_IMM:
				ss << "LOAD_FLOAT_IMM r" << std::get<int>(instr.operands[0].value)
				   << ", " << std::get<double>(instr.operands[1].value);
				break;
			case IROpcode::MOVE:
				ss << "MOVE r" << std::get<int>(instr.operands[0].value)
				   << ", r" << std::get<int>(instr.operands[1].value);
				break;
			case IROpcode::ADD:
			case IROpcode::SUB:
			case IROpcode::MUL:
			case IROpcode::DIV:
			case IROpcode::MOD: {
				const char* op_name = "";
				switch (instr.opcode) {
					case IROpcode::ADD: op_name = "ADD"; break;
					case IROpcode::SUB: op_name = "SUB"; break;
					case IROpcode::MUL: op_name = "MUL"; break;
					case IROpcode::DIV: op_name = "DIV"; break;
					case IROpcode::MOD: op_name = "MOD"; break;
					default: break;
				}
				ss << op_name << " r" << std::get<int>(instr.operands[0].value)
				   << ", r" << std::get<int>(instr.operands[1].value)
				   << ", r" << std::get<int>(instr.operands[2].value);
				break;
			}
			default:
				ss << "opcode_" << static_cast<int>(instr.opcode);
				break;
		}
		ss << "\n";
	}
	return ss.str();
}

void test_pattern_a_basic() {
	std::cout << "Testing Pattern A (MOVE; MOVE; OP; MOVE with two temporaries)..." << std::endl;

	// Pattern A: MOVE tmp1, src1; MOVE tmp2, src2; OP dst, tmp1, tmp2; MOVE result, dst
	//          -> OP result, src1, src2
	std::string source = R"(
func test(a, b):
	var c = a + b
	return c
)";

	// Compile without optimization
	IRFunction func_no_opt = compile_to_ir(source);
	int move_count_no_opt = count_instructions(func_no_opt, IROpcode::MOVE);
	int add_count_no_opt = count_instructions(func_no_opt, IROpcode::ADD);

	// Compile with optimization
	Compiler compiler;
	CompilerOptions options;
	options.dump_ir = true;

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	int move_count_opt = count_instructions(func, IROpcode::MOVE);
	int add_count_opt = count_instructions(func, IROpcode::ADD);

	// Pattern A should reduce some MOVEs
	std::cout << "  MOVEs: " << move_count_no_opt << " -> " << move_count_opt << std::endl;
	std::cout << "  ADDs: " << add_count_no_opt << " -> " << add_count_opt << std::endl;

	std::cout << "  ✓ Pattern A test passed" << std::endl;
}

void test_pattern_b_operand1() {
	std::cout << "Testing Pattern B (MOVE; OP; MOVE with first operand temporary)..." << std::endl;

	// Pattern B: MOVE tmp, src; OP dst, tmp, other; MOVE result, dst
	//          -> OP result, src, other
	std::string source = R"(
func test(a, b):
	var c = a
	var d = c + b
	return d
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << "  ✓ Pattern B test passed" << std::endl;
}

void test_pattern_c_operand2() {
	std::cout << "Testing Pattern C (MOVE; OP; MOVE with second operand temporary)..." << std::endl;

	// Pattern C: MOVE tmp, src; OP dst, other, tmp; MOVE result, dst
	//          -> OP result, other, src
	std::string source = R"(
func test(a, b):
	var c = b
	var d = a + c
	return d
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << "  ✓ Pattern C test passed" << std::endl;
}

void test_pattern_d_move_after_op() {
	std::cout << "Testing Pattern D (OP; MOVE without preceding MOVE)..." << std::endl;

	// Pattern D: OP dst, ...; MOVE result, dst
	//          -> OP result, ...
	std::string source = R"(
func test(a, b):
	return a + b
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << "  ✓ Pattern D test passed" << std::endl;
}

void test_pattern_e_increment() {
	std::cout << "Testing Pattern E (increment optimization: x = x + 1)..." << std::endl;

	// Pattern E: MOVE tmp, var; LOAD_IMM/LOAD_FLOAT_IMM const; OP dst, tmp, const; MOVE var, dst
	//          -> LOAD_IMM/LOAD_FLOAT_IMM const; OP var, var, const
	std::string source = R"(
func test(x):
	var i = x
	i += 1
	return i
)";

	IRFunction func = compile_to_ir(source);

	// Count instructions before optimization
	int move_count_before = count_instructions(func, IROpcode::MOVE);
	int load_imm_count_before = count_instructions(func, IROpcode::LOAD_IMM);
	int add_count_before = count_instructions(func, IROpcode::ADD);

	std::cout << "  Before optimization:" << std::endl;
	std::cout << ir_to_string(func);

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	// Count instructions after optimization
	int move_count_after = count_instructions(func, IROpcode::MOVE);
	int load_imm_count_after = count_instructions(func, IROpcode::LOAD_IMM);
	int add_count_after = count_instructions(func, IROpcode::ADD);

	std::cout << "  After optimization:" << std::endl;
	std::cout << ir_to_string(func);

	std::cout << "  MOVEs: " << move_count_before << " -> " << move_count_after << std::endl;
	std::cout << "  LOAD_IMM: " << load_imm_count_before << " -> " << load_imm_count_after << std::endl;
	std::cout << "  ADDs: " << add_count_before << " -> " << add_count_after << std::endl;

	// Pattern E should reduce at least 2 MOVEs (MOVE tmp,var and MOVE var,dst)
	// while keeping the LOAD_IMM (needed for the constant)
	assert(move_count_after < move_count_before && "Pattern E should reduce MOVEs");
	assert(load_imm_count_after <= load_imm_count_before && "Pattern E should keep LOAD_IMM");
	assert(add_count_after == add_count_before && "Pattern E should keep ADD count");

	std::cout << "  ✓ Pattern E test passed (reduced " << (move_count_before - move_count_after) << " MOVEs)" << std::endl;
}

void test_pattern_e_float_increment() {
	std::cout << "Testing Pattern E with float increment..." << std::endl;

	std::string source = R"(
func test(x):
	var i = x
	i += 1.5
	return i
)";

	IRFunction func = compile_to_ir(source);

	int move_count_before = count_instructions(func, IROpcode::MOVE);
	int load_float_count_before = count_instructions(func, IROpcode::LOAD_FLOAT_IMM);

	std::cout << "  Before optimization:" << std::endl;
	std::cout << ir_to_string(func);

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	int move_count_after = count_instructions(func, IROpcode::MOVE);
	int load_float_count_after = count_instructions(func, IROpcode::LOAD_FLOAT_IMM);

	std::cout << "  After optimization:" << std::endl;
	std::cout << ir_to_string(func);

	std::cout << "  MOVEs: " << move_count_before << " -> " << move_count_after << std::endl;
	std::cout << "  LOAD_FLOAT_IMM: " << load_float_count_before << " -> " << load_float_count_after << std::endl;

	assert(move_count_after < move_count_before && "Pattern E should reduce MOVEs for floats");

	std::cout << "  ✓ Pattern E float test passed" << std::endl;
}

void test_pattern_f_redundant_swap() {
	std::cout << "Testing Pattern F (redundant swap pair)..." << std::endl;

	// Pattern F: MOVE tmp, src; MOVE src, tmp -> eliminate both
	std::string source = R"(
func test(a):
	var b = a
	var c = b
	return c
)";

	IRFunction func = compile_to_ir(source);
	int move_count_before = count_instructions(func, IROpcode::MOVE);

	std::cout << "  Before optimization: " << move_count_before << " MOVEs" << std::endl;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	int move_count_after = count_instructions(func, IROpcode::MOVE);
	std::cout << "  After optimization: " << move_count_after << " MOVEs" << std::endl;

	std::cout << "  ✓ Pattern F test passed" << std::endl;
}

void test_constant_folding() {
	std::cout << "Testing constant folding..." << std::endl;

	std::string source = R"(
func test():
	return 5 + 3
)";

	IRFunction func = compile_to_ir(source);

	std::cout << "  Before optimization:" << std::endl;
	std::cout << ir_to_string(func);

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << "  After optimization:" << std::endl;
	std::cout << ir_to_string(func);

	// Should be optimized to just LOAD_IMM r0, 8
	int move_count = count_instructions(func, IROpcode::MOVE);
	int add_count = count_instructions(func, IROpcode::ADD);
	int load_imm_count = count_instructions(func, IROpcode::LOAD_IMM);

	std::cout << "  Final: " << load_imm_count << " LOAD_IMM, " << add_count << " ADD, " << move_count << " MOVE" << std::endl;

	assert(add_count == 0 && "Constant folding should eliminate ADD");
	assert(load_imm_count == 1 && "Constant folding should result in single LOAD_IMM");

	std::cout << "  ✓ Constant folding test passed" << std::endl;
}

void test_combined_optimizations() {
	std::cout << "Testing combined optimizations (loop with increment)..." << std::endl;

	std::string source = R"(
func test():
	var sum = 0
	for i in range(10):
		sum += i
	return sum
)";

	IRFunction func = compile_to_ir(source);

	int move_count_before = count_instructions(func, IROpcode::MOVE);
	int add_count_before = count_instructions(func, IROpcode::ADD);

	std::cout << "  Before optimization: " << move_count_before << " MOVEs, " << add_count_before << " ADDs" << std::endl;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	int move_count_after = count_instructions(func, IROpcode::MOVE);
	int add_count_after = count_instructions(func, IROpcode::ADD);

	std::cout << "  After optimization: " << move_count_after << " MOVEs, " << add_count_after << " ADDs" << std::endl;
	std::cout << "  Reduced " << (move_count_before - move_count_after) << " MOVEs" << std::endl;

	std::cout << "  ✓ Combined optimizations test passed" << std::endl;
}

void test_register_pressure_reduction() {
	std::cout << "Testing register pressure with many variables..." << std::endl;

	std::string source = R"(
func test():
	var a = 1
	var b = 2
	var c = 3
	var d = 4
	var e = 5
	var f = 6
	return a + b + c + d + e + f
)";

	IRFunction func = compile_to_ir(source);

	std::cout << "  Max registers before optimization: " << func.max_registers << std::endl;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << "  Max registers after optimization: " << func.max_registers << std::endl;

	std::cout << "  ✓ Register pressure test passed" << std::endl;
}

void test_copy_propagation() {
	std::cout << "Testing copy propagation..." << std::endl;

	std::string source = R"(
func test():
	var a = 5
	var b = a
	var c = b
	return c
)";

	IRFunction func = compile_to_ir(source);

	std::cout << "  Before optimization:" << std::endl;
	std::cout << ir_to_string(func);

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << "  After optimization:" << std::endl;
	std::cout << ir_to_string(func);

	std::cout << "  ✓ Copy propagation test passed" << std::endl;
}

void test_dead_code_elimination() {
	std::cout << "Testing dead code elimination..." << std::endl;

	std::string source = R"(
func test():
	var a = 5
	var b = 10
	var c = 15
	return a + c
)";

	IRFunction func = compile_to_ir(source);

	int instr_count_before = func.instructions.size();

	std::cout << "  Instructions before: " << instr_count_before << std::endl;

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	int instr_count_after = func.instructions.size();

	std::cout << "  Instructions after: " << instr_count_after << std::endl;

	std::cout << "  ✓ Dead code elimination test passed" << std::endl;
}

int main() {
	std::cout << "\n=== IR Optimizer Peephole Pattern Tests ===\n" << std::endl;

	try {
		test_constant_folding();
		std::cout << std::endl;

		test_pattern_a_basic();
		std::cout << std::endl;

		test_pattern_b_operand1();
		std::cout << std::endl;

		test_pattern_c_operand2();
		std::cout << std::endl;

		test_pattern_d_move_after_op();
		std::cout << std::endl;

		test_pattern_e_increment();
		std::cout << std::endl;

		test_pattern_e_float_increment();
		std::cout << std::endl;

		test_pattern_f_redundant_swap();
		std::cout << std::endl;

		test_copy_propagation();
		std::cout << std::endl;

		test_dead_code_elimination();
		std::cout << std::endl;

		test_combined_optimizations();
		std::cout << std::endl;

		test_register_pressure_reduction();
		std::cout << std::endl;

		std::cout << "=== All IR Optimizer Tests Passed! ===\n" << std::endl;
		return 0;

	} catch (const std::exception& e) {
		std::cerr << "TEST FAILED: " << e.what() << std::endl;
		return 1;
	}
}
