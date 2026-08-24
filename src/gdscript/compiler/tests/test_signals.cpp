// Signal declaration publishing and name resolution.
#include "../codegen.h"
#include "../compiler.h"
#include "../compiler_exception.h"
#include "../function_signature.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

// -= Helpers =-

static IRProgram compile_to_ir(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	parser.set_doc_comments(lexer.doc_comments());
	Program program = parser.parse();
	CodeGenerator codegen;
	return codegen.generate(program);
}

static void compile_to_machine_code(const std::string& source) {
	IRProgram ir = compile_to_ir(source);
	IROptimizer optimizer;
	optimizer.optimize(ir);
	ir_verify(ir, "the optimizer");
	RISCVCodeGen backend;
	const std::vector<uint8_t> code = backend.generate(ir);
	assert(!code.empty());
}

static CompilerException compile_failure(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException& e) {
		return e;
	}
	assert(false && "expected this source to be refused");
	throw std::runtime_error("unreachable");
}

static const FunctionSignature& find_signal(const IRProgram& ir, const std::string& name) {
	for (const auto& sig : ir.signals) {
		if (sig.name == name) {
			return sig;
		}
	}
	throw std::runtime_error("Signal not published: " + name);
}

static const IRFunction& find_function(const IRProgram& ir, const std::string& name) {
	for (const auto& func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	throw std::runtime_error("Function not found: " + name);
}

static std::string function_text(const IRProgram& ir, const std::string& name) {
	std::string text;
	for (const auto& instr : find_function(ir, name).instructions) {
		text += instr.to_string();
		text += '\n';
	}
	return text;
}

static int count_opcode(const IRFunction& func, IROpcode opcode) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

// -= What is published =-

static void test_a_signal_is_published_with_its_parameters() {
	std::cout << "Testing what a declaration publishes..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"signal health_changed(new_value: int, cause)\n"
		"func f():\n"
		"\treturn 1\n");

	const FunctionSignature& sig = find_signal(ir, "health_changed");
	assert(sig.parameters.size() == 2);
	assert(sig.parameters[0].name == "new_value");
	assert(sig.parameters[0].type == Variant::INT);
	assert(sig.parameters[1].name == "cause");
	assert(sig.parameters[1].type == FunctionParameter::ANY_TYPE);

	assert(sig.required_arguments == sig.parameters.size());
	assert(!sig.parameters[0].optional());
	assert(sig.line == 1);

	std::cout << "  ✓ a signal publishes its name, parameters and their types" << std::endl;
}

static void test_a_struct_parameter_is_a_dictionary() {
	std::cout << "Testing a struct as a signal parameter..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"struct Hit:\n"
		"\tvar damage = 0\n"
		"signal landed(hit: Hit)\n"
		"func f():\n"
		"\treturn 1\n");

	assert(find_signal(ir, "landed").parameters[0].type == Variant::DICTIONARY);

	std::cout << "  ✓ a struct parameter is published as a Dictionary" << std::endl;
}

static void test_a_signal_generates_no_code() {
	std::cout << "Testing that a signal is not a function..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"signal done\n"
		"func f():\n"
		"\treturn 1\n");

	assert(ir.functions.size() == 1);
	assert(ir.signatures.size() == 1);
	assert(ir.signatures[0].name == "f");
	assert(ir.signals.size() == 1);

	std::cout << "  ✓ a signal adds no function and no function signature" << std::endl;
}

static void test_empty_parameter_list_is_no_parameter_list() {
	std::cout << "Testing `signal x` against `signal x()`..." << std::endl;

	const IRProgram bare = compile_to_ir("signal done\nfunc f():\n\treturn 1\n");
	const IRProgram parens = compile_to_ir("signal done()\nfunc f():\n\treturn 1\n");

	assert(find_signal(bare, "done").parameters.empty());
	assert(find_signal(parens, "done").parameters.empty());

	std::cout << "  ✓ both spellings declare the same signal" << std::endl;
}

