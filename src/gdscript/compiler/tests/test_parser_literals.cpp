#include "../lexer.h"
#include "../parser.h"
#include <cassert>
#include <iostream>

using namespace gdscript;

void test_array_literal() {
	std::cout << "Testing array literal..." << std::endl;

	std::string source = R"(func test():
	var arr = [1, 2, 3]
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 2);

	// Check variable declaration with array literal
	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);
	assert(var_decl->name == "arr");
	assert(var_decl->initializer != nullptr);

	auto *array_expr = dynamic_cast<ArrayExpr *>(var_decl->initializer.get());
	assert(array_expr != nullptr);
	assert(array_expr->elements.size() == 3);

	// Check that elements are literals
	for (const auto &elem : array_expr->elements) {
		auto *lit = dynamic_cast<LiteralExpr *>(elem.get());
		assert(lit != nullptr);
		assert(lit->lit_type == LiteralExpr::Type::INTEGER);
	}

	std::cout << "  ✓ Array literal test passed" << std::endl;
}

void test_empty_array() {
	std::cout << "Testing empty array..." << std::endl;

	std::string source = R"(func test():
	var arr = []
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);

	auto *array_expr = dynamic_cast<ArrayExpr *>(var_decl->initializer.get());
	assert(array_expr != nullptr);
	assert(array_expr->elements.size() == 0);

	std::cout << "  ✓ Empty array test passed" << std::endl;
}

void test_dictionary_literal() {
	std::cout << "Testing dictionary literal..." << std::endl;

	std::string source = R"(func test():
	var dict = {"key1": "value1", "key2": 42}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 2);

	// Check variable declaration with dictionary literal
	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);
	assert(var_decl->name == "dict");
	assert(var_decl->initializer != nullptr);

	auto *dict_expr = dynamic_cast<DictionaryExpr *>(var_decl->initializer.get());
	assert(dict_expr != nullptr);
	assert(dict_expr->pairs.size() == 2);

	// Check first pair: "key1": "value1"
	assert(dict_expr->pairs[0].key != nullptr);
	assert(dict_expr->pairs[0].value != nullptr);
	auto *key1 = dynamic_cast<LiteralExpr *>(dict_expr->pairs[0].key.get());
	auto *val1 = dynamic_cast<LiteralExpr *>(dict_expr->pairs[0].value.get());
	assert(key1 != nullptr);
	assert(val1 != nullptr);
	assert(key1->lit_type == LiteralExpr::Type::STRING);
	assert(val1->lit_type == LiteralExpr::Type::STRING);
	assert(std::get<std::string>(key1->value) == "key1");
	assert(std::get<std::string>(val1->value) == "value1");

	// Check second pair: "key2": 42
	auto *key2 = dynamic_cast<LiteralExpr *>(dict_expr->pairs[1].key.get());
	auto *val2 = dynamic_cast<LiteralExpr *>(dict_expr->pairs[1].value.get());
	assert(key2 != nullptr);
	assert(val2 != nullptr);
	assert(key2->lit_type == LiteralExpr::Type::STRING);
	assert(val2->lit_type == LiteralExpr::Type::INTEGER);
	assert(std::get<std::string>(key2->value) == "key2");
	assert(std::get<int64_t>(val2->value) == 42);

	std::cout << "  ✓ Dictionary literal test passed" << std::endl;
}

void test_empty_dictionary() {
	std::cout << "Testing empty dictionary..." << std::endl;

	std::string source = R"(func test():
	var dict = {}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);

	auto *dict_expr = dynamic_cast<DictionaryExpr *>(var_decl->initializer.get());
	assert(dict_expr != nullptr);
	assert(dict_expr->pairs.size() == 0);

	std::cout << "  ✓ Empty dictionary test passed" << std::endl;
}

