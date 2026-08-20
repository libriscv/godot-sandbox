// Function signatures, as the host has to see them.
//
// A call arriving from Godot lands on the exported symbol directly. The Sandbox
// ABI hands the guest one pointer per argument and no count, so a caller that
// leaves an argument out leaves a null pointer in its register and the guest
// faults reading a Variant out of it. The host is therefore the only place that
// can reject the call, and it can only do so with the arity in hand -- which is
// what IRProgram::signatures carries out of the compiler.
//
// Defaults are part of the same problem: the callee cannot fill one in, since
// it cannot tell whether it was given the argument. A default that folds to a
// constant is handed to the host to pass; one that does not leaves the
// parameter required for a host call, which is refused rather than guessed at.
#include "../compiler.h"
#include "../codegen.h"
#include "../function_signature.h"
#include "../lexer.h"
#include "../parser.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace gdscript;

static IRProgram compile_to_ir(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	return codegen.generate(program);
}

static const FunctionSignature& find_signature(const IRProgram& ir, const std::string& name) {
	for (const auto& sig : ir.signatures) {
		if (sig.name == name) {
			return sig;
		}
	}
	throw std::runtime_error("Signature not found: " + name);
}

// -= Tests =-

static void test_one_signature_per_function() {
	const IRProgram ir = compile_to_ir(
		"func a():\n"
		"\treturn 1\n"
		"func b(x):\n"
		"\treturn x\n");

	// Same order as the functions, so the two can be walked together.
	assert(ir.signatures.size() == ir.functions.size());
	for (size_t i = 0; i < ir.signatures.size(); i++) {
		assert(ir.signatures[i].name == ir.functions[i].name);
	}

	std::cout << "  ✓ one signature per function, in order" << std::endl;
}

static void test_arity_of_a_plain_function() {
	const IRProgram ir = compile_to_ir(
		"func other(f: float):\n"
		"\treturn f\n");

	const FunctionSignature& sig = find_signature(ir, "other");
	assert(sig.parameters.size() == 1);
	assert(sig.required_arguments == 1);
	assert(sig.parameters[0].name == "f");
	assert(sig.parameters[0].type == Variant::FLOAT);
	assert(!sig.parameters[0].optional());

	std::cout << "  ✓ a typed parameter is required and carries its type" << std::endl;
}

static void test_untyped_parameter_is_any_variant() {
	const IRProgram ir = compile_to_ir(
		"func f(a, b: int):\n"
		"\treturn b\n");

	const FunctionSignature& sig = find_signature(ir, "f");
	assert(sig.parameters[0].type == FunctionParameter::ANY_TYPE);
	assert(sig.parameters[1].type == Variant::INT);
	assert(sig.required_arguments == 2);

	std::cout << "  ✓ an untyped parameter is any Variant" << std::endl;
}

static void test_constant_defaults_are_carried() {
	const IRProgram ir = compile_to_ir(
		"func f(a, b = 5, c = 1.5, d = \"hi\", e = true, g = null):\n"
		"\treturn a\n");

	const FunctionSignature& sig = find_signature(ir, "f");
	assert(sig.parameters.size() == 6);
	// Only 'a' has to be supplied; the host can produce the rest itself.
	assert(sig.required_arguments == 1);

	assert(!sig.parameters[0].optional());
	assert(sig.parameters[1].optional());
	assert(sig.parameters[1].default_kind == FunctionParameter::DefaultKind::INT);
	assert(std::get<int64_t>(sig.parameters[1].default_value) == 5);
	assert(sig.parameters[2].default_kind == FunctionParameter::DefaultKind::FLOAT);
	assert(std::get<double>(sig.parameters[2].default_value) == 1.5);
	assert(sig.parameters[3].default_kind == FunctionParameter::DefaultKind::STRING);
	assert(std::get<std::string>(sig.parameters[3].default_value) == "hi");
	assert(sig.parameters[4].default_kind == FunctionParameter::DefaultKind::BOOL);
	assert(std::get<bool>(sig.parameters[4].default_value) == true);
	assert(sig.parameters[5].default_kind == FunctionParameter::DefaultKind::NIL);

	std::cout << "  ✓ literal defaults reach the host as constants" << std::endl;
}