static void test_doc_comment_is_published() {
	std::cout << "Testing the doc comment above a signal..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"## Fired when the health changes.\n"
		"## The new value comes with it.\n"
		"signal health_changed(new_value)\n"
		"func f():\n"
		"\treturn 1\n");

	const FunctionSignature& sig = find_signal(ir, "health_changed");
	assert(sig.description == "Fired when the health changes.\nThe new value comes with it.");
	assert(sig.line == 3);

	std::cout << "  ✓ the '##' block above a signal reaches the editor" << std::endl;
}

static void test_wire_format_round_trip() {
	std::cout << "Testing the published blob..." << std::endl;

	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile(
		"## Doc.\n"
		"signal hit(damage: int, source)\n"
		"signal done\n"
		"func f():\n"
		"\treturn 1\n");
	assert(!elf.empty());
	assert(compiler.get_signal_signatures().size() == 2);
	assert(compiler.get_function_signatures().size() == 1);

	const std::vector<uint8_t> blob = encode_function_signatures(compiler.get_signal_signatures());
	std::vector<FunctionSignature> decoded;
	assert(decode_function_signatures(blob.data(), blob.size(), decoded));
	assert(decoded.size() == 2);
	assert(decoded[0].name == "hit");
	assert(decoded[0].description == "Doc.");
	assert(decoded[0].parameters.size() == 2);
	assert(decoded[0].parameters[0].type == Variant::INT);
	assert(decoded[0].parameters[1].type == FunctionParameter::ANY_TYPE);
	assert(decoded[0].required_arguments == 2);
	assert(decoded[1].name == "done");
	assert(decoded[1].parameters.empty());

	std::cout << "  ✓ the signal table survives the wire format intact" << std::endl;
}

// -= What a use of the name lowers to =-

static void test_a_signal_name_is_the_property_read() {
	std::cout << "Testing that `sig` means `self.sig`..." << std::endl;

	const char* uses[] = {
		"\tsig.emit(1)\n",
		"\tsig.connect(c)\n",
		"\treturn sig\n",
		"\treturn sig.is_connected(c)\n",
	};
	for (const char* use : uses) {
		const std::string bare = std::string("signal sig(v)\nfunc test(c):\n") + use;
		std::string qualified = std::string("signal sig(v)\nfunc test(c):\n") + use;
		qualified.replace(qualified.find("sig", qualified.find("func")), 3, "self.sig");

		const std::string bare_text = function_text(compile_to_ir(bare), "test");
		const std::string qualified_text = function_text(compile_to_ir(qualified), "test");
		if (bare_text != qualified_text) {
			std::cerr << "FAIL: " << use << "--- bare ---\n"
					  << bare_text << "--- qualified ---\n"
					  << qualified_text;
			assert(false && "a signal name must lower to the property read");
		}
	}

	const IRProgram ir = compile_to_ir("signal sig(v)\nfunc test():\n\treturn sig\n");
	const IRFunction& test = find_function(ir, "test");
	assert(count_opcode(test, IROpcode::GET_NODE) == 1);
	assert(count_opcode(test, IROpcode::VGET) == 1);

	std::cout << "  ✓ a signal name is one property read on self" << std::endl;
}

// -= What emit/connect lower to =-

static void test_a_signal_method_call_goes_to_the_owner() {
	std::cout << "Testing that `sig.emit(1)` means `self.emit_signal(\"sig\", 1)`..." << std::endl;

	struct { const char* call; const char* owner_method; } cases[] = {
		{ "sig.emit(1)", "emit_signal" },
		{ "sig.connect(c)", "connect" },
		{ "sig.disconnect(c)", "disconnect" },
		{ "sig.is_connected(c)", "is_connected" },
		{ "self.sig.emit(1)", "emit_signal" },
	};
	for (const auto& [call, owner_method] : cases) {
		const IRProgram ir = compile_to_ir(
			std::string("signal sig(v)\nfunc test(c):\n\t") + call + "\n");
		const IRFunction& test = find_function(ir, "test");

		if (count_opcode(test, IROpcode::VGET) != 0) {
			std::cerr << "FAIL: " << call << " still reads the signal as a property\n"
					  << function_text(ir, "test");
			assert(false && "a signal method call must not build the Signal");
		}
		assert(count_opcode(test, IROpcode::VCALL) == 1);

		const std::string text = function_text(ir, "test");
		if (text.find(std::string("\"") + owner_method + "\"") == std::string::npos) {
			std::cerr << "FAIL: " << call << " does not call " << owner_method << "\n" << text;
			assert(false && "expected the owner method");
		}
		assert(count_opcode(test, IROpcode::LOAD_STRING) == 1);
		assert(std::find(ir.string_constants.begin(), ir.string_constants.end(), "sig")
			!= ir.string_constants.end());
	}

	std::cout << "  ✓ emit/connect/disconnect/is_connected go to the owner Object"
			  << std::endl;
}

