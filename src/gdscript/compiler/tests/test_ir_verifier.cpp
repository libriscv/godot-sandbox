// The IR verifier.
//
// Two halves. The first runs the verifier over everything the compiler actually
// produces -- the whole corpus, after code generation and after every optimizer
// pass -- so a pass that corrupts the IR fails here rather than in whatever
// test happens to exercise the miscompiled path.
//
// The second hands the verifier IR that is broken in each of the ways it exists
// to catch, and requires it to say so. A verifier that accepts everything
// passes the first half and is worth nothing.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "test_corpus.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

namespace {

IRProgram compile_to_ir(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	return codegen.generate(program);
}

// -= What the compiler produces has to verify =-

void test_corpus_verifies() {
	std::cout << "Verifying the corpus, at every point in the pipeline..." << std::endl;

	const auto& passes = IROptimizer::pipeline();
	for (const auto& program : gdscript_test::corpus()) {
		IRProgram ir = compile_to_ir(program.source);
		try {
			ir_verify(ir, "codegen");
		} catch (const CompilerException& e) {
			std::cerr << "FAIL " << program.name << ": " << e.what() << std::endl;
			assert(false && "code generation produced IR the verifier rejects");
		}

		// Each prefix of the pipeline separately: the verifier runs between
		// passes inside optimize_function() too, but doing it here as well keeps
		// the check alive in a build where verification is off by default.
		for (size_t n = 1; n <= passes.size(); n++) {
			IRProgram optimized = compile_to_ir(program.source);
			IROptimizer optimizer;
			optimizer.set_pass_limit(n);
			try {
				optimizer.optimize(optimized);
				ir_verify(optimized, passes[n - 1].name);
			} catch (const CompilerException& e) {
				std::cerr << "FAIL " << program.name << " after passes 1.." << n << " ("
					<< passes[n - 1].name << "): " << e.what() << std::endl;
				assert(false && "an optimizer pass produced IR the verifier rejects");
			}
		}
	}

	std::cout << "  " << gdscript_test::corpus().size() << " programs verified through "
		<< passes.size() << " passes" << std::endl;
}

// -= Broken IR has to be rejected =-

// Hand-built IR has no IRProgram to intern into.
IRStringTable test_strings;

// Runs the verifier and requires it to reject, with `expected` appearing in the
// message so a check cannot pass for the wrong reason.
void expect_rejected(const IRFunction& func, const std::string& expected) {
	try {
		ir_verify(func, "a test", &test_strings);
	} catch (const CompilerException& e) {
		const std::string message = e.what();
		if (message.find(expected) == std::string::npos) {
			std::cerr << "Rejected, but not for the expected reason.\n"
				<< "  expected to contain: " << expected << "\n"
				<< "  actual: " << message << std::endl;
			assert(false && "verifier rejected for the wrong reason");
		}
		return;
	}
	std::cerr << "The verifier accepted IR it should have rejected (" << expected << ")" << std::endl;
	assert(false && "verifier accepted broken IR");
}

// A function that verifies clean, as the starting point for each mutation.
IRFunction good_function() {
	IRFunction func;
	func.name = "test";
	func.max_registers = 3;
	func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(2));
	func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(2), IRValue::imm(3));
	func.instructions.emplace_back(IROpcode::ADD, IRValue::reg(0), IRValue::reg(1), IRValue::reg(2));
	func.instructions.emplace_back(IROpcode::RETURN);
	return func;
}

void test_good_function_verifies() {
	std::cout << "Testing the baseline function..." << std::endl;
	ir_verify(good_function(), "a test");
	std::cout << "  Baseline verifies" << std::endl;
}

void test_arity() {
	std::cout << "Testing arity checks..." << std::endl;

	// Too few operands.
	{
		IRFunction func = good_function();
		func.instructions[2].operands.pop_back();
		expect_rejected(func, "needs at least 3 operands");
	}

	// Too many operands on a non-variadic opcode.
	{
		IRFunction func = good_function();
		func.instructions[2].operands.push_back(IRValue::reg(1));
		expect_rejected(func, "takes 3 operands");
	}

	std::cout << "  Arity OK" << std::endl;
}

