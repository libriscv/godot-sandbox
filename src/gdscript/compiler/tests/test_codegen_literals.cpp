#include "../codegen.h"
#include "../lexer.h"
#include "../parser.h"
#include <cassert>
#include <iostream>

using namespace gdscript;

void test_array_literal() {
	std::cout << "Testing array literal codegen..." << std::endl;

	std::string source = R"(func test():
	var arr = [1, 2, 3]
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have CREATE_ARRAY and ARRAY_PUSH instructions
	bool has_create_array = false;
	int array_push_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			has_create_array = true;
			// Check that size is 3
			assert(instr.operands.size() == 2);
			assert(instr.operands[1].type == IRValue::Type::IMMEDIATE);
			assert(std::get<int64_t>(instr.operands[1].value) == 3);
		}
		if (instr.opcode == IROpcode::ARRAY_PUSH) {
			array_push_count++;
		}
	}

	assert(has_create_array);
	assert(array_push_count == 3); // Three elements pushed

	std::cout << "  ✓ Array literal codegen test passed" << std::endl;
}

void test_empty_array() {
	std::cout << "Testing empty array codegen..." << std::endl;

	std::string source = R"(func test():
	var arr = []
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	bool has_create_array = false;
	int array_push_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			has_create_array = true;
			assert(instr.operands.size() == 2);
			assert(std::get<int64_t>(instr.operands[1].value) == 0);
		}
		if (instr.opcode == IROpcode::ARRAY_PUSH) {
			array_push_count++;
		}
	}

	assert(has_create_array);
	assert(array_push_count == 0);

	std::cout << "  ✓ Empty array codegen test passed" << std::endl;
}

void test_dictionary_literal() {
	std::cout << "Testing dictionary literal codegen..." << std::endl;

	std::string source = R"(func test():
	var dict = {"key1": "value1", "key2": 42}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	// Should have CREATE_DICT and DICT_SET instructions
	bool has_create_dict = false;
	int dict_set_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_DICT) {
			has_create_dict = true;
		}
		if (instr.opcode == IROpcode::DICT_SET) {
			dict_set_count++;
			// DICT_SET should have 3 operands: dict_reg, key_reg, value_reg
			assert(instr.operands.size() == 3);
		}
	}

	assert(has_create_dict);
	assert(dict_set_count == 2); // Two key-value pairs

	std::cout << "  ✓ Dictionary literal codegen test passed" << std::endl;
}

void test_empty_dictionary() {
	std::cout << "Testing empty dictionary codegen..." << std::endl;

	std::string source = R"(func test():
	var dict = {}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	bool has_create_dict = false;
	int dict_set_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_DICT) {
			has_create_dict = true;
		}
		if (instr.opcode == IROpcode::DICT_SET) {
			dict_set_count++;
		}
	}

	assert(has_create_dict);
	assert(dict_set_count == 0);

	std::cout << "  ✓ Empty dictionary codegen test passed" << std::endl;
}

