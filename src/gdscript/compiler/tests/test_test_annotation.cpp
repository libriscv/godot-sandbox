// `@test`: in-document test cases.
//
// A `.sgd` marks an argless top-level `func` with `@test`, and the compiler
// publishes it as a test case beside the ELF. The function stays an ordinary
// method -- same symbol, same signature, same arity check -- so the only new
// information is which functions are tests. The host runner calls them and
// counts exceptions; nothing about the run-time surface changes.
//
// Two things are pinned down here. First, where the annotation is refused:
// anywhere the runner would have to invent something (a parameter value, a
// resumption of a coroutine, an instance of a nested class). Second, that a
// shipping build (`emit_tests` off) leaves no trace of a test in the ELF, since
// the tests are dropped before codegen rather than filtered out of the table.
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../codegen.h"
#include "../function_signature.h"
#include "../lexer.h"
#include "../parser.h"
#include "../source_model.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

static IRProgram compile_to_ir(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	parser.set_doc_comments(lexer.doc_comments());
	Program program = parser.parse();
	CodeGenerator codegen;
	return codegen.generate(program);
}

static std::string rejection(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException& e) {
		return e.message();
	}
	return "";
}

static int rejection_line(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException& e) {
		return e.line();
	}
	return 0;
}

static const FunctionSignature& find_test(const IRProgram& ir, const std::string& name) {
	for (const FunctionSignature& test : ir.tests) {
		if (test.name == name) return test;
	}
	throw std::runtime_error("Test not found: " + name);
}

static bool has_function(const IRProgram& ir, const std::string& name) {
	return std::any_of(ir.functions.begin(), ir.functions.end(),
		[&name](const IRFunction& function) { return function.name == name; });
}

// -= Accepted placements =-

static void test_a_plain_function_becomes_a_test() {
	const IRProgram ir = compile_to_ir(
		"func helper() -> int:\n"
		"\treturn 1\n"
		"@test\n"
		"func it_adds():\n"
		"\tassert(helper() == 1)\n");
	assert(ir.tests.size() == 1);
	assert(ir.tests[0].name == "it_adds");
	assert(ir.tests[0].parameters.empty());
	// Still an ordinary method: symbol, signature and arity are untouched.
	assert(has_function(ir, "it_adds"));
	assert(std::any_of(ir.signatures.begin(), ir.signatures.end(),
		[](const FunctionSignature& s) { return s.name == "it_adds"; }));
}

static void test_a_static_function_becomes_a_test() {
	const IRProgram ir = compile_to_ir(
		"@test\n"
		"static func math_is_math():\n"
		"\tassert(1 + 1 == 2)\n");
	assert(ir.tests.size() == 1);
	assert(find_test(ir, "math_is_math").is_static);
}

static void test_tests_are_published_in_declaration_order() {
	const IRProgram ir = compile_to_ir(
		"@test\n"
		"func first():\n"
		"\tpass\n"
		"func not_a_test():\n"
		"\tpass\n"
		"@test\n"
		"func second():\n"
		"\tpass\n");
	assert(ir.tests.size() == 2);
	assert(ir.tests[0].name == "first");
	assert(ir.tests[1].name == "second");
}

static void test_a_test_carries_its_line_and_doc_comment() {
	const IRProgram ir = compile_to_ir(
		"extends Node\n"
		"\n"
		"@test\n"
		"## Members start at their declared defaults.\n"
		"func defaults_hold():\n"
		"\tpass\n");
	const FunctionSignature& test = find_test(ir, "defaults_hold");
	assert(test.line == 5);
	assert(test.description == "Members start at their declared defaults.");
}

static void test_an_annotation_stacks_with_others() {
	const IRProgram ir = compile_to_ir(
		"@tool\n"
		"@test\n"
		"func runs_in_the_editor_too():\n"
		"\tpass\n");
	assert(ir.tests.size() == 1);
}

// -= Refused placements =-

static void test_a_variable_is_refused() {
	assert(rejection("@test\nvar x = 1\n") ==
		"@test can only be applied to a function");
	assert(rejection("@test\nconst X = 1\n") ==
		"@test can only be applied to a function");
}

static void test_a_signal_is_refused() {
	assert(rejection("@test\nsignal fired()\n") ==
		"@test can only be applied to a function");
}