static void test_emitting_in_a_loop_builds_no_signals() {
	std::cout << "Testing an emit inside a loop..." << std::endl;

	// A host call, so nothing hoists it out: a Signal per pass would abort the guest
	// at Sandbox::MAX_REFS.
	const IRProgram ir = compile_to_ir(
		"signal sig(v)\n"
		"func test():\n"
		"\tfor i in range(200):\n"
		"\t\tsig.emit(i)\n");

	assert(count_opcode(find_function(ir, "test"), IROpcode::VGET) == 0);

	std::cout << "  ✓ an emit in a loop materialises no Signal" << std::endl;
}

static void test_emit_answers_null() {
	std::cout << "Testing what an emit answers..." << std::endl;

	// Signal.emit() is void; Object.emit_signal() answers an error code.
	const IRProgram ir = compile_to_ir(
		"signal sig(v)\nfunc test():\n\treturn sig.emit(1)\n");

	assert(count_opcode(find_function(ir, "test"), IROpcode::LOAD_NIL) == 1);

	std::cout << "  ✓ an emit answers null, not emit_signal's error code" << std::endl;
}

static void test_a_local_signal_method_is_not_rerouted() {
	std::cout << "Testing that a local shadows the signal in a method call..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"signal sig(v)\n"
		"func test(c):\n"
		"\tvar sig = c\n"
		"\treturn sig.is_connected(c)\n");

	const IRFunction& test = find_function(ir, "test");
	assert(count_opcode(test, IROpcode::GET_NODE) == 0);
	assert(count_opcode(test, IROpcode::VGET) == 0);

	std::cout << "  ✓ a local of the same name keeps its own methods" << std::endl;
}

static void test_a_local_shadows_a_signal() {
	std::cout << "Testing that a local shadows a signal..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"signal sig(v)\n"
		"func test():\n"
		"\tvar sig = 5\n"
		"\treturn sig\n");

	const IRFunction& test = find_function(ir, "test");
	assert(count_opcode(test, IROpcode::GET_NODE) == 0);
	assert(count_opcode(test, IROpcode::VGET) == 0);

	std::cout << "  ✓ a local of the same name shadows the signal" << std::endl;
}

static void test_a_signal_is_visible_before_its_declaration() {
	std::cout << "Testing declaration order..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"func test():\n"
		"\tlate.emit()\n"
		"signal late\n");

	assert(count_opcode(find_function(ir, "test"), IROpcode::VCALL) == 1);
	assert(find_signal(ir, "late").parameters.empty());

	std::cout << "  ✓ a signal declared below a function is visible in it" << std::endl;
}

static void test_a_signal_reaches_a_lambda() {
	std::cout << "Testing a signal inside a lambda..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"signal sig(v)\n"
		"func test():\n"
		"\tvar f = func(): sig.emit(1)\n"
		"\treturn f\n");

	const IRFunction& lifted = find_function(ir, "@lambda_0");
	assert(count_opcode(lifted, IROpcode::GET_NODE) == 1);
	assert(count_opcode(lifted, IROpcode::VCALL) == 1);
	assert(lifted.parameters.empty());

	std::cout << "  ✓ a lambda reads the signal from self, not from a capture" << std::endl;
}

