#include "../lexer.h"
#include "../parser.h"
#include <cassert>
#include <iostream>

using namespace gdscript;

void test_simple_function() {
	std::cout << "Testing simple function..." << std::endl;

	std::string source = R"(func add(a, b):
	return a + b
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions.size() == 1);
	assert(program.functions[0].name == "add");
	assert(program.functions[0].parameters.size() == 2);
	assert(program.functions[0].parameters[0].name == "a");
	assert(program.functions[0].parameters[1].name == "b");
	assert(program.functions[0].body.size() == 1);

	// Check return statement
	auto *ret_stmt = dynamic_cast<ReturnStmt *>(program.functions[0].body[0].get());
	assert(ret_stmt != nullptr);
	assert(ret_stmt->value != nullptr);

	std::cout << "  ✓ Simple function test passed" << std::endl;
}

void test_variable_declaration() {
	std::cout << "Testing variable declaration..." << std::endl;

	std::string source = R"(func test():
	var x = 10
	var y
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 2);

	auto *var1 = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var1 != nullptr);
	assert(var1->name == "x");
	assert(var1->initializer != nullptr);

	auto *var2 = dynamic_cast<VarDeclStmt *>(program.functions[0].body[1].get());
	assert(var2 != nullptr);
	assert(var2->name == "y");
	assert(var2->initializer == nullptr);

	std::cout << "  ✓ Variable declaration test passed" << std::endl;
}

void test_if_statement() {
	std::cout << "Testing if statement..." << std::endl;

	std::string source = R"(func test(x):
	if x > 0:
		return 1
	else:
		return -1
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 1);

	auto *if_stmt = dynamic_cast<IfStmt *>(program.functions[0].body[0].get());
	assert(if_stmt != nullptr);
	assert(if_stmt->condition != nullptr);
	assert(if_stmt->then_branch.size() == 1);
	assert(if_stmt->else_branch.size() == 1);

	std::cout << "  ✓ If statement test passed" << std::endl;
}

void test_while_loop() {
	std::cout << "Testing while loop..." << std::endl;

	std::string source = R"(func test():
	var i = 0
	while i < 10:
		i = i + 1
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 2);

	auto *while_stmt = dynamic_cast<WhileStmt *>(program.functions[0].body[1].get());
	assert(while_stmt != nullptr);
	assert(while_stmt->condition != nullptr);
	assert(while_stmt->body.size() == 1);

	std::cout << "  ✓ While loop test passed" << std::endl;
}

void test_expressions() {
	std::cout << "Testing expressions..." << std::endl;

	std::string source = R"(func test():
	var a = 1 + 2 * 3
	var b = (1 + 2) * 3
	var c = x and y or z
	var d = not x
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 4);

	// Check that all are variable declarations with expressions
	for (int i = 0; i < 4; i++) {
		auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[i].get());
		assert(var_decl != nullptr);
		assert(var_decl->initializer != nullptr);
	}

	std::cout << "  ✓ Expressions test passed" << std::endl;
}

void test_function_call() {
	std::cout << "Testing function calls..." << std::endl;

	std::string source = R"(func test():
	var result = add(1, 2)
	print("hello")
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 2);

	// First statement: var result = add(1, 2)
	auto *var_decl = dynamic_cast<VarDeclStmt *>(program.functions[0].body[0].get());
	assert(var_decl != nullptr);

	auto *call_expr = dynamic_cast<CallExpr *>(var_decl->initializer.get());
	assert(call_expr != nullptr);
	assert(call_expr->function_name == "add");
	assert(call_expr->arguments.size() == 2);

	std::cout << "  ✓ Function call test passed" << std::endl;
}

void test_method_call() {
	std::cout << "Testing method calls..." << std::endl;

	std::string source = R"(func test():
	var node = get_node("/root")
	node.set_position(Vector2(0, 0))
	var pos = node.get_position()
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions[0].body.size() == 3);

	// Second statement: node.set_position(...)
	auto *expr_stmt = dynamic_cast<ExprStmt *>(program.functions[0].body[1].get());
	assert(expr_stmt != nullptr);

	auto *member_call = dynamic_cast<MemberCallExpr *>(expr_stmt->expression.get());
	assert(member_call != nullptr);
	assert(member_call->member_name == "set_position");
	assert(member_call->arguments.size() == 1);

	std::cout << "  ✓ Method call test passed" << std::endl;
}

void test_nested_control_flow() {
	std::cout << "Testing nested control flow..." << std::endl;

	std::string source = R"(func test(x):
	if x > 0:
		while x > 0:
			x = x - 1
			if x == 5:
				break
	else:
		return -1
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	auto *if_stmt = dynamic_cast<IfStmt *>(program.functions[0].body[0].get());
	assert(if_stmt != nullptr);
	assert(if_stmt->then_branch.size() == 1);

	auto *while_stmt = dynamic_cast<WhileStmt *>(if_stmt->then_branch[0].get());
	assert(while_stmt != nullptr);
	assert(while_stmt->body.size() == 2);

	std::cout << "  ✓ Nested control flow test passed" << std::endl;
}

void test_multiple_functions() {
	std::cout << "Testing multiple functions..." << std::endl;

	std::string source = R"(func add(a, b):
	return a + b

func multiply(a, b):
	return a * b

func main():
	var x = add(10, 20)
	var y = multiply(x, 2)
	return y
)";

	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	assert(program.functions.size() == 3);
	assert(program.functions[0].name == "add");
	assert(program.functions[1].name == "multiply");
	assert(program.functions[2].name == "main");

	std::cout << "  ✓ Multiple functions test passed" << std::endl;
}

int main() {
	std::cout << "\n=== Running Parser Tests ===" << std::endl;

	try {
		test_simple_function();
		test_variable_declaration();
		test_if_statement();
		test_while_loop();
		test_expressions();
		test_function_call();
		test_method_call();
		test_nested_control_flow();
		test_multiple_functions();

		std::cout << "\n✅ All parser tests passed!" << std::endl;
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}