static void test_no_declaration_is_refused() {
	assert(rejection("@test\n") == "@test can only be applied to a function");
}

static void test_a_nested_class_method_is_refused() {
	const std::string source =
		"class Inner:\n"
		"\t@test\n"
		"\tfunc inner_test():\n"
		"\t\tpass\n";
	assert(rejection(source) == "@test is for a file-level function");
	// Reported where the annotation is, not where the file-level scan gave up.
	assert(rejection_line(source) == 2);
}

static void test_a_trait_method_is_refused() {
	assert(rejection(
		"trait Killable:\n"
		"\t@test\n"
		"\tfunc die() -> void\n") == "@test is for a file-level function");
}

static void test_a_parameter_is_refused() {
	assert(rejection(
		"@test\n"
		"func takes_one(value: int):\n"
		"\tpass\n") == "@test function 'takes_one' takes no parameters");
	// A default is still a parameter the runner would have to invent.
	assert(rejection(
		"@test\n"
		"func takes_default(value: int = 3):\n"
		"\tpass\n") == "@test function 'takes_default' takes no parameters");
}

static void test_a_coroutine_is_refused() {
	assert(rejection(
		"@test\n"
		"func waits():\n"
		"\tawait get_tree().process_frame\n") ==
		"@test function 'waits' cannot be a coroutine");
}

static void test_rpc_is_refused() {
	assert(rejection(
		"@rpc(\"any_peer\")\n"
		"@test\n"
		"func remote_test():\n"
		"\tpass\n") == "@test cannot be combined with @rpc");
}

static void test_arguments_are_refused() {
	assert(rejection(
		"@test(\"name\")\n"
		"func named():\n"
		"\tpass\n") == "@test takes no arguments");
}

static void test_a_duplicate_is_refused() {
	assert(rejection(
		"@test\n"
		"@test\n"
		"func twice():\n"
		"\tpass\n") == "@test can only be used once per function");
}

// -= Base sources =-

static void test_an_overridden_base_test_is_not_a_test() {
	// A displaced base implementation lands on a mangled symbol; only the
	// method visible on the final script is a test of it. Same rule @rpc uses.
	CompilerOptions options;
	options.output_elf = false;
	options.base_sources.push_back(CompilerOptions::BaseSource{
		"Base", "res://base.sgd",
		"@test\n"
		"func shared_case():\n"
		"\tpass\n",
		false});

	Compiler compiler;
	compiler.compile(
		"extends \"res://base.sgd\"\n"
		"@test\n"
		"func shared_case():\n"
		"\tpass\n", options);
	assert(compiler.get_error().empty());
	const std::vector<FunctionSignature>& tests = compiler.get_test_signatures();
	assert(tests.size() == 1);
	assert(tests[0].name == "shared_case");
}

static void test_an_inherited_base_test_is_a_test() {
	CompilerOptions options;
	options.output_elf = false;
	options.base_sources.push_back(CompilerOptions::BaseSource{
		"Base", "res://base.sgd",
		"@test\n"
		"func base_case():\n"
		"\tpass\n",
		false});

	Compiler compiler;
	compiler.compile("extends \"res://base.sgd\"\nfunc leaf():\n\tpass\n", options);
	assert(compiler.get_error().empty());
	assert(compiler.get_test_signatures().size() == 1);
	assert(compiler.get_test_signatures()[0].name == "base_case");
}

// -= Shipping builds =-

static const std::string TEST_PROGRAM =
	"func helper() -> int:\n"
	"\treturn 7\n"
	"@test\n"
	"func helper_answers_seven():\n"
	"\tassert(helper() == 7)\n"
	"func other() -> int:\n"
	"\treturn helper() + 1\n";

static void test_a_shipping_build_drops_the_tests() {
	CompilerOptions options;
	options.emit_tests = false;
	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile(TEST_PROGRAM, options);
	assert(!elf.empty());
	assert(compiler.get_test_signatures().empty());
	// The symbol is gone, not merely unlisted: the declaration never reached
	// codegen, so nothing in the ELF names it.
	const std::string image(reinterpret_cast<const char*>(elf.data()), elf.size());
	assert(image.find("helper_answers_seven") == std::string::npos);
	assert(image.find("helper") != std::string::npos);
	assert(std::none_of(compiler.get_function_signatures().begin(),
		compiler.get_function_signatures().end(),
		[](const FunctionSignature& s) { return s.name == "helper_answers_seven"; }));
}