void test_operand_kinds() {
	std::cout << "Testing operand kind checks..." << std::endl;

	// An immediate where a source register belongs.
	{
		IRFunction func = good_function();
		func.instructions[2].operands[2] = IRValue::imm(7);
		expect_rejected(func, "expected source register");
	}

	// A register where an immediate belongs.
	{
		IRFunction func = good_function();
		func.instructions[0].operands[1] = IRValue::reg(2);
		expect_rejected(func, "expected immediate");
	}

	// A float immediate where an integer one belongs.
	{
		IRFunction func = good_function();
		func.instructions[0].operands[1] = IRValue::fimm(2.0);
		expect_rejected(func, "expected immediate");
	}

	std::cout << "  Operand kinds OK" << std::endl;
}

void test_undefined_register() {
	std::cout << "Testing definedness..." << std::endl;

	// Reading a register nothing defined. This is the shape a dead-store pass
	// leaves behind when it deletes a definition that was still live.
	{
		IRFunction func = good_function();
		func.instructions.erase(func.instructions.begin());
		expect_rejected(func, "r1 is read but is not defined");
	}

	// Defined on one path only: the definition sits inside an if.
	{
		IRFunction func;
		func.name = "one_path";
		func.max_registers = 3;
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(0));
		func.instructions.emplace_back(IROpcode::BRANCH_ZERO, IRValue::reg(1), IRValue::label(test_strings.intern("skip")));
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(2), IRValue::imm(5));
		func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(test_strings.intern("skip")));
		func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(0), IRValue::reg(2));
		func.instructions.emplace_back(IROpcode::RETURN);
		expect_rejected(func, "r2 is read but is not defined on every path");
	}

	// Defined on both paths: accepted.
	{
		IRFunction func;
		func.name = "both_paths";
		func.max_registers = 3;
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(0));
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(2), IRValue::imm(1));
		func.instructions.emplace_back(IROpcode::BRANCH_ZERO, IRValue::reg(1), IRValue::label(test_strings.intern("skip")));
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(2), IRValue::imm(5));
		func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(test_strings.intern("skip")));
		func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(0), IRValue::reg(2));
		func.instructions.emplace_back(IROpcode::RETURN);
		ir_verify(func, "a test");
	}

	// Parameters count as defined on entry.
	{
		IRFunction func;
		func.name = "with_param";
		func.parameters.push_back("a");
		func.max_registers = 2;
		func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(1), IRValue::reg(0));
		func.instructions.emplace_back(IROpcode::MOVE, IRValue::reg(0), IRValue::reg(1));
		func.instructions.emplace_back(IROpcode::RETURN);
		ir_verify(func, "a test");
	}

	std::cout << "  Definedness OK" << std::endl;
}

void test_labels() {
	std::cout << "Testing label checks..." << std::endl;

	// A branch to a label that does not exist -- what a pass leaves behind when
	// it deletes a label it thought was unreachable.
	{
		IRFunction func = good_function();
		func.instructions.insert(func.instructions.begin() + 2,
			IRInstruction(IROpcode::JUMP, IRValue::label(test_strings.intern("nowhere"))));
		expect_rejected(func, "branch target 'nowhere' has no label");
	}

	// The same label defined twice: a jump to it is ambiguous.
	{
		IRFunction func = good_function();
		func.instructions.insert(func.instructions.begin(),
			IRInstruction(IROpcode::LABEL, IRValue::label(test_strings.intern("twice"))));
		func.instructions.insert(func.instructions.begin() + 2,
			IRInstruction(IROpcode::LABEL, IRValue::label(test_strings.intern("twice"))));
		expect_rejected(func, "defined more than once");
	}

	std::cout << "  Labels OK" << std::endl;
}

