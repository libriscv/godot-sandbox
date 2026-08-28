// Diagnostics: what a failed compile can tell an editor.
//
// The .sgd script language extension underlines errors in the Godot editor, and
// to do that it needs the line and column as numbers rather than as prose
// inside the formatted message. Compiler::get_error_info() is that channel, so
// these tests pin down that every phase's errors arrive with their location
// intact, that a successful compile clears it, and that the formatted message
// still quotes the offending source line.
#include "../compiler.h"
#include "../compiler_exception.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace gdscript;

// -= Helpers =-

// Compile source that is expected to fail, and hand back what the compiler knew
// about the failure.
static CompilerError failing_compile(const std::string& source) {
	Compiler compiler;
	CompilerOptions options;
	options.output_elf = true;
	const std::vector<uint8_t> elf = compiler.compile(source, options);
	assert(elf.empty() && "expected this program to fail to compile");
	const CompilerError& error = compiler.get_error_info();
	assert(error.has_error);
	return error;
}

static bool contains(const std::string& haystack, const std::string& needle) {
	return haystack.find(needle) != std::string::npos;
}

// -= Tests =-

static void test_success_leaves_no_error() {
	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile("func f():\n\treturn 1\n");
	assert(!elf.empty());
	assert(!compiler.get_error_info().has_error);
	assert(compiler.get_error().empty());

	std::cout << "  ✓ a successful compile reports no error" << std::endl;
}

static void test_error_is_cleared_by_a_later_success() {
	// The same Compiler reused: a stale error would make the editor underline a
	// line in a file that now compiles.
	Compiler compiler;
	assert(compiler.compile("func f(:\n\treturn 1\n").empty());
	assert(compiler.get_error_info().has_error);

	assert(!compiler.compile("func f():\n\treturn 1\n").empty());
	assert(!compiler.get_error_info().has_error);
	assert(compiler.get_error_info().line == 0);
	assert(compiler.get_error_info().message.empty());

	std::cout << "  ✓ a later success clears the previous error" << std::endl;
}

static void test_lexer_error_has_a_location() {
	const CompilerError error = failing_compile("func f():\n\tvar s = \"unterminated\n");
	assert(error.type == ErrorType::LEXER_ERROR);
	assert(error.line == 2);

	std::cout << "  ✓ a lexer error carries its line" << std::endl;
}

static void test_parser_error_has_a_location() {
	const CompilerError error = failing_compile("func f():\n\treturn 1\n\nfunc g)\n");
	assert(error.type == ErrorType::PARSER_ERROR);
	assert(error.line == 4);
	assert(error.column > 0);

	std::cout << "  ✓ a parser error carries its line and column" << std::endl;
}

static void test_codegen_error_names_the_function() {
	// An unknown struct field is caught while lowering to IR, the one place the
	// compiler knows a location, the enclosing function and a hint all at once.
	const CompilerError error = failing_compile(
		"struct Point:\n"
		"\tvar x = 0\n"
		"\n"
		"func f():\n"
		"\tvar p = Point.new()\n"
		"\treturn p.z\n");
	assert(error.type == ErrorType::CODEGEN_ERROR);
	assert(error.line == 6);
	assert(error.column > 0);
	assert(error.function == "f");
	assert(contains(error.message, "z"));
	assert(contains(error.hint, "x"));

	std::cout << "  ✓ a codegen error names its function and hint" << std::endl;
}

static void test_message_has_no_type_prefix() {
	// get_error() formats "[TYPE] message (line N)" for a terminal. The editor
	// puts the message in its own error field and the line in its own gutter,
	// so message must be the message alone.
	const CompilerError error = failing_compile("func f():\n\treturn 1\n\nfunc g)\n");
	assert(!error.message.empty());
	assert(error.message.front() != '[');
	assert(!contains(error.message, "line "));

	std::cout << "  ✓ the structured message is free of formatting" << std::endl;
}