void test_nested_structures() {
	std::cout << "Testing nested structures..." << std::endl;

	// Test array containing dictionary
	std::string source1 = R"(func test():
	var arr = [1, 2, {"a": 1, "b": 2}]
	return arr
)";

	Lexer lexer1(source1);
	Parser parser1(lexer1.tokenize());
	Program program1 = parser1.parse();

	auto *var_decl1 = dynamic_cast<VarDeclStmt *>(program1.functions[0].body[0].get());
	assert(var_decl1 != nullptr);
	auto *array_expr1 = dynamic_cast<ArrayExpr *>(var_decl1->initializer.get());
	assert(array_expr1 != nullptr);
	assert(array_expr1->elements.size() == 3);

	// Third element should be a dictionary
	auto *dict_expr = dynamic_cast<DictionaryExpr *>(array_expr1->elements[2].get());
	assert(dict_expr != nullptr);
	assert(dict_expr->pairs.size() == 2);

	// Test deeply nested arrays
	std::string source2 = R"(func test():
	var arr = [[1, [2, 3]], [4, [5, 6]]]
	return arr
)";

	Lexer lexer2(source2);
	Parser parser2(lexer2.tokenize());
	Program program2 = parser2.parse();

	auto *var_decl2 = dynamic_cast<VarDeclStmt *>(program2.functions[0].body[0].get());
	assert(var_decl2 != nullptr);
	auto *array_expr2 = dynamic_cast<ArrayExpr *>(var_decl2->initializer.get());
	assert(array_expr2 != nullptr);
	assert(array_expr2->elements.size() == 2);

	// First element should be an array
	auto *nested_array1 = dynamic_cast<ArrayExpr *>(array_expr2->elements[0].get());
	assert(nested_array1 != nullptr);
	assert(nested_array1->elements.size() == 2);

	// Second element of nested array should also be an array
	auto *nested_array2 = dynamic_cast<ArrayExpr *>(nested_array1->elements[1].get());
	assert(nested_array2 != nullptr);
	assert(nested_array2->elements.size() == 2);

	// Test dictionary containing arrays
	std::string source3 = R"(func test():
	var dict = {"arr1": [1, 2, 3], "arr2": [4, 5, 6]}
	return dict
)";

	Lexer lexer3(source3);
	Parser parser3(lexer3.tokenize());
	Program program3 = parser3.parse();

	auto *var_decl3 = dynamic_cast<VarDeclStmt *>(program3.functions[0].body[0].get());
	assert(var_decl3 != nullptr);
	auto *dict_expr3 = dynamic_cast<DictionaryExpr *>(var_decl3->initializer.get());
	assert(dict_expr3 != nullptr);
	assert(dict_expr3->pairs.size() == 2);

	// First value should be an array
	auto *arr_value1 = dynamic_cast<ArrayExpr *>(dict_expr3->pairs[0].value.get());
	assert(arr_value1 != nullptr);
	assert(arr_value1->elements.size() == 3);

	// Test complex mixed structure
	std::string source4 = R"(func test():
	var complex = [[1, 2, 3], {"a": 1, "b": 2}, [{"x": 10}, {"y": 20}]]
	return complex
)";

	Lexer lexer4(source4);
	Parser parser4(lexer4.tokenize());
	Program program4 = parser4.parse();

	auto *var_decl4 = dynamic_cast<VarDeclStmt *>(program4.functions[0].body[0].get());
	assert(var_decl4 != nullptr);
	auto *array_expr4 = dynamic_cast<ArrayExpr *>(var_decl4->initializer.get());
	assert(array_expr4 != nullptr);
	assert(array_expr4->elements.size() == 3);

	// First element: array
	auto *elem1 = dynamic_cast<ArrayExpr *>(array_expr4->elements[0].get());
	assert(elem1 != nullptr);

	// Second element: dictionary
	auto *elem2 = dynamic_cast<DictionaryExpr *>(array_expr4->elements[1].get());
	assert(elem2 != nullptr);

	// Third element: array containing dictionaries
	auto *elem3 = dynamic_cast<ArrayExpr *>(array_expr4->elements[2].get());
	assert(elem3 != nullptr);
	assert(elem3->elements.size() == 2);
	auto *dict_in_array = dynamic_cast<DictionaryExpr *>(elem3->elements[0].get());
	assert(dict_in_array != nullptr);

	std::cout << "  ✓ Nested structures test passed" << std::endl;
}

void test_single_element_array() {
	std::cout << "Testing single element array..." << std::endl;

	std::string source = R"(func test():
	var arr = [42]
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);
	auto *array_expr = dynamic_cast<ArrayExpr *>(var_decl->initializer.get());
	assert(array_expr != nullptr);
	assert(array_expr->elements.size() == 1);

	std::cout << "  ✓ Single element array test passed" << std::endl;
}

void test_single_pair_dictionary() {
	std::cout << "Testing single pair dictionary..." << std::endl;

	std::string source = R"(func test():
	var dict = {"key": "value"}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);
	auto *dict_expr = dynamic_cast<DictionaryExpr *>(var_decl->initializer.get());
	assert(dict_expr != nullptr);
	assert(dict_expr->pairs.size() == 1);
	assert(dict_expr->pairs[0].key != nullptr);
	assert(dict_expr->pairs[0].value != nullptr);

	std::cout << "  ✓ Single pair dictionary test passed" << std::endl;
}