void test_nested_structures_codegen() {
	std::cout << "Testing nested structures codegen..." << std::endl;

	// Test array containing dictionary
	std::string source1 = R"(func test():
	var arr = [1, 2, {"a": 1, "b": 2}]
	return arr
)";

	Lexer lexer1(source1);
	Parser parser1(lexer1.tokenize());
	Program program1 = parser1.parse();

	CodeGenerator codegen1;
	IRProgram ir1 = codegen1.generate(program1);

	int create_array_count = 0;
	int create_dict_count = 0;
	int array_push_count = 0;
	int dict_set_count = 0;

	for (const auto &instr : ir1.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			create_array_count++;
		}
		if (instr.opcode == IROpcode::CREATE_DICT) {
			create_dict_count++;
		}
		if (instr.opcode == IROpcode::ARRAY_PUSH) {
			array_push_count++;
		}
		if (instr.opcode == IROpcode::DICT_SET) {
			dict_set_count++;
		}
	}

	// Should have: 1 outer array + 1 dictionary = 2 creates
	// 3 array pushes (1, 2, dict) + 2 dict sets (a:1, b:2)
	assert(create_array_count >= 1);
	assert(create_dict_count >= 1);
	assert(array_push_count >= 3);
	assert(dict_set_count >= 2);

	// Test deeply nested arrays
	std::string source2 = R"(func test():
	var arr = [[1, [2, 3]], [4, [5, 6]]]
	return arr
)";

	Lexer lexer2(source2);
	Parser parser2(lexer2.tokenize());
	Program program2 = parser2.parse();

	CodeGenerator codegen2;
	IRProgram ir2 = codegen2.generate(program2);

	int create_array_count2 = 0;
	int array_push_count2 = 0;

	for (const auto &instr : ir2.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			create_array_count2++;
		}
		if (instr.opcode == IROpcode::ARRAY_PUSH) {
			array_push_count2++;
		}
	}

	// Should have: 1 outer + 2 middle + 2 inner = 5 arrays
	// Multiple pushes for all elements
	assert(create_array_count2 >= 5);
	assert(array_push_count2 >= 6); // At least 6 elements total

	// Test dictionary containing arrays
	std::string source3 = R"(func test():
	var dict = {"arr1": [1, 2, 3], "arr2": [4, 5, 6]}
	return dict
)";

	Lexer lexer3(source3);
	Parser parser3(lexer3.tokenize());
	Program program3 = parser3.parse();

	CodeGenerator codegen3;
	IRProgram ir3 = codegen3.generate(program3);

	int create_array_count3 = 0;
	int create_dict_count3 = 0;
	int dict_set_count3 = 0;

	for (const auto &instr : ir3.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			create_array_count3++;
		}
		if (instr.opcode == IROpcode::CREATE_DICT) {
			create_dict_count3++;
		}
		if (instr.opcode == IROpcode::DICT_SET) {
			dict_set_count3++;
		}
	}

	// Should have: 1 dict + 2 arrays = 3 creates
	// 2 dict sets (arr1, arr2)
	assert(create_dict_count3 >= 1);
	assert(create_array_count3 >= 2);
	assert(dict_set_count3 >= 2);

	// Test complex mixed structure
	std::string source4 = R"(func test():
	var complex = [[1, 2, 3], {"a": 1, "b": 2}, [{"x": 10}, {"y": 20}]]
	return complex
)";

	Lexer lexer4(source4);
	Parser parser4(lexer4.tokenize());
	Program program4 = parser4.parse();

	CodeGenerator codegen4;
	IRProgram ir4 = codegen4.generate(program4);

	int create_array_count4 = 0;
	int create_dict_count4 = 0;

	for (const auto &instr : ir4.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			create_array_count4++;
		}
		if (instr.opcode == IROpcode::CREATE_DICT) {
			create_dict_count4++;
		}
	}

	// Should have: 1 outer array + 1 inner array + 1 dict + 2 dicts in array = 5 total
	// At least 3 arrays (outer, first element, third element) and 3 dicts (second element, two in third element)
	assert(create_array_count4 >= 3);
	assert(create_dict_count4 >= 3);

	std::cout << "  ✓ Nested structures codegen test passed" << std::endl;
}

void test_single_element_array_codegen() {
	std::cout << "Testing single element array codegen..." << std::endl;

	std::string source = R"(func test():
	var arr = [42]
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	bool has_create_array = false;
	int array_push_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			has_create_array = true;
			assert(instr.operands.size() == 2);
			assert(std::get<int64_t>(instr.operands[1].value) == 1);
		}
		if (instr.opcode == IROpcode::ARRAY_PUSH) {
			array_push_count++;
		}
	}

	assert(has_create_array);
	assert(array_push_count == 1);

	std::cout << "  ✓ Single element array codegen test passed" << std::endl;
}

void test_single_pair_dictionary_codegen() {
	std::cout << "Testing single pair dictionary codegen..." << std::endl;

	std::string source = R"(func test():
	var dict = {"key": "value"}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	bool has_create_dict = false;
	int dict_set_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_DICT) {
			has_create_dict = true;
		}
		if (instr.opcode == IROpcode::DICT_SET) {
			dict_set_count++;
		}
	}

	assert(has_create_dict);
	assert(dict_set_count == 1);

	std::cout << "  ✓ Single pair dictionary codegen test passed" << std::endl;
}

void test_array_with_expressions_codegen() {
	std::cout << "Testing array with expressions codegen..." << std::endl;

	std::string source = R"(func test():
	var arr = [1 + 2, 3 * 4]
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	bool has_create_array = false;
	int array_push_count = 0;
	int add_count = 0;
	int mul_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			has_create_array = true;
		}
		if (instr.opcode == IROpcode::ARRAY_PUSH) {
			array_push_count++;
		}
		if (instr.opcode == IROpcode::ADD) {
			add_count++;
		}
		if (instr.opcode == IROpcode::MUL) {
			mul_count++;
		}
	}

	assert(has_create_array);
	assert(array_push_count == 2);
	assert(add_count >= 1); // At least one ADD for 1 + 2
	assert(mul_count >= 1); // At least one MUL for 3 * 4

	std::cout << "  ✓ Array with expressions codegen test passed" << std::endl;
}

