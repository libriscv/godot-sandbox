#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include <cassert>
#include <iostream>

using namespace gdscript;

void test_simple_arithmetic() {
	std::cout << "Testing simple arithmetic..." << std::endl;

	std::string source = R"(func add(a, b):
	return a + b
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	assert(ir.functions.size() == 1);
	assert(ir.functions[0].name == "add");
	assert(ir.functions[0].parameters.size() == 2);
	assert(ir.functions[0].instructions.size() > 0);

	// Should have ADD instruction
	bool has_add = false;
	for (const auto& instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::ADD) {
			has_add = true;
			break;
		}
	}
	assert(has_add);

	std::cout << "  ✓ Simple arithmetic test passed" << std::endl;
}

void test_variable_operations() {
	std::cout << "Testing variable operations..." << std::endl;

	std::string source = R"(func test():
	var x = 10
	var y = 20
	var sum = x + y
	return sum
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	assert(ir.functions[0].instructions.size() > 0);

	// Should have LOAD_IMM for constants
	int load_imm_count = 0;
	for (const auto& instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM) {
			load_imm_count++;
		}
	}
	assert(load_imm_count >= 2); // At least for 10 and 20

	std::cout << "  ✓ Variable operations test passed" << std::endl;
}

void test_control_flow() {
	std::cout << "Testing control flow..." << std::endl;

	std::string source = R"(func abs(x):
	if x < 0:
		return -x
	else:
		return x
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have labels and branches
	bool has_label = false;
	bool has_branch = false;

	for (const auto& instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::LABEL) has_label = true;
		if (instr.opcode == IROpcode::BRANCH_ZERO) has_branch = true;
	}

	assert(has_label);
	assert(has_branch);

	std::cout << "  ✓ Control flow test passed" << std::endl;
}

void test_loop_generation() {
	std::cout << "Testing loop generation..." << std::endl;

	std::string source = R"(func count(n):
	var i = 0
	while i < n:
		i = i + 1
	return i
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have loop label and jump back
	bool has_jump = false;
	int label_count = 0;

	for (const auto& instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::JUMP) has_jump = true;
		if (instr.opcode == IROpcode::LABEL) label_count++;
	}

	assert(has_jump);
	assert(label_count >= 2); // loop start and end labels

	std::cout << "  ✓ Loop generation test passed" << std::endl;
}

void test_function_calls() {
	std::cout << "Testing function calls..." << std::endl;

	std::string source = R"(func helper(x):
	return x * 2

func main():
	var result = helper(21)
	return result
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	assert(ir.functions.size() == 2);

	// main() should have a CALL instruction
	bool has_call = false;
	for (const auto& instr : ir.functions[1].instructions) {
		if (instr.opcode == IROpcode::CALL) {
			has_call = true;
			break;
		}
	}
	assert(has_call);

	std::cout << "  ✓ Function calls test passed" << std::endl;
}

void test_comparison_operators() {
	std::cout << "Testing comparison operators..." << std::endl;

	std::string source = R"(func test(a, b):
	var eq = a == b
	var ne = a != b
	var lt = a < b
	var lte = a <= b
	var gt = a > b
	var gte = a >= b
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Count different comparison operations
	int cmp_count = 0;
	for (const auto& instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CMP_EQ ||
		    instr.opcode == IROpcode::CMP_NEQ ||
		    instr.opcode == IROpcode::CMP_LT ||
		    instr.opcode == IROpcode::CMP_LTE ||
		    instr.opcode == IROpcode::CMP_GT ||
		    instr.opcode == IROpcode::CMP_GTE) {
			cmp_count++;
		}
	}

	assert(cmp_count == 6);

	std::cout << "  ✓ Comparison operators test passed" << std::endl;
}

