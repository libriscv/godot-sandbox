#include "../compiler.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

IRProgram compile_program(const std::string& source, bool optimize) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
	}
	return ir;
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
				ss << "LOAD_IMM r" << instr.operands[0].reg_index()
				   << ", " << instr.operands[1].immediate();
				break;
			case IROpcode::LOAD_FLOAT_IMM:
				ss << "LOAD_FLOAT_IMM r" << instr.operands[0].reg_index()
				   << ", " << instr.operands[1].float_number();
				break;
			case IROpcode::MOVE:
				ss << "MOVE r" << instr.operands[0].reg_index()
				   << ", r" << instr.operands[1].reg_index();
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
				ss << op_name << " r" << instr.operands[0].reg_index()
				   << ", r" << instr.operands[1].reg_index()
				   << ", r" << instr.operands[2].reg_index();
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

void test_dead_code_elimination_keeps_stored_globals() {
	std::cout << "Testing that dead code elimination keeps globals' source registers..." << std::endl;

	// Regression test: STORE_GLOBAL reads its value from operand 1, but the
	// liveness analysis used to only consider a whitelist of opcodes. Because
	// STORE_GLOBAL was not on it, the LOAD_IMM defining the stored register
	// looked dead and was deleted, leaving the global initialised from an
	// uninitialised register.
	std::string source = R"(
var g = 0
var h = 0
func test():
	g = 5
	h = 7
)";

	IRFunction func = compile_to_ir(source);

	IROptimizer optimizer;
	optimizer.optimize_function(func);

	// Collect every register that a STORE_GLOBAL reads
	std::vector<int> stored_regs;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::STORE_GLOBAL && instr.operands.size() > 1 &&
		    instr.operands[1].type == IRValue::Type::REGISTER) {
			stored_regs.push_back(instr.operands[1].reg_index());
		}
	}
	assert(stored_regs.size() == 2);

	// Each of them must still be defined before it is stored
	for (int reg : stored_regs) {
		bool defined = false;
		for (const auto& instr : func.instructions) {
			if (instr.opcode == IROpcode::STORE_GLOBAL) {
				continue;
			}
			if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER &&
			    instr.operands[0].reg_index() == reg) {
				defined = true;
				break;
			}
		}
		assert(defined && "STORE_GLOBAL reads a register that is never defined");
	}

	std::cout << "  ✓ Dead code elimination keeps stored globals test passed" << std::endl;
}

// A chain of copies has to collapse onto its original source, and the copies
// that nothing reads any more have to go.
//
// Both halves used to fail, and each hid the other. Copy propagation recorded a
// MOVE as a copy and then immediately erased that record when it invalidated
// the register the MOVE had written, so no later instruction ever saw a copy to
// propagate. And dead-code elimination refused to delete any instruction that
// read a register, which a MOVE always does, so the copies left behind by the
// front end stayed to the end. A `return` at the end of a loop reached the
// backend as three Variant copies where one would do.
void test_copy_chains_collapse() {
	std::cout << "Test: copy chains collapse to one move" << std::endl;

	const std::string source = R"(
func test(n):
	var total = 0
	for i in range(n):
		total += i
	return total
)";

	IRFunction unoptimized = compile_to_ir(source);
	IRFunction optimized = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(optimized);

	const int before = count_instructions(unoptimized, IROpcode::MOVE);
	const int after = count_instructions(optimized, IROpcode::MOVE);
	std::cout << "  MOVE instructions: " << before << " -> " << after << std::endl;
	assert(after < before && "the copy chain into the return register survived");

	// Whatever survives has to end with the value reaching the return register,
	// which RETURN reads without naming it.
	bool defines_return_register = false;
	for (const auto& instr : optimized.instructions) {
		if (ir_destination_register(instr) == IRFunction::RETURN_REGISTER) {
			defines_return_register = true;
		}
	}
	assert(defines_return_register && "nothing writes the value that gets returned");

	// The registers the deleted copies used are gone with them.
	std::cout << "  max_registers: " << unoptimized.max_registers
			  << " -> " << optimized.max_registers << std::endl;
	assert(optimized.max_registers < unoptimized.max_registers);

	std::cout << "  PASSED" << std::endl;
}

// Whether any instruction sits between an unconditional transfer of control and
// the next label, which is code nothing can reach.
bool has_unreachable_tail(const IRFunction& func) {
	for (size_t i = 0; i + 1 < func.instructions.size(); i++) {
		if (ir_has_effect(func.instructions[i].opcode, IR_TERMINATOR) &&
		    !ir_has_effect(func.instructions[i + 1].opcode, IR_LABEL)) {
			return true;
		}
	}
	return false;
}

