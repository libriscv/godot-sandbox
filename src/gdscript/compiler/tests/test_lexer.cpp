#include "../lexer.h"
#include "../compiler_exception.h"
#include <cassert>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <string>

using namespace gdscript;

// Tokenize source that is expected to be rejected, and hand back the error.
static CompilerException lex_failure(const std::string& source) {
	try {
		Lexer lexer(source);
		lexer.tokenize();
	} catch (const CompilerException& e) {
		return e;
	}
	assert(false && "expected this source to fail to tokenize");
	throw std::runtime_error("unreachable");
}

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

static std::string lex_string(const std::string& source) {
	Lexer lexer(source);
	auto tokens = lexer.tokenize();
	assert(tokens[0].type == TokenType::STRING);
	return std::get<std::string>(tokens[0].value);
}

void test_control_escapes() {
	std::cout << "Testing control escapes..." << std::endl;

	assert(lex_string(R"("\a")") == "\a");
	assert(lex_string(R"("\b")") == "\b");
	assert(lex_string(R"("\f")") == "\f");
	assert(lex_string(R"("\v")") == "\v");
	assert(lex_string(R"("\r")") == "\r");

	// Unknown escapes are rejected.
	assert(lex_failure(R"("\x41")").error_type() == ErrorType::LEXER_ERROR);
	lex_failure(R"("\0")");
	lex_failure(R"("\q")");

	// r"..." keeps the backslash it is written with, escape or not.
	assert(lex_string(R"(r"\x41")") == "\\x41");

	std::cout << "  ✓ Control escapes test passed" << std::endl;
}

void test_line_continuation_in_string() {
	std::cout << "Testing string line continuation..." << std::endl;

	// Backslash-newline joins lines; indentation of the continuation is kept.
	assert(lex_string("\"a\\\n\tb\"") == "a\tb");
	assert(lex_string("\"a\\\r\n b\"") == "a b");
	// A lone CR is not a continuation.
	lex_failure("\"a\\\rb\"");

	std::cout << "  ✓ String line continuation test passed" << std::endl;
}

void test_unicode_escapes() {
	std::cout << "Testing unicode escapes..." << std::endl;

	assert(lex_string(R"("é")") == "\xc3\xa9");
	assert(lex_string(R"("\U0000e9")") == "\xc3\xa9");
	assert(lex_string(R"("A")") == "A");
	assert(lex_string(R"("\U01F600")") == "\xf0\x9f\x98\x80");

	// Surrogate pair via two \u escapes.
	assert(lex_string(R"("😀")") == "\xf0\x9f\x98\x80");

	// Incomplete/misordered surrogates are rejected.
	lex_failure(R"("\ud83d")");
	lex_failure(R"("\ud83dx")");
	lex_failure(R"("\ud83dA")");
	lex_failure(R"("\ude00")");

	// Exactly 4 / 6 hex digits consumed.
	assert(lex_string(R"("\u00e941")") == "\xc3\xa9" "41");
	assert(lex_string(R"("\U0000e941")") == "\xc3\xa9" "41");
	lex_failure(R"("\u00e")");
	lex_failure(R"("\U00e9")");

	// Out of range.
	lex_failure(R"("\U110000")");

	assert(lex_string(R"("aéb")") == "a\xc3\xa9" "b");

	std::cout << "  ✓ Unicode escapes test passed" << std::endl;
}

void test_comments() {
	std::cout << "Testing comments..." << std::endl;

	Lexer lexer("# This is a comment\nvar x = 10  # inline comment\n");
	auto tokens = lexer.tokenize();

	// Should skip comments entirely
	assert(tokens[0].type == TokenType::NEWLINE || tokens[0].type == TokenType::VAR);

	std::cout << "  ✓ Comments test passed" << std::endl;
}

void test_bitwise_operators() {
	Lexer lexer("a & b | c ^ ~d << e >> f\n");
	auto tokens = lexer.tokenize();

	assert(tokens[1].type == TokenType::BIT_AND);
	assert(tokens[3].type == TokenType::BIT_OR);
	assert(tokens[5].type == TokenType::BIT_XOR);
	assert(tokens[6].type == TokenType::BIT_NOT);
	assert(tokens[8].type == TokenType::SHIFT_LEFT);
	assert(tokens[10].type == TokenType::SHIFT_RIGHT);

	std::cout << "  ✓ Bitwise operators test passed" << std::endl;
}

void test_bitwise_compound_assignment() {
	Lexer lexer("a &= 1\nb |= 2\nc ^= 3\nd <<= 4\ne >>= 5\n");
	auto tokens = lexer.tokenize();

	assert(tokens[1].type == TokenType::BIT_AND_ASSIGN);
	assert(tokens[5].type == TokenType::BIT_OR_ASSIGN);
	assert(tokens[9].type == TokenType::BIT_XOR_ASSIGN);
	assert(tokens[13].type == TokenType::SHIFT_LEFT_ASSIGN);
	assert(tokens[17].type == TokenType::SHIFT_RIGHT_ASSIGN);

	std::cout << "  ✓ Bitwise compound assignment test passed" << std::endl;
}