void test_logical_operators() {
	std::cout << "Testing logical operators..." << std::endl;

	std::string source = R"(func test(a, b, c):
	var result = a and b or not c
	return result
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have AND, OR, and NOT operations
	bool has_and = false;
	bool has_or = false;
	bool has_not = false;

	for (const auto& instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::AND) has_and = true;
		if (instr.opcode == IROpcode::OR) has_or = true;
		if (instr.opcode == IROpcode::NOT) has_not = true;
	}

	assert(has_and);
	assert(has_or);
	assert(has_not);

	std::cout << "  ✓ Logical operators test passed" << std::endl;
}

void test_complex_expression() {
	std::cout << "Testing complex expression..." << std::endl;

	std::string source = R"(func calc(a, b, c):
	return (a + b) * c - a / b
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have multiple arithmetic operations
	int add_count = 0, mul_count = 0, sub_count = 0, div_count = 0;

	for (const auto& instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::ADD) add_count++;
		if (instr.opcode == IROpcode::MUL) mul_count++;
		if (instr.opcode == IROpcode::SUB) sub_count++;
		if (instr.opcode == IROpcode::DIV) div_count++;
	}

	assert(add_count > 0);
	assert(mul_count > 0);
	assert(sub_count > 0);
	assert(div_count > 0);

	std::cout << "  ✓ Complex expression test passed" << std::endl;
}

void test_string_constants() {
	std::cout << "Testing string constants..." << std::endl;

	std::string source = R"(func greet():
	var msg = "Hello, World!"
	return msg
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have at least one string constant
	assert(ir.string_constants.size() == 1);
	assert(ir.string_constants[0] == "Hello, World!");

	std::cout << "  ✓ String constants test passed" << std::endl;
}

void test_subscript_operations() {
	std::cout << "Testing subscript operations..." << std::endl;

	// Test array/dict indexing for reading
	std::string source_read = R"(func get_item(arr, idx):
	var item = arr[idx]
	return item
)";

	Lexer lexer_read(source_read);
	Parser parser_read(lexer_read.tokenize());
	Program program_read = parser_read.parse();

	CodeGenerator codegen_read;
	IRProgram ir_read = codegen_read.generate(program_read);

	// Should have VCALL for arr.get(idx)
	bool has_get_vcall = false;
	for (const auto& instr : ir_read.functions[0].instructions) {
		if (instr.opcode == IROpcode::VCALL) {
			if (instr.operands.size() >= 3 && instr.operands[2].type == IRValue::Type::STRING) {
				if (std::get<std::string>(instr.operands[2].value) == "get") {
					has_get_vcall = true;
					break;
				}
			}
		}
	}
	assert(has_get_vcall);

	// Test array/dict indexing for writing
	std::string source_write = R"(func set_item(arr, idx, value):
	arr[idx] = value
	return arr
)";

	Lexer lexer_write(source_write);
	Parser parser_write(lexer_write.tokenize());
	Program program_write = parser_write.parse();

	CodeGenerator codegen_write;
	IRProgram ir_write = codegen_write.generate(program_write);

	// Should have VCALL for arr.set(idx, value)
	bool has_set_vcall = false;
	for (const auto& instr : ir_write.functions[0].instructions) {
		if (instr.opcode == IROpcode::VCALL) {
			if (instr.operands.size() >= 3 && instr.operands[2].type == IRValue::Type::STRING) {
				if (std::get<std::string>(instr.operands[2].value) == "set") {
					has_set_vcall = true;
					break;
				}
			}
		}
	}
	assert(has_set_vcall);

	std::cout << "  ✓ Subscript operations test passed" << std::endl;
}

int main() {
	std::cout << "\n=== Running Code Generation Tests ===" << std::endl;

	try {
		test_simple_arithmetic();
		test_variable_operations();
		test_control_flow();
		test_loop_generation();
		test_function_calls();
		test_comparison_operators();
		test_logical_operators();
		test_complex_expression();
		test_string_constants();
		test_subscript_operations();

		std::cout << "\n✅ All code generation tests passed!" << std::endl;
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}