void test_array_with_expressions() {
	std::cout << "Testing array with expressions..." << std::endl;

	std::string source = R"(func test():
	var arr = [1 + 2, 3 * 4, 10 - 5]
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);
	auto *array_expr = dynamic_cast<ArrayExpr *>(var_decl->initializer.get());
	assert(array_expr != nullptr);
	assert(array_expr->elements.size() == 3);

	// First element should be a binary expression (1 + 2)
	auto *bin_expr1 = dynamic_cast<BinaryExpr *>(array_expr->elements[0].get());
	assert(bin_expr1 != nullptr);
	assert(bin_expr1->op == BinaryExpr::Op::ADD);

	// Second element should be a binary expression (3 * 4)
	auto *bin_expr2 = dynamic_cast<BinaryExpr *>(array_expr->elements[1].get());
	assert(bin_expr2 != nullptr);
	assert(bin_expr2->op == BinaryExpr::Op::MUL);

	std::cout << "  ✓ Array with expressions test passed" << std::endl;
}

void test_dictionary_with_expressions() {
	std::cout << "Testing dictionary with expressions..." << std::endl;

	std::string source = R"(func test():
	var dict = {1 + 1: 2 * 2, "key": 10 - 5}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);
	auto *dict_expr = dynamic_cast<DictionaryExpr *>(var_decl->initializer.get());
	assert(dict_expr != nullptr);
	assert(dict_expr->pairs.size() == 2);

	// First pair: key should be binary expression (1 + 1), value should be binary expression (2 * 2)
	auto *key1 = dynamic_cast<BinaryExpr *>(dict_expr->pairs[0].key.get());
	auto *val1 = dynamic_cast<BinaryExpr *>(dict_expr->pairs[0].value.get());
	assert(key1 != nullptr);
	assert(val1 != nullptr);
	assert(key1->op == BinaryExpr::Op::ADD);
	assert(val1->op == BinaryExpr::Op::MUL);

	// Second pair: key should be string literal, value should be binary expression
	auto *key2 = dynamic_cast<LiteralExpr *>(dict_expr->pairs[1].key.get());
	auto *val2 = dynamic_cast<BinaryExpr *>(dict_expr->pairs[1].value.get());
	assert(key2 != nullptr);
	assert(val2 != nullptr);
	assert(key2->lit_type == LiteralExpr::Type::STRING);
	assert(val2->op == BinaryExpr::Op::SUB);

	std::cout << "  ✓ Dictionary with expressions test passed" << std::endl;
}

void test_array_as_return_value() {
	std::cout << "Testing array as return value..." << std::endl;

	std::string source = R"(func test():
	return [1, 2, 3]
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 1);
	auto *return_stmt = dynamic_cast<ReturnStmt *>(program.functions[0].body[0].get());
	assert(return_stmt != nullptr);
	assert(return_stmt->value != nullptr);

	auto *array_expr = dynamic_cast<ArrayExpr *>(return_stmt->value.get());
	assert(array_expr != nullptr);
	assert(array_expr->elements.size() == 3);

	std::cout << "  ✓ Array as return value test passed" << std::endl;
}

void test_dictionary_as_return_value() {
	std::cout << "Testing dictionary as return value..." << std::endl;

	std::string source = R"(func test():
	return {"key": "value"}
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 1);
	auto *return_stmt = dynamic_cast<ReturnStmt *>(program.functions[0].body[0].get());
	assert(return_stmt != nullptr);
	assert(return_stmt->value != nullptr);

	auto *dict_expr = dynamic_cast<DictionaryExpr *>(return_stmt->value.get());
	assert(dict_expr != nullptr);
	assert(dict_expr->pairs.size() == 1);

	std::cout << "  ✓ Dictionary as return value test passed" << std::endl;
}

void test_deeply_nested_dictionary() {
	std::cout << "Testing deeply nested dictionary..." << std::endl;

	std::string source = R"(func test():
	var dict = {'a': {'b': {'c': {'d': 1}}}}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);
	auto *dict_expr = dynamic_cast<DictionaryExpr *>(var_decl->initializer.get());
	assert(dict_expr != nullptr);
	assert(dict_expr->pairs.size() == 1);

	// Value should be a nested dictionary
	auto *nested_dict1 = dynamic_cast<DictionaryExpr *>(dict_expr->pairs[0].value.get());
	assert(nested_dict1 != nullptr);
	assert(nested_dict1->pairs.size() == 1);

	auto *nested_dict2 = dynamic_cast<DictionaryExpr *>(nested_dict1->pairs[0].value.get());
	assert(nested_dict2 != nullptr);
	assert(nested_dict2->pairs.size() == 1);

	auto *nested_dict3 = dynamic_cast<DictionaryExpr *>(nested_dict2->pairs[0].value.get());
	assert(nested_dict3 != nullptr);
	assert(nested_dict3->pairs.size() == 1);

	std::cout << "  ✓ Deeply nested dictionary test passed" << std::endl;
}