void test_logical_operator_aliases() {
	// '&&', '||' and '!' are accepted as aliases for 'and', 'or' and 'not'
	Lexer lexer("a && b || !c != d\n");
	auto tokens = lexer.tokenize();

	assert(tokens[1].type == TokenType::AND);
	assert(tokens[3].type == TokenType::OR);
	assert(tokens[4].type == TokenType::NOT);
	assert(tokens[6].type == TokenType::NOT_EQUAL);

	std::cout << "  ✓ Logical operator aliases test passed" << std::endl;
}

void test_radix_literals() {
	Lexer lexer("0xFF 0Xdead_beef 0b1011 0B11\n");
	auto tokens = lexer.tokenize();

	assert(tokens[0].type == TokenType::INTEGER);
	assert(std::get<int64_t>(tokens[0].value) == 255);
	assert(std::get<int64_t>(tokens[1].value) == 0xdeadbeefLL);
	assert(std::get<int64_t>(tokens[2].value) == 11);
	assert(std::get<int64_t>(tokens[3].value) == 3);

	std::cout << "  ✓ Radix literals test passed" << std::endl;
}

void test_numeric_separators_and_exponents() {
	Lexer lexer("1_000_000 1.5e3 2.5E-3 7e2\n");
	auto tokens = lexer.tokenize();

	assert(tokens[0].type == TokenType::INTEGER);
	assert(std::get<int64_t>(tokens[0].value) == 1000000);
	assert(tokens[1].type == TokenType::FLOAT);
	assert(std::get<double>(tokens[1].value) == 1500.0);
	assert(tokens[2].type == TokenType::FLOAT);
	assert(std::abs(std::get<double>(tokens[2].value) - 0.0025) < 1e-12);
	assert(tokens[3].type == TokenType::FLOAT);
	assert(std::get<double>(tokens[3].value) == 700.0);

	std::cout << "  ✓ Numeric separators and exponents test passed" << std::endl;
}

void test_match_keyword() {
	Lexer lexer("match x:\n");
	auto tokens = lexer.tokenize();

	assert(tokens[0].type == TokenType::MATCH);

	std::cout << "  ✓ Match keyword test passed" << std::endl;
}

void test_triple_quoted_strings() {
	std::cout << "Testing triple-quoted strings..." << std::endl;

	// A triple quote is the one string that may hold a raw newline, which is
	// what the .sgd editor's string delimiters have always advertised.
	Lexer lexer("\"\"\"one\ntwo\"\"\"\n");
	auto tokens = lexer.tokenize();

	assert(tokens[0].type == TokenType::STRING);
	assert(std::get<std::string>(tokens[0].value) == "one\ntwo");

	// The lines it spans still count, so an error below it is reported on the
	// line the user is looking at.
	Lexer counting("\"\"\"one\ntwo\"\"\"\nx\n");
	auto counted = counting.tokenize();
	bool found_x = false;
	for (const auto& tok : counted) {
		if (tok.type == TokenType::IDENTIFIER && tok.lexeme == "x") {
			assert(tok.line == 3);
			found_x = true;
		}
	}
	assert(found_x);

	// Single quotes open one just the same, and an empty one is still a string.
	Lexer single("'''a'''");
	assert(std::get<std::string>(single.tokenize()[0].value) == "a");
	Lexer empty("\"\"\"\"\"\"");
	assert(std::get<std::string>(empty.tokenize()[0].value).empty());

	std::cout << "  ✓ Triple-quoted strings test passed" << std::endl;
}

void test_unterminated_string_stops_at_the_line() {
	std::cout << "Testing unterminated strings..." << std::endl;

	// A plain string ends at its own line, so one stray quote does not swallow
	// the rest of the file, and the error points at the quote that opened it
	// rather than at the end of input.
	const CompilerException error = lex_failure("func f():\n\tvar s = \"oops\n\treturn s\n");
	assert(error.error_type() == ErrorType::LEXER_ERROR);
	assert(error.line() == 2);
	assert(error.column() == 10);

	// An unterminated triple-quoted string is reported where it opened too.
	const CompilerException triple = lex_failure("\n\"\"\"oops\nand more\n");
	assert(triple.line() == 2);
	assert(triple.column() == 1);

	std::cout << "  ✓ Unterminated string test passed" << std::endl;
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
		test_control_escapes();
		test_line_continuation_in_string();
		test_unicode_escapes();
		test_comments();
		test_bitwise_operators();
		test_bitwise_compound_assignment();
		test_logical_operator_aliases();
		test_radix_literals();
		test_numeric_separators_and_exponents();
		test_match_keyword();
		test_triple_quoted_strings();
		test_unterminated_string_stops_at_the_line();

		std::cout << "\n✅ All lexer tests passed!" << std::endl;
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}