static void test_the_whole_pipeline() {
	std::cout << "Testing that a signal program reaches machine code..." << std::endl;

	compile_to_machine_code(
		"signal hit(damage: int)\n"
		"signal done\n"
		"func fire(n):\n"
		"\thit.emit(n)\n"
		"\tdone.emit()\n"
		"func hook(c):\n"
		"\thit.connect(c)\n"
		"\treturn hit.is_connected(c)\n");

	std::cout << "  ✓ emit and connect lower to machine code" << std::endl;
}

// -= What is refused =-

static void test_a_default_on_a_signal_parameter_is_refused() {
	std::cout << "Testing a default on a signal parameter..." << std::endl;

	const CompilerException error = compile_failure("signal sig(v = 1)\n");
	assert(std::string(error.message()).find("default value") != std::string::npos);

	// At the parameter, not at the ')' the check runs after.
	const CompilerException across_lines = compile_failure("signal sig(\n\ta = 1\n)\n");
	assert(across_lines.line() == 2);
	assert(across_lines.column() == 3); // the tab, then the name

	std::cout << "  ✓ a signal parameter cannot have a default, reported at it" << std::endl;
}

static void test_a_name_taken_by_a_signal_is_refused() {
	std::cout << "Testing collisions with a signal name..." << std::endl;

	const char* collisions[] = {
		"signal sig(v)\nvar sig = 1\n",
		"signal sig(v)\nfunc sig():\n\treturn 1\n",
		"signal sig(v)\nenum { sig }\n",
		"signal sig(v)\nstruct sig:\n\tvar a = 1\n",
		"signal sig(v)\nsignal sig(v)\n",
	};
	for (const char* source : collisions) {
		const CompilerException error = compile_failure(source);
		const std::string message = error.message();
		assert(message.find("sig") != std::string::npos);
		assert(message.find("signal") != std::string::npos ||
			   message.find("Signal") != std::string::npos);
	}

	std::cout << "  ✓ a variable, function, enum member or struct cannot take the name" << std::endl;
}

static void test_assigning_to_a_signal_is_refused() {
	std::cout << "Testing assignment to a signal..." << std::endl;

	const CompilerException error = compile_failure(
		"signal sig(v)\n"
		"func test():\n"
		"\tsig = 1\n");
	assert(std::string(error.message()).find("sig") != std::string::npos);

	std::cout << "  ✓ a signal cannot be assigned to" << std::endl;
}

static void test_calling_a_signal_is_refused() {
	std::cout << "Testing a call to a signal..." << std::endl;

	const CompilerException error = compile_failure(
		"signal sig(v)\n"
		"func test():\n"
		"\tsig(1)\n");
	const std::string message = error.message();
	assert(message.find("sig") != std::string::npos);
	assert(std::string(error.hint()).find("emit") != std::string::npos);

	std::cout << "  ✓ a signal is not callable; the message says to emit it" << std::endl;
}

int main() {
	std::cout << "=== Signal Tests ===" << std::endl << std::endl;

	test_a_signal_is_published_with_its_parameters();
	test_a_struct_parameter_is_a_dictionary();
	test_a_signal_generates_no_code();
	test_empty_parameter_list_is_no_parameter_list();
	test_doc_comment_is_published();
	test_wire_format_round_trip();

	test_a_signal_name_is_the_property_read();
	test_a_signal_method_call_goes_to_the_owner();
	test_emitting_in_a_loop_builds_no_signals();
	test_emit_answers_null();
	test_a_local_signal_method_is_not_rerouted();
	test_a_local_shadows_a_signal();
	test_a_signal_is_visible_before_its_declaration();
	test_a_signal_reaches_a_lambda();
	test_the_whole_pipeline();

	test_a_default_on_a_signal_parameter_is_refused();
	test_a_name_taken_by_a_signal_is_refused();
	test_assigning_to_a_signal_is_refused();
	test_calling_a_signal_is_refused();

	std::cout << std::endl << "All signal tests passed!" << std::endl;
	return 0;
}