void test_large_array() {
	std::cout << "Testing large array..." << std::endl;

	std::string source = R"(func test():
	var arr = [0, 1, 2, 3, 4, 5, 6, 7, 8]
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);
	auto *array_expr = dynamic_cast<ArrayExpr *>(var_decl->initializer.get());
	assert(array_expr != nullptr);
	assert(array_expr->elements.size() == 9);

	std::cout << "  ✓ Large array test passed" << std::endl;
}

void test_array_of_arrays_of_dictionaries() {
	std::cout << "Testing array of arrays of dictionaries..." << std::endl;

	std::string source = R"(func test():
	var arr = [[{'a': 1}], [{'b': 2}], [{'c': 3}]]
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);
	auto *array_expr = dynamic_cast<ArrayExpr *>(var_decl->initializer.get());
	assert(array_expr != nullptr);
	assert(array_expr->elements.size() == 3);

	// Each element should be an array
	for (size_t i = 0; i < array_expr->elements.size(); i++) {
		auto *nested_array = dynamic_cast<ArrayExpr *>(array_expr->elements[i].get());
		assert(nested_array != nullptr);
		assert(nested_array->elements.size() == 1);

		// Each nested array should contain a dictionary
		auto *dict = dynamic_cast<DictionaryExpr *>(nested_array->elements[0].get());
		assert(dict != nullptr);
		assert(dict->pairs.size() == 1);
	}

	std::cout << "  ✓ Array of arrays of dictionaries test passed" << std::endl;
}

void test_dictionary_with_nested_dict_and_array() {
	std::cout << "Testing dictionary with nested dict and array..." << std::endl;

	std::string source = R"(func test():
	var dict = {'a': 1, 'b': {'c': 88, 'd': 22}, 'i': [1, 2, 3], 'z': {}}
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);
	auto *dict_expr = dynamic_cast<DictionaryExpr *>(var_decl->initializer.get());
	assert(dict_expr != nullptr);
	assert(dict_expr->pairs.size() == 4);

	// Check for nested dictionary
	bool found_nested_dict = false;
	bool found_array = false;
	bool found_empty_dict = false;

	for (const auto &pair : dict_expr->pairs) {
		if (auto *key = dynamic_cast<LiteralExpr *>(pair.key.get())) {
			if (std::get<std::string>(key->value) == "b") {
				auto *nested = dynamic_cast<DictionaryExpr *>(pair.value.get());
				assert(nested != nullptr);
				found_nested_dict = true;
			} else if (std::get<std::string>(key->value) == "i") {
				auto *arr = dynamic_cast<ArrayExpr *>(pair.value.get());
				assert(arr != nullptr);
				found_array = true;
			} else if (std::get<std::string>(key->value) == "z") {
				auto *empty = dynamic_cast<DictionaryExpr *>(pair.value.get());
				assert(empty != nullptr);
				assert(empty->pairs.size() == 0);
				found_empty_dict = true;
			}
		}
	}

	assert(found_nested_dict);
	assert(found_array);
	assert(found_empty_dict);

	std::cout << "  ✓ Dictionary with nested dict and array test passed" << std::endl;
}

void test_array_index_assignment() {
	std::cout << "Testing array index assignment..." << std::endl;

	std::string source = R"(func test():
	var arr = [1, 2, 3]
	arr[0] = 42
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 3);

	// Check assignment statement
	auto *assign = dynamic_cast<AssignStmt *>(program.functions[0].body[1].get());
	assert(assign != nullptr);
	assert(assign->value != nullptr);

	// Check that target is an IndexExpr
	auto *index_expr = dynamic_cast<IndexExpr *>(assign->target.get());
	assert(index_expr != nullptr);
	assert(index_expr->object != nullptr);
	assert(index_expr->index != nullptr);

	// Check object is a variable
	auto *var_expr = dynamic_cast<VariableExpr *>(index_expr->object.get());
	assert(var_expr != nullptr);
	assert(var_expr->name == "arr");

	// Check index is a literal
	auto *idx_lit = dynamic_cast<LiteralExpr *>(index_expr->index.get());
	assert(idx_lit != nullptr);
	assert(idx_lit->lit_type == LiteralExpr::Type::INTEGER);
	assert(std::get<int64_t>(idx_lit->value) == 0);

	// Check value is a literal
	auto *val_lit = dynamic_cast<LiteralExpr *>(assign->value.get());
	assert(val_lit != nullptr);
	assert(val_lit->lit_type == LiteralExpr::Type::INTEGER);
	assert(std::get<int64_t>(val_lit->value) == 42);

	std::cout << "  ✓ Array index assignment test passed" << std::endl;
}