void test_max_registers() {
	std::cout << "Testing max_registers..." << std::endl;

	{
		IRFunction func = good_function();
		func.max_registers = 2;
		expect_rejected(func, "outside max_registers");
	}

	// A parameter is defined by the calling convention rather than by an
	// instruction, so a count that covers every operand can still leave the
	// last parameter without a stack slot for the prologue to copy it into.
	{
		IRFunction func = good_function();
		func.parameters = { "a", "b", "c", "d" };
		expect_rejected(func, "parameter registers");
	}

	std::cout << "  max_registers OK" << std::endl;
}

void test_call_shape() {
	std::cout << "Testing call checks..." << std::endl;

	// CALL keeps its destination in operand 1, so a pass that assumes operand 0
	// is the destination rewrites the callee name instead. The verifier catches
	// the result as an operand-kind mismatch.
	{
		IRFunction func;
		func.name = "call_dest";
		func.max_registers = 2;
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(1));
		IRInstruction call(IROpcode::CALL);
		call.operands.push_back(IRValue::reg(0)); // should be the callee name
		call.operands.push_back(IRValue::reg(0));
		call.operands.push_back(IRValue::imm(1));
		call.operands.push_back(IRValue::reg(1));
		func.instructions.push_back(call);
		func.instructions.emplace_back(IROpcode::RETURN);
		expect_rejected(func, "operand 0: expected string");
	}

	// The declared argument count has to match the operands that follow it.
	{
		IRFunction func;
		func.name = "call_count";
		func.max_registers = 2;
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(1));
		IRInstruction call(IROpcode::CALL);
		call.operands.push_back(IRValue::str(test_strings.intern("other")));
		call.operands.push_back(IRValue::reg(0));
		call.operands.push_back(IRValue::imm(2)); // says two, passes one
		call.operands.push_back(IRValue::reg(1));
		func.instructions.push_back(call);
		func.instructions.emplace_back(IROpcode::RETURN);
		expect_rejected(func, "declares 2 arguments");
	}

	// A call must not take its own result register as an argument: the backend
	// writes the result there before the argument would be read.
	{
		IRFunction func;
		func.name = "call_aliases";
		func.max_registers = 2;
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(1));
		IRInstruction call(IROpcode::CALL);
		call.operands.push_back(IRValue::str(test_strings.intern("other")));
		call.operands.push_back(IRValue::reg(1));
		call.operands.push_back(IRValue::imm(1));
		call.operands.push_back(IRValue::reg(1));
		func.instructions.push_back(call);
		func.instructions.emplace_back(IROpcode::RETURN);
		expect_rejected(func, "and also reads it as an argument");
	}

	// A well-formed call verifies.
	{
		IRFunction func;
		func.name = "call_ok";
		func.max_registers = 2;
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(1));
		IRInstruction call(IROpcode::CALL);
		call.operands.push_back(IRValue::str(test_strings.intern("other")));
		call.operands.push_back(IRValue::reg(0));
		call.operands.push_back(IRValue::imm(1));
		call.operands.push_back(IRValue::reg(1));
		func.instructions.push_back(call);
		func.instructions.emplace_back(IROpcode::RETURN);
		ir_verify(func, "a test");
	}

	std::cout << "  Calls OK" << std::endl;
}

void test_dictionary_pair_count() {
	std::cout << "Testing pair counts..." << std::endl;

	IRFunction func;
	func.name = "dict";
	func.max_registers = 3;
	func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(1));
	func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(2), IRValue::imm(2));

	IRInstruction make(IROpcode::MAKE_DICTIONARY);
	make.operands.push_back(IRValue::reg(0));
	make.operands.push_back(IRValue::imm(1));
	make.operands.push_back(IRValue::reg(1));
	make.operands.push_back(IRValue::reg(2));
	func.instructions.push_back(make);
	func.instructions.emplace_back(IROpcode::RETURN);
	ir_verify(func, "a test"); // one pair, two operands: fine

	// Half a pair is not a dictionary.
	func.instructions[2].operands.pop_back();
	expect_rejected(func, "declares 1 pairs");

	std::cout << "  Pair counts OK" << std::endl;
}