// Whether any jump or branch targets the label that immediately follows it.
bool has_branch_to_next(const IRFunction& func) {
	for (size_t i = 0; i + 1 < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];
		if (instr.opcode == IROpcode::SWITCH) {
			continue;
		}
		if (instr.opcode != IROpcode::JUMP && !ir_has_effect(instr.opcode, IR_BRANCH)) {
			continue;
		}
		const IRValue* target = nullptr;
		for (const auto& operand : instr.operands) {
			if (operand.type == IRValue::Type::LABEL) {
				target = &operand;
			}
		}
		if (target == nullptr) {
			continue;
		}
		for (size_t j = i + 1; j < func.instructions.size(); j++) {
			if (!ir_has_effect(func.instructions[j].opcode, IR_LABEL)) {
				break;
			}
			if (func.instructions[j].operands[0].string_id == target->string_id) {
				return true;
			}
		}
	}
	return false;
}

// Whether the function loads `value` into some register as an integer immediate.
bool loads_int_immediate(const IRFunction& func, int64_t value) {
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM &&
		    instr.operands[1].immediate() == value) {
			return true;
		}
	}
	return false;
}

// Every label a jump or branch names has to exist: a pass that removes code
// must not leave a target behind.
void assert_labels_resolve(const IRFunction& func) {
	std::vector<uint32_t> defined;
	for (const auto& instr : func.instructions) {
		if (ir_has_effect(instr.opcode, IR_LABEL)) {
			defined.push_back(instr.operands[0].string_id);
		}
	}
	for (const auto& instr : func.instructions) {
		if (ir_has_effect(instr.opcode, IR_LABEL)) {
			continue;
		}
		for (const auto& operand : instr.operands) {
			if (operand.type != IRValue::Type::LABEL) {
				continue;
			}
			assert(std::find(defined.begin(), defined.end(), operand.string_id) != defined.end() &&
			       "branch target survived but its label did not");
		}
	}
}

// A label is a join point, and clearing the constant state at one used to end
// constant folding at the first `if` in a function. Neither `a` nor `b` is
// touched by the branch, so both are still known where they are added.
void test_constants_survive_a_label() {
	std::cout << "Testing constants kept across a label..." << std::endl;

	std::string source = R"(
func test(n):
	var a = 4
	var b = 9
	if n:
		pass
	return a + b
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << ir_to_string(func);

	assert(count_instructions(func, IROpcode::ADD) == 0 &&
	       "the addition is of two constants and should have folded");
	assert(loads_int_immediate(func, 13) && "4 + 9 should have folded to 13");

	std::cout << "  \u2713 Constants survive a label" << std::endl;
}

// A branch whose condition folded to a constant is not a branch, and the arm it
// guarded is not code.
void test_constant_branch_folds() {
	std::cout << "Testing branch on a constant condition..." << std::endl;

	std::string source = R"(
func test(x):
	var flag = false
	if flag:
		return 4
	else:
		return x
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << ir_to_string(func);

	for (const auto& instr : func.instructions) {
		assert(!ir_has_effect(instr.opcode, IR_BRANCH) &&
		       "a branch on a known condition should not survive");
	}
	assert(!loads_int_immediate(func, 4) &&
	       "the arm the condition rules out should not be emitted");
	assert_labels_resolve(func);

	std::cout << "  \u2713 Constant branch folded away" << std::endl;
}

// The same, taken the other way: a condition that is true leaves the else arm
// unreachable rather than the then arm.
void test_constant_branch_folds_when_taken() {
	std::cout << "Testing branch on a condition that is true..." << std::endl;

	std::string source = R"(
func test(x):
	var flag = 7
	if flag:
		return 4
	else:
		return x
)";

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << ir_to_string(func);

	for (const auto& instr : func.instructions) {
		assert(!ir_has_effect(instr.opcode, IR_BRANCH) &&
		       "a branch on a known condition should not survive");
	}
	assert(loads_int_immediate(func, 4) && "the arm the condition selects has to stay");
	assert_labels_resolve(func);

	std::cout << "  \u2713 Constant branch folded away, other arm dropped" << std::endl;
}

// `if/return/else` leaves a jump after every return, and the join at the end of
// the chain is reached by nothing at all.
void test_unreachable_code_removed() {
	std::cout << "Testing unreachable code removal..." << std::endl;

	std::string source = R"(
func test(n):
	if n < 0:
		return 1
	elif n == 0:
		return 2
	else:
		return 3
)";

	IRFunction unoptimized = compile_to_ir(source);
	assert(has_unreachable_tail(unoptimized) &&
	       "the reproduction needs code after a terminator to remove");

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << ir_to_string(func);

	assert(!has_unreachable_tail(func) && "nothing may follow a terminator but a label");
	assert_labels_resolve(func);

	std::cout << "  \u2713 Unreachable code removed" << std::endl;
}