void test_dictionary_index_assignment() {
	std::cout << "Testing dictionary index assignment..." << std::endl;

	std::string source = R"(func test():
	var dict = {"key1": "value1"}
	dict["key2"] = "value2"
	return dict
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 3);

	// Check assignment statement
	auto *assign = dynamic_cast<AssignStmt *>(program.functions[0].body[1].get());
	assert(assign != nullptr);

	// Check that target is an IndexExpr
	auto *index_expr = dynamic_cast<IndexExpr *>(assign->target.get());
	assert(index_expr != nullptr);

	// Check object is a variable
	auto *var_expr = dynamic_cast<VariableExpr *>(index_expr->object.get());
	assert(var_expr != nullptr);
	assert(var_expr->name == "dict");

	// Check index is a string literal
	auto *idx_lit = dynamic_cast<LiteralExpr *>(index_expr->index.get());
	assert(idx_lit != nullptr);
	assert(idx_lit->lit_type == LiteralExpr::Type::STRING);
	assert(std::get<std::string>(idx_lit->value) == "key2");

	std::cout << "  ✓ Dictionary index assignment test passed" << std::endl;
}

void test_index_assignment_with_expression() {
	std::cout << "Testing index assignment with expression..." << std::endl;

	std::string source = R"(func test():
	var arr = [1, 2, 3]
	var i = 1
	arr[i + 1] = 10
	return arr
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 4);

	// Check assignment statement
	auto *assign = dynamic_cast<AssignStmt *>(program.functions[0].body[2].get());
	assert(assign != nullptr);

	// Check that target is an IndexExpr
	auto *index_expr = dynamic_cast<IndexExpr *>(assign->target.get());
	assert(index_expr != nullptr);

	// Check index is a binary expression
	auto *bin_expr = dynamic_cast<BinaryExpr *>(index_expr->index.get());
	assert(bin_expr != nullptr);
	assert(bin_expr->op == BinaryExpr::Op::ADD);

	std::cout << "  ✓ Index assignment with expression test passed" << std::endl;
}

void test_variable_assignment_still_works() {
	std::cout << "Testing variable assignment still works..." << std::endl;

	std::string source = R"(func test():
	var x = 5
	x = 10
	return x
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 3);

	// Check assignment statement
	auto *assign = dynamic_cast<AssignStmt *>(program.functions[0].body[1].get());
	assert(assign != nullptr);

	// Check that target is a VariableExpr (not IndexExpr)
	auto *var_expr = dynamic_cast<VariableExpr *>(assign->target.get());
	assert(var_expr != nullptr);
	assert(var_expr->name == "x");

	// Check value
	auto *val_lit = dynamic_cast<LiteralExpr *>(assign->value.get());
	assert(val_lit != nullptr);
	assert(std::get<int64_t>(val_lit->value) == 10);

	std::cout << "  ✓ Variable assignment still works test passed" << std::endl;
}

int main() {
	std::cout << "\n=== Running Array and Dictionary Literal Parser Tests ===" << std::endl;

	try {
		test_array_literal();
		test_empty_array();
		test_dictionary_literal();
		test_empty_dictionary();
		test_nested_structures();
		test_single_element_array();
		test_single_pair_dictionary();
		test_array_with_expressions();
		test_dictionary_with_expressions();
		test_array_as_return_value();
		test_dictionary_as_return_value();
		test_deeply_nested_dictionary();
		test_large_array();
		test_array_of_arrays_of_dictionaries();
		test_dictionary_with_nested_dict_and_array();

		// Index assignment tests
		test_array_index_assignment();
		test_dictionary_index_assignment();
		test_index_assignment_with_expression();
		test_variable_assignment_still_works();

		std::cout << "\n✅ All array and dictionary literal parser tests passed!" << std::endl;
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}