void test_dictionary_with_expressions_codegen() {
	std::cout << "Testing dictionary with expressions codegen..." << std::endl;

	std::string source = R"(func test():
	var dict = {1 + 1: 2 * 2}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	bool has_create_dict = false;
	int dict_set_count = 0;
	int add_count = 0;
	int mul_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_DICT) {
			has_create_dict = true;
		}
		if (instr.opcode == IROpcode::DICT_SET) {
			dict_set_count++;
		}
		if (instr.opcode == IROpcode::ADD) {
			add_count++;
		}
		if (instr.opcode == IROpcode::MUL) {
			mul_count++;
		}
	}

	assert(has_create_dict);
	assert(dict_set_count == 1);
	assert(add_count >= 1); // At least one ADD for 1 + 1 (key)
	assert(mul_count >= 1); // At least one MUL for 2 * 2 (value)

	std::cout << "  ✓ Dictionary with expressions codegen test passed" << std::endl;
}

void test_array_as_return_value_codegen() {
	std::cout << "Testing array as return value codegen..." << std::endl;

	std::string source = R"(func test():
	return [1, 2, 3]
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	bool has_create_array = false;
	bool has_return = false;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			has_create_array = true;
		}
		if (instr.opcode == IROpcode::RETURN) {
			has_return = true;
		}
	}

	assert(has_create_array);
	assert(has_return);

	std::cout << "  ✓ Array as return value codegen test passed" << std::endl;
}

void test_dictionary_as_return_value_codegen() {
	std::cout << "Testing dictionary as return value codegen..." << std::endl;

	std::string source = R"(func test():
	return {"key": "value"}
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	bool has_create_dict = false;
	bool has_return = false;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_DICT) {
			has_create_dict = true;
		}
		if (instr.opcode == IROpcode::RETURN) {
			has_return = true;
		}
	}

	assert(has_create_dict);
	assert(has_return);

	std::cout << "  ✓ Dictionary as return value codegen test passed" << std::endl;
}

void test_deeply_nested_dictionary_codegen() {
	std::cout << "Testing deeply nested dictionary codegen..." << std::endl;

	std::string source = R"(func test():
	var dict = {'a': {'b': {'c': {'d': 1}}}}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	int create_dict_count = 0;
	int dict_set_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_DICT) {
			create_dict_count++;
		}
		if (instr.opcode == IROpcode::DICT_SET) {
			dict_set_count++;
		}
	}

	// Should have: 1 outer + 3 nested = 4 dicts
	// 4 dict sets (one for each level)
	assert(create_dict_count >= 4);
	assert(dict_set_count >= 4);

	std::cout << "  ✓ Deeply nested dictionary codegen test passed" << std::endl;
}

void test_large_array_codegen() {
	std::cout << "Testing large array codegen..." << std::endl;

	std::string source = R"(func test():
	var arr = [0, 1, 2, 3, 4, 5, 6, 7, 8]
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	bool has_create_array = false;
	int array_push_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			has_create_array = true;
			assert(instr.operands.size() == 2);
			assert(std::get<int64_t>(instr.operands[1].value) == 9);
		}
		if (instr.opcode == IROpcode::ARRAY_PUSH) {
			array_push_count++;
		}
	}

	assert(has_create_array);
	assert(array_push_count == 9);

	std::cout << "  ✓ Large array codegen test passed" << std::endl;
}

void test_array_of_arrays_of_dictionaries_codegen() {
	std::cout << "Testing array of arrays of dictionaries codegen..." << std::endl;

	std::string source = R"(func test():
	var arr = [[{'a': 1}], [{'b': 2}], [{'c': 3}]]
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	int create_array_count = 0;
	int create_dict_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			create_array_count++;
		}
		if (instr.opcode == IROpcode::CREATE_DICT) {
			create_dict_count++;
		}
	}

	// Should have: 1 outer + 3 middle = 4 arrays, 3 dicts
	assert(create_array_count >= 4);
	assert(create_dict_count >= 3);

	std::cout << "  ✓ Array of arrays of dictionaries codegen test passed" << std::endl;
}