static std::string build_error(const std::string& source, bool emit_tests) {
	CompilerOptions options;
	options.emit_tests = emit_tests;
	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile(source, options);
	return elf.empty() ? compiler.get_error() : std::string();
}

static void test_reaching_a_test_from_a_plain_function_is_refused() {
	const std::string called =
		"@test\nfunc checks_it():\n\tpass\n"
		"func caller():\n\tchecks_it()\n";
	assert(build_error(called, true).find("@test") != std::string::npos);
	assert(build_error(called, false).find("@test") != std::string::npos);

	const std::string as_callable =
		"@test\nfunc checks_it():\n\tpass\n"
		"func caller():\n\tvar c = checks_it\n\tc.call()\n";
	assert(as_callable.find("checks_it") != std::string::npos);
	assert(build_error(as_callable, true).find("@test") != std::string::npos);
	assert(build_error(as_callable, false).find("@test") != std::string::npos);

	const std::string between_tests =
		"@test\nfunc checks_it():\n\tpass\n"
		"@test\nfunc checks_more():\n\tchecks_it()\n";
	assert(build_error(between_tests, true).empty());
	assert(build_error(between_tests, false).empty());

	const std::string helper =
		"@test\nfunc checks_it():\n\thelper()\n"
		"func helper():\n\tpass\n";
	assert(build_error(helper, true).empty());
	assert(build_error(helper, false).empty());
}

static void test_dropping_the_tests_leaves_the_rest_unchanged() {
	// Tests call helpers, never the reverse, so removing them is not supposed
	// to move a single instruction of the code that ships.
	CompilerOptions with_tests;
	CompilerOptions without_tests;
	without_tests.emit_tests = false;

	Compiler a;
	Compiler b;
	const std::vector<uint8_t> shipped = b.compile(TEST_PROGRAM, without_tests);
	a.compile(TEST_PROGRAM, with_tests);
	assert(!shipped.empty());

	// Same function bodies, same signatures, minus the one test.
	std::vector<std::string> kept;
	for (const FunctionSignature& s : a.get_function_signatures()) {
		if (s.name != "helper_answers_seven") kept.push_back(s.name);
	}
	std::vector<std::string> shipped_names;
	for (const FunctionSignature& s : b.get_function_signatures()) {
		shipped_names.push_back(s.name);
	}
	assert(kept == shipped_names);
	assert(a.get_test_signatures().size() == 1);
}

// -= Source model =-

static void test_the_source_model_flags_a_test() {
	const SourceModel model = analyze_source(TEST_PROGRAM, "res://sample.sgd",
		ANALYZE_DECLARATIONS);

	bool found = false;
	for (const SourceDeclaration& declaration : model.declarations) {
		if (declaration.name != "helper_answers_seven") continue;
		found = true;
		assert(declaration.kind == DeclarationKind::FUNCTION);
		assert((declaration.flags & DECLARATION_TEST) != 0);
		assert(std::find(declaration.annotation_arguments.begin(),
			declaration.annotation_arguments.end(), "@test") !=
			declaration.annotation_arguments.end());
	}
	assert(found);
}

int main() {
	std::cout << "=== @test Annotation Tests ===" << std::endl;

	test_a_plain_function_becomes_a_test();
	test_a_static_function_becomes_a_test();
	test_tests_are_published_in_declaration_order();
	test_a_test_carries_its_line_and_doc_comment();
	test_an_annotation_stacks_with_others();

	test_a_variable_is_refused();
	test_a_signal_is_refused();
	test_no_declaration_is_refused();
	test_a_nested_class_method_is_refused();
	test_a_trait_method_is_refused();
	test_a_parameter_is_refused();
	test_a_coroutine_is_refused();
	test_rpc_is_refused();
	test_arguments_are_refused();
	test_a_duplicate_is_refused();

	test_an_overridden_base_test_is_not_a_test();
	test_an_inherited_base_test_is_a_test();

	test_a_shipping_build_drops_the_tests();
	test_reaching_a_test_from_a_plain_function_is_refused();
	test_dropping_the_tests_leaves_the_rest_unchanged();

	test_the_source_model_flags_a_test();

	std::cout << "All @test annotation tests passed!" << std::endl;
	return 0;
}