static void test_negated_and_const_defaults_fold() {
	// '-1' is a unary minus over a literal, and a global const is a name; both
	// fold, so neither makes the parameter required.
	const IRProgram ir = compile_to_ir(
		"const LIMIT = 42\n"
		"func f(a = -1, b = LIMIT):\n"
		"\treturn a\n");

	const FunctionSignature& sig = find_signature(ir, "f");
	assert(sig.required_arguments == 0);
	assert(std::get<int64_t>(sig.parameters[0].default_value) == -1);
	assert(std::get<int64_t>(sig.parameters[1].default_value) == 42);

	std::cout << "  ✓ a negated literal and a global const fold" << std::endl;
}

static void test_unfoldable_default_stays_required() {
	// The host cannot build a two-element array, and the callee cannot tell it
	// was left out. Requiring it is refused at the boundary rather than
	// silently passing something else.
	const IRProgram ir = compile_to_ir(
		"func f(a = [1, 2]):\n"
		"\treturn a\n");

	const FunctionSignature& sig = find_signature(ir, "f");
	assert(sig.parameters.size() == 1);
	assert(!sig.parameters[0].optional());
	assert(sig.required_arguments == 1);

	// An empty container does fold: the backend writes it directly.
	const IRProgram empty = compile_to_ir(
		"func f(a = [], b = {}):\n"
		"\treturn a\n");
	const FunctionSignature& esig = find_signature(empty, "f");
	assert(esig.required_arguments == 0);
	assert(esig.parameters[0].default_kind == FunctionParameter::DefaultKind::EMPTY_ARRAY);
	assert(esig.parameters[1].default_kind == FunctionParameter::DefaultKind::EMPTY_DICT);

	std::cout << "  ✓ a default that does not fold keeps its parameter required" << std::endl;
}

static void test_struct_parameter_is_a_dictionary() {
	// A struct instance is an ordinary Dictionary, so that is what the host is
	// told to pass and what it gets back.
	const IRProgram ir = compile_to_ir(
		"struct BankAccount:\n"
		"\tvar balance = 0\n"
		"\n"
		"func f(acct: BankAccount) -> BankAccount:\n"
		"\treturn acct\n");

	const FunctionSignature& sig = find_signature(ir, "f");
	assert(sig.parameters[0].type == Variant::DICTIONARY);
	assert(sig.return_type == Variant::DICTIONARY);

	std::cout << "  ✓ a struct parameter is a Dictionary" << std::endl;
}

static void test_return_type() {
	const IRProgram ir = compile_to_ir(
		"func f() -> int:\n"
		"\treturn 1\n"
		"func g():\n"
		"\treturn 1\n");

	assert(find_signature(ir, "f").return_type == Variant::INT);
	assert(find_signature(ir, "g").return_type == FunctionParameter::ANY_TYPE);

	std::cout << "  ✓ a declared return type is reported, and an absent one is any Variant" << std::endl;
}

static void test_compiler_publishes_signatures() {
	// The .sgd script language reads these off the Compiler right after a
	// compile, so they have to survive the whole pipeline.
	CompilerOptions options;
	options.output_elf = true;

	Compiler compiler;
	const auto elf = compiler.compile("func other(f: float):\n\treturn f\n", options);
	assert(!elf.empty());
	assert(compiler.get_function_signatures().size() == 1);
	assert(compiler.get_function_signatures()[0].name == "other");
	assert(compiler.get_function_signatures()[0].required_arguments == 1);

	// A compile that fails before code generation publishes nothing, rather
	// than the previous compile's answer.
	Compiler failing;
	assert(failing.compile("func f(:\n", options).empty());
	assert(failing.get_function_signatures().empty());

	std::cout << "  ✓ Compiler publishes the signatures of its last compile" << std::endl;
}

int main() {
	std::cout << "=== Function Signature Tests ===" << std::endl << std::endl;

	test_one_signature_per_function();
	test_arity_of_a_plain_function();
	test_untyped_parameter_is_any_variant();
	test_constant_defaults_are_carried();
	test_negated_and_const_defaults_fold();
	test_unfoldable_default_stays_required();
	test_struct_parameter_is_a_dictionary();
	test_return_type();
	test_compiler_publishes_signatures();

	std::cout << std::endl << "All function signature tests passed!" << std::endl;
	return 0;
}