void test_dictionary_with_nested_dict_and_array_codegen() {
	std::cout << "Testing dictionary with nested dict and array codegen..." << std::endl;

	std::string source = R"(func test():
	var dict = {'a': 1, 'b': {'c': 88}, 'i': [1, 2, 3], 'z': {}}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);

	int create_dict_count = 0;
	int create_array_count = 0;
	int dict_set_count = 0;

	for (const auto &instr : ir.functions[0].instructions) {
		if (instr.opcode == IROpcode::CREATE_DICT) {
			create_dict_count++;
		}
		if (instr.opcode == IROpcode::CREATE_ARRAY) {
			create_array_count++;
		}
		if (instr.opcode == IROpcode::DICT_SET) {
			dict_set_count++;
		}
	}

	// Should have: 1 outer dict + 1 nested dict + 1 empty dict = 3 dicts
	// 1 array
	// Multiple dict sets
	assert(create_dict_count >= 3);
	assert(create_array_count >= 1);
	assert(dict_set_count >= 4); // At least 4 sets (a, b, c, i)

	std::cout << "  ✓ Dictionary with nested dict and array codegen test passed" << std::endl;
}

void test_array_index_assignment_codegen() {
	std::cout << "Testing array index assignment codegen..." << std::endl;

	std::string source = R"(func test():
	var arr = [1, 2, 3]
	arr[0] = 42
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(parser.parse());

	assert(ir.functions.size() == 1);
	const auto &func = ir.functions[0];

	// Should have VSET instruction for arr[0] = 42
	bool has_vset = false;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::VSET) {
			has_vset = true;
			// VSET should have 3 operands: obj_reg, idx_reg, value_reg
			assert(instr.operands.size() == 3);
			break;
		}
	}
	assert(has_vset);

	std::cout << "  ✓ Array index assignment codegen test passed" << std::endl;
}

void test_dictionary_index_assignment_codegen() {
	std::cout << "Testing dictionary index assignment codegen..." << std::endl;

	std::string source = R"(func test():
	var dict = {"key1": "value1"}
	dict["key2"] = "value2"
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(parser.parse());

	assert(ir.functions.size() == 1);
	const auto &func = ir.functions[0];

	// Should have VSET instruction for dict["key2"] = "value2"
	bool has_vset = false;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::VSET) {
			has_vset = true;
			assert(instr.operands.size() == 3);
			break;
		}
	}
	assert(has_vset);

	std::cout << "  ✓ Dictionary index assignment codegen test passed" << std::endl;
}

void test_index_read_codegen() {
	std::cout << "Testing index read codegen..." << std::endl;

	std::string source = R"(func test():
	var arr = [1, 2, 3]
	var x = arr[0]
	return x
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(parser.parse());

	assert(ir.functions.size() == 1);
	const auto &func = ir.functions[0];

	// Should have VGET instruction for arr[0]
	bool has_vget = false;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::VGET) {
			has_vget = true;
			// VGET should have 3 operands: result_reg, obj_reg, idx_reg
			assert(instr.operands.size() == 3);
			break;
		}
	}
	assert(has_vget);

	std::cout << "  ✓ Index read codegen test passed" << std::endl;
}

void test_variable_assignment_still_works_codegen() {
	std::cout << "Testing variable assignment still works codegen..." << std::endl;

	std::string source = R"(func test():
	var x = 5
	x = 10
	return x
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(parser.parse());

	assert(ir.functions.size() == 1);
	const auto &func = ir.functions[0];

	// Should NOT have VSET (variable assignment uses MOVE)
	// Should have MOVE instruction for x = 10
	bool has_move = false;
	bool has_vset = false;
	for (const auto &instr : func.instructions) {
		if (instr.opcode == IROpcode::MOVE) {
			has_move = true;
		}
		if (instr.opcode == IROpcode::VSET) {
			has_vset = true;
		}
	}
	assert(has_move);
	assert(!has_vset); // Variable assignment should not use VSET

	std::cout << "  ✓ Variable assignment still works codegen test passed" << std::endl;
}

int main() {
	std::cout << "\n=== Running Array and Dictionary Literal Code Generation Tests ===" << std::endl;

	try {
		test_array_literal();
		test_empty_array();
		test_dictionary_literal();
		test_empty_dictionary();
		test_nested_structures_codegen();
		test_single_element_array_codegen();
		test_single_pair_dictionary_codegen();
		test_array_with_expressions_codegen();
		test_dictionary_with_expressions_codegen();
		test_array_as_return_value_codegen();
		test_dictionary_as_return_value_codegen();
		test_deeply_nested_dictionary_codegen();
		test_large_array_codegen();
		test_array_of_arrays_of_dictionaries_codegen();
		test_dictionary_with_nested_dict_and_array_codegen();

		// Index assignment codegen tests
		test_array_index_assignment_codegen();
		test_dictionary_index_assignment_codegen();
		test_index_read_codegen();
		test_variable_assignment_still_works_codegen();

		std::cout << "\n✅ All array and dictionary literal code generation tests passed!" << std::endl;
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}