void test_type_hints() {
	std::cout << "Testing type hint checks..." << std::endl;

	// A CONVERT that does not say what it converts to.
	{
		IRFunction func;
		func.name = "convert";
		func.max_registers = 2;
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(1));
		func.instructions.emplace_back(IROpcode::CONVERT, IRValue::reg(0), IRValue::reg(1),
			IRValue::imm(Variant::INT));
		func.instructions.emplace_back(IROpcode::RETURN);
		expect_rejected(func, "CONVERT does not say what it converts to");

		func.instructions[1].type_hint = Variant::FLOAT;
		ir_verify(func, "a test");
	}

	// An integer native path claimed over an operand that holds a float. This
	// is the shape register types leaking between functions produced: a hint
	// left over from another function put the backend on the wrong path.
	{
		IRFunction func;
		func.name = "wrong_hint";
		func.max_registers = 3;
		func.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(1), IRValue::fimm(1.5));
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(2), IRValue::imm(2));
		auto& add = func.instructions.emplace_back(IROpcode::ADD, IRValue::reg(0), IRValue::reg(1), IRValue::reg(2));
		add.type_hint = Variant::INT;
		func.instructions.emplace_back(IROpcode::RETURN);
		expect_rejected(func, "is hinted INT but r1 holds FLOAT");
	}

	// The same hint over operands that do hold integers is fine.
	{
		IRFunction func;
		func.name = "right_hint";
		func.max_registers = 3;
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(1));
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(2), IRValue::imm(2));
		auto& add = func.instructions.emplace_back(IROpcode::ADD, IRValue::reg(0), IRValue::reg(1), IRValue::reg(2));
		add.type_hint = Variant::INT;
		func.instructions.emplace_back(IROpcode::RETURN);
		ir_verify(func, "a test");
	}

	// Two paths that disagree about a register's type make it unknown, and an
	// unknown type is not evidence of a wrong hint.
	{
		IRFunction func;
		func.name = "merged_types";
		func.max_registers = 4;
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(3), IRValue::imm(0));
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(1));
		func.instructions.emplace_back(IROpcode::BRANCH_ZERO, IRValue::reg(3), IRValue::label(test_strings.intern("skip")));
		func.instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(1), IRValue::fimm(1.5));
		func.instructions.emplace_back(IROpcode::LABEL, IRValue::label(test_strings.intern("skip")));
		func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(2), IRValue::imm(2));
		auto& add = func.instructions.emplace_back(IROpcode::ADD, IRValue::reg(0), IRValue::reg(1), IRValue::reg(2));
		add.type_hint = Variant::INT;
		func.instructions.emplace_back(IROpcode::RETURN);
		ir_verify(func, "a test");
	}

	std::cout << "  Type hints OK" << std::endl;
}

void test_verification_toggle() {
	std::cout << "Testing the verification switch..." << std::endl;

	const bool previous = ir_verification_enabled();
	set_ir_verification_enabled(false);
	assert(!ir_verification_enabled());
	set_ir_verification_enabled(true);
	assert(ir_verification_enabled());
	set_ir_verification_enabled(previous);

	std::cout << "  Switch OK" << std::endl;
}

} // namespace

int main() {
	std::cout << "=== IR verifier ===" << std::endl;

	// Every unit test verifies unconditionally, whatever the build type.
	set_ir_verification_enabled(true);

	test_good_function_verifies();
	test_arity();
	test_operand_kinds();
	test_undefined_register();
	test_labels();
	test_max_registers();
	test_call_shape();
	test_dictionary_pair_count();
	test_type_hints();
	test_verification_toggle();
	test_corpus_verifies();

	std::cout << "All IR verifier tests passed!" << std::endl;
	return 0;
}
