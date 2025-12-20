#include "../lexer.h"
#include <cassert>
#include <iostream>

using namespace gdscript;

void test_basic_tokens() {
	std::cout << "Testing basic tokens..." << std::endl;

	Lexer lexer("func main():\n\tpass");
	auto tokens = lexer.tokenize();

	// Find specific tokens we care about (don't rely on exact count)
	bool found_func = false, found_main = false, found_lparen = false;
	bool found_rparen = false, found_colon = false, found_pass = false;

	for (const auto& tok : tokens) {
		if (tok.type == TokenType::FUNC) found_func = true;
		if (tok.type == TokenType::IDENTIFIER && tok.lexeme == "main") found_main = true;
		if (tok.type == TokenType::LPAREN) found_lparen = true;
		if (tok.type == TokenType::RPAREN) found_rparen = true;
		if (tok.type == TokenType::COLON) found_colon = true;
		if (tok.type == TokenType::PASS) found_pass = true;
	}

	assert(found_func);
	assert(found_main);
	assert(found_lparen);
	assert(found_rparen);
	assert(found_colon);
	assert(found_pass);

	std::cout << "  ✓ Basic tokens test passed" << std::endl;
}

void test_indentation() {
	std::cout << "Testing indentation..." << std::endl;

	std::string source = R"(func test():
	var x = 1
	if x > 0:
		return x
)";

	Lexer lexer(source);
	auto tokens = lexer.tokenize();

	// Check for INDENT and DEDENT tokens
	int indent_count = 0;
	int dedent_count = 0;
	for (const auto& tok : tokens) {
		if (tok.type == TokenType::INDENT) indent_count++;
		if (tok.type == TokenType::DEDENT) dedent_count++;
	}

	assert(indent_count == 2); // After function def, after if
	assert(dedent_count == 2); // Matching dedents

	std::cout << "  ✓ Indentation test passed" << std::endl;
}

void test_operators() {
	std::cout << "Testing operators..." << std::endl;

	Lexer lexer("x = a + b * c - d / e % f");
	auto tokens = lexer.tokenize();

	assert(tokens[1].type == TokenType::ASSIGN);
	assert(tokens[3].type == TokenType::PLUS);
	assert(tokens[5].type == TokenType::MULTIPLY);
	assert(tokens[7].type == TokenType::MINUS);
	assert(tokens[9].type == TokenType::DIVIDE);
	assert(tokens[11].type == TokenType::MODULO);

	std::cout << "  ✓ Operators test passed" << std::endl;
}

void test_comparison_operators() {
	std::cout << "Testing comparison operators..." << std::endl;

	Lexer lexer("a == b != c < d <= e > f >= g");
	auto tokens = lexer.tokenize();

	assert(tokens[1].type == TokenType::EQUAL);
	assert(tokens[3].type == TokenType::NOT_EQUAL);
	assert(tokens[5].type == TokenType::LESS);
	assert(tokens[7].type == TokenType::LESS_EQUAL);
	assert(tokens[9].type == TokenType::GREATER);
	assert(tokens[11].type == TokenType::GREATER_EQUAL);

	std::cout << "  ✓ Comparison operators test passed" << std::endl;
}

void test_literals() {
	std::cout << "Testing literals..." << std::endl;

	Lexer lexer(R"(42 3.14 "hello" 'world' true false null)");
	auto tokens = lexer.tokenize();

	assert(tokens[0].type == TokenType::INTEGER);
	assert(std::get<int64_t>(tokens[0].value) == 42);

	assert(tokens[1].type == TokenType::FLOAT);
	assert(std::get<double>(tokens[1].value) == 3.14);

	assert(tokens[2].type == TokenType::STRING);
	assert(std::get<std::string>(tokens[2].value) == "hello");

	assert(tokens[3].type == TokenType::STRING);
	assert(std::get<std::string>(tokens[3].value) == "world");

	assert(tokens[4].type == TokenType::TRUE);
	assert(tokens[5].type == TokenType::FALSE);
	assert(tokens[6].type == TokenType::NULL_VAL);

	std::cout << "  ✓ Literals test passed" << std::endl;
}

void test_keywords() {
	std::cout << "Testing keywords..." << std::endl;

	Lexer lexer("func var return if else elif while for break continue pass and or not");
	auto tokens = lexer.tokenize();

	assert(tokens[0].type == TokenType::FUNC);
	assert(tokens[1].type == TokenType::VAR);
	assert(tokens[2].type == TokenType::RETURN);
	assert(tokens[3].type == TokenType::IF);
	assert(tokens[4].type == TokenType::ELSE);
	assert(tokens[5].type == TokenType::ELIF);
	assert(tokens[6].type == TokenType::WHILE);
	assert(tokens[7].type == TokenType::FOR);
	assert(tokens[8].type == TokenType::BREAK);
	assert(tokens[9].type == TokenType::CONTINUE);
	assert(tokens[10].type == TokenType::PASS);
	assert(tokens[11].type == TokenType::AND);
	assert(tokens[12].type == TokenType::OR);
	assert(tokens[13].type == TokenType::NOT);

	std::cout << "  ✓ Keywords test passed" << std::endl;
}

void test_string_escapes() {
	std::cout << "Testing string escapes..." << std::endl;

	Lexer lexer(R"("hello\nworld\t\"test\"")");
	auto tokens = lexer.tokenize();

	assert(tokens[0].type == TokenType::STRING);
	std::string expected = "hello\nworld\t\"test\"";
	assert(std::get<std::string>(tokens[0].value) == expected);

	std::cout << "  ✓ String escapes test passed" << std::endl;
}

void test_comments() {
	std::cout << "Testing comments..." << std::endl;

	Lexer lexer("# This is a comment\nvar x = 10  # inline comment\n");
	auto tokens = lexer.tokenize();

	// Should skip comments entirely
	assert(tokens[0].type == TokenType::NEWLINE || tokens[0].type == TokenType::VAR);

	std::cout << "  ✓ Comments test passed" << std::endl;
}

int main() {
	std::cout << "\n=== Running Lexer Tests ===" << std::endl;

	try {
		test_basic_tokens();
		test_indentation();
		test_operators();
		test_comparison_operators();
		test_literals();
		test_keywords();
		test_string_escapes();
		test_comments();

		std::cout << "\n✅ All lexer tests passed!" << std::endl;
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}