static void test_formatted_message_quotes_the_source_line() {
	// Only compile() has the source text, so it is the one place that can put
	// the offending line under the message for a terminal user.
	Compiler compiler;
	assert(compiler.compile("func f():\n\treturn 1\n\nfunc g)\n").empty());
	const std::string message = compiler.get_error();
	assert(contains(message, "[Parser Error]"));
	assert(contains(message, "func g)"));
	assert(contains(message, "^"));

	std::cout << "  ✓ the formatted message quotes the offending line" << std::endl;
}

static void test_unclosed_bracket_points_at_the_bracket() {
	// An unclosed '(' swallows every newline after it, so EOF is the symptom,
	// not the cause: report the bracket's position.
	const CompilerError error = failing_compile("func f():\n\treturn 1\n\nfunc g(\n");
	assert(error.type == ErrorType::LEXER_ERROR);
	assert(error.line == 4);
	assert(error.column == 7);
	assert(contains(error.message, "Unclosed '('"));

	std::cout << "  ✓ an unclosed bracket is reported where it was opened" << std::endl;
}

static void test_source_line_survives_crlf() {
	// A file saved on Windows must not put a carriage return in the snippet,
	// which would land the caret on its own line.
	Compiler compiler;
	assert(compiler.compile("func f():\r\n\treturn 1\r\n\r\nfunc g(\r\n").empty());
	const std::string message = compiler.get_error();
	assert(!contains(message, "\r"));

	std::cout << "  ✓ a CRLF source line is quoted without its carriage return" << std::endl;
}

// The boxed ABI has sixteen total slots: seven pointers in a1-a7 and nine on
// the entry stack. A seventeenth parameter must still fail at its declaration.
static void test_too_many_parameters_is_refused() {
	const CompilerError error = failing_compile(
		"func f(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q):\n\treturn q\n");
	assert(error.type == ErrorType::CODEGEN_ERROR);
	assert(error.line == 1);
	assert(contains(error.message, "at most 16"));

	std::cout << "  ✓ a function with more than sixteen parameters is refused" << std::endl;
}

// A project `class_name` script is not an engine singleton, so `Other.helper()`
// used to lower to a property read on the owner Node and answer null at run
// time. The file compiled clean; the breakage was entirely at run time, which is
// the failure mode a compiler exists to remove.
static void test_a_script_class_outside_the_program_is_refused() {
	Compiler compiler;
	CompilerOptions options;
	options.global_script_classes.emplace_back("Other", "res://other.gd");

	assert(compiler.compile("func f():\n\treturn Other.helper()\n", options).empty());
	const CompilerError &call = compiler.get_error_info();
	assert(call.has_error);
	assert(call.type == ErrorType::CODEGEN_ERROR);
	assert(call.line == 2);
	assert(contains(call.message, "none of its body is compiled into this program"));
	assert(contains(call.hint, "Other.new()"));

	// Constants and nested enums miscompiled the same way, one VGET per dot.
	assert(compiler.compile("func f():\n\treturn Other.Shape.BOX\n", options).empty());
	assert(contains(compiler.get_error_info().message, "'Other'"));

	// Instantiating one is still how a script reaches another script's body.
	assert(!compiler.compile("func f():\n\treturn Other.new()\n", options).empty());

	std::cout << "  \u2713 reaching into a script class this program does not contain is refused"
			  << std::endl;
}

int main() {
	std::cout << "=== Diagnostics Tests ===" << std::endl << std::endl;

	test_success_leaves_no_error();
	test_error_is_cleared_by_a_later_success();
	test_lexer_error_has_a_location();
	test_parser_error_has_a_location();
	test_unclosed_bracket_points_at_the_bracket();
	test_codegen_error_names_the_function();
	test_message_has_no_type_prefix();
	test_formatted_message_quotes_the_source_line();
	test_source_line_survives_crlf();
	test_too_many_parameters_is_refused();
	test_a_script_class_outside_the_program_is_refused();

	std::cout << std::endl << "All diagnostics tests passed!" << std::endl;
	return 0;
}