// The last arm of a match jumps to the end label that immediately follows it.
void test_branch_to_next_removed() {
	std::cout << "Testing removal of a branch to the next instruction..." << std::endl;

	std::string source = R"(
func test(op):
	var r = 0
	match op:
		1:
			r = 10
		2:
			r = 20
		_:
			r = 30
	return r
)";

	IRFunction unoptimized = compile_to_ir(source);
	assert(has_branch_to_next(unoptimized) &&
	       "the reproduction needs a jump to the following label to remove");

	IRFunction func = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	std::cout << ir_to_string(func);

	assert(!has_branch_to_next(func) && "a jump to the next instruction is a no-op");
	assert_labels_resolve(func);

	std::cout << "  \u2713 Branch to the next instruction removed" << std::endl;
}

void test_move_after_call_folds_into_call_destination() {
	std::cout << "Testing MOVE-after-CALL folding..." << std::endl;
	IRFunction func = compile_to_ir(
		"func source():\n"
		"\treturn 4\n"
		"func test():\n"
		"\tvar value = source()\n"
		"\treturn value\n");
	IROptimizer optimizer;
	optimizer.optimize_function(func);

	assert(count_instructions(func, IROpcode::CALL) == 1);
	assert(count_instructions(func, IROpcode::MOVE) == 0);
	for (const IRInstruction& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL) {
			assert(ir_destination_register(instr) == IRFunction::RETURN_REGISTER);
		}
	}
	std::cout << "  ✓ CALL writes directly to the moved destination" << std::endl;
}

void test_struct_scalar_replacement_snapshots_fields() {
	std::cout << "Testing scalar-replaced struct fields are snapshots..." << std::endl;
	IRProgram ir = compile_program(
		"struct Point:\n"
		"\tvar x = 0\n"
		"\tvar y = 0\n\n"
		"func test():\n"
		"\tvar i = 4\n"
		"\tvar p = Point(i, 0)\n"
		"\ti += 1\n"
		"\treturn p.x\n\n"
		"func test_set():\n"
		"\tvar i = 4\n"
		"\tvar p = Point(0, 0)\n"
		"\tp.x = i\n"
		"\ti += 1\n"
		"\treturn p.x\n", true);
	const IRFunction& func = ir.functions.front();
	assert(count_instructions(func, IROpcode::MAKE_DICTIONARY_KEYED) == 0);
	assert(count_instructions(func, IROpcode::DICT_GET_CONST) == 0);
	const IRFunction& set_func = ir.functions.back();
	assert(count_instructions(set_func, IROpcode::MAKE_DICTIONARY_KEYED) == 0);
	assert(count_instructions(set_func, IROpcode::DICT_SET_CONST) == 0);
	IRInterpreter interpreter(ir);
	assert(std::get<int64_t>(interpreter.call("test")) == 4 &&
		"a constructed field must retain its source value");
	assert(std::get<int64_t>(interpreter.call("test_set")) == 4 &&
		"an assigned field must retain its source value");
	std::cout << "  ✓ scalar replacement snapshots source registers" << std::endl;
}

void test_immutable_struct_scalar_replacement_crosses_a_loop() {
	std::cout << "Testing immutable struct scalar replacement in a loop..." << std::endl;
	IRProgram ir = compile_program(
		"struct Point:\n"
		"\tvar x = 0\n"
		"\tvar y = 0\n\n"
		"func test(n):\n"
		"\tvar i = 0\n"
		"\tvar acc = 0\n"
		"\twhile i < n:\n"
		"\t\tvar p = Point(i, i + 1)\n"
		"\t\ti += 1\n"
		"\t\tacc += p.x\n"
		"\treturn acc\n", true);
	const IRFunction& func = ir.functions.front();
	assert(count_instructions(func, IROpcode::MAKE_DICTIONARY_KEYED) == 0);
	assert(count_instructions(func, IROpcode::DICT_GET_CONST) == 0);
	IRInterpreter interpreter(ir);
	assert(std::get<int64_t>(interpreter.call("test", {int64_t(5)})) == 10);
	std::cout << "  ✓ immutable loop-local struct is scalar-replaced" << std::endl;
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

		test_move_after_call_folds_into_call_destination();
		std::cout << std::endl;

		test_struct_scalar_replacement_snapshots_fields();
		std::cout << std::endl;

		test_immutable_struct_scalar_replacement_crosses_a_loop();
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

		test_dead_code_elimination_keeps_stored_globals();
		std::cout << std::endl;

		test_copy_chains_collapse();
		std::cout << std::endl;

		test_constants_survive_a_label();
		std::cout << std::endl;

		test_constant_branch_folds();
		std::cout << std::endl;

		test_constant_branch_folds_when_taken();
		std::cout << std::endl;

		test_unreachable_code_removed();
		std::cout << std::endl;

		test_branch_to_next_removed();
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
