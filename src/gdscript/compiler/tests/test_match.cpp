// Match patterns beyond a plain value.
//
// Dense-integer dispatch is tests/test_switch.cpp's subject. This file covers
// the non-value pattern kinds and the guard that can decline an arm after its
// pattern matched:
//
//   _              matches anything, binds nothing
//   var name       matches anything, binds it for the guard and the body
//   [a, b, ..]     an Array of that shape, elementwise
//   {"k": p, ..}   a Dictionary with those keys
//
// Three properties, one test each: an arm runs only when every part of its
// pattern matched; a binding is a copy, not an alias of the subject; and arms
// are tried in source order whichever lowering carried them.
//
// Container patterns need the host, so those tests assert the emitted shape --
// type test, size, element and key syscalls -- and leave execution to the Godot
// integration tests, where a real Array exists. Everything the IR interpreter
// can execute is executed here.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

// -= Helpers =-

static IRProgram compile_to_ir(const std::string& source, bool optimize = false) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
	}
	return ir;
}

static const IRFunction& find_function(const IRProgram& ir, const std::string& name) {
	for (const auto& func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	throw std::runtime_error("Function not found: " + name);
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

// Syscalls emitted by a container pattern, counted by number. Trailing operands
// identify the Dictionary_Op of a dictionary call.
static int count_syscall(const IRFunction& func, int64_t number, int64_t op = -1) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode != IROpcode::CALL_SYSCALL || instr.operands.size() < 2) {
			continue;
		}
		if (instr.operands[1].immediate() != number) {
			continue;
		}
		if (op >= 0 && (instr.operands.size() < 3 ||
		                instr.operands[2].immediate() != op)) {
			continue;
		}
		count++;
	}
	return count;
}

static int64_t call_int(const IRProgram& ir, const std::string& function,
                        const std::vector<IRInterpreter::Value>& args = {}) {
	IRInterpreter interp(ir);
	return std::get<int64_t>(interp.call(function, args));
}

// Every arm must answer the same before and after the optimizer, and the IR must
// stay verifiable through the pipeline: a pattern's tests jump between arms, and
// a pass that dropped one of those labels would leave a branch into nothing.
static void check_unoptimized_and_optimized(const std::string& source,
                                            const std::string& function,
                                            const std::vector<int64_t>& inputs) {
	IRProgram plain = compile_to_ir(source, /*optimize=*/false);
	for (const auto& func : plain.functions) {
		ir_verify(func, "codegen");
	}
	IRProgram optimized = compile_to_ir(source, /*optimize=*/true);
	for (const auto& func : optimized.functions) {
		ir_verify(func, "the optimizer");
	}
	for (int64_t input : inputs) {
		const int64_t before = call_int(plain, function, {input});
		const int64_t after = call_int(optimized, function, {input});
		assert(before == after && "the optimizer changed what a match answers");
	}
}

// The CompilerException message a source is expected to raise.
static std::string compile_error(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException& e) {
		return e.what();
	}
	return "";
}

// -= Bindings =-

static void test_a_binding_names_the_subject() {
	std::cout << "Testing that `var name` names what it matched..." << std::endl;

	const std::string source = R"(
func classify(n):
	match n:
		0:
			return 100
		var v:
			return v * 2

func test():
	return classify(0) * 1000 + classify(7)
)";

	IRProgram ir = compile_to_ir(source);
	assert(call_int(ir, "classify", {int64_t(0)}) == 100);
	assert(call_int(ir, "classify", {int64_t(7)}) == 14);
	assert(call_int(ir, "test") == 100014);

	// A binding matches everything, so its arm is the fall-through arm: no test
	// is emitted before it.
	check_unoptimized_and_optimized(source, "classify", {0, 7, -3});

	std::cout << "  ✓ A binding matches anything and names it" << std::endl;
}

static void test_a_binding_is_a_copy() {
	std::cout << "Testing that assigning to a binding leaves the subject alone..." << std::endl;

	// Binding and subject must not share a register: writing the name in the
	// body would otherwise change the subject, visible to later arms and to
	// everything after the match.
	const std::string source = R"(
func test():
	var subject = 5
	match subject:
		var v:
			v = 99
	return subject
)";

	assert(call_int(compile_to_ir(source), "test") == 5);
	assert(call_int(compile_to_ir(source, /*optimize=*/true), "test") == 5);

	std::cout << "  ✓ A binding is a copy of the subject" << std::endl;
}

static void test_a_binding_lives_only_in_its_arm() {
	std::cout << "Testing that a binding is scoped to its arm..." << std::endl;

	// Each arm is its own scope, so two arms may bind the same name; a shared
	// scope would make the second a redeclaration error.
	const std::string source = R"(
func pick(n):
	match n:
		1:
			return 1
		var v when v > 10:
			return v
		var v:
			return -v

func test():
	return pick(1) + pick(20) + pick(3)
)";

	IRProgram ir = compile_to_ir(source);
	assert(call_int(ir, "pick", {int64_t(20)}) == 20);
	assert(call_int(ir, "pick", {int64_t(3)}) == -3);
	assert(call_int(ir, "test") == 1 + 20 - 3);

	std::cout << "  ✓ Each arm's bindings are its own" << std::endl;
}

// -= Guards =-

static void test_a_guard_can_decline_an_arm() {
	std::cout << "Testing that a guard is asked after the pattern matched..." << std::endl;

	const std::string source = R"(
func classify(n):
	match n:
		var v when v < 0:
			return 0
		var v when v == 0:
			return 1
		var v when v < 10:
			return 2
		_:
			return 3

func test():
	return classify(-5) * 1000 + classify(0) * 100 + classify(4) * 10 + classify(50)
)";

	IRProgram ir = compile_to_ir(source);
	assert(call_int(ir, "classify", {int64_t(-5)}) == 0);
	assert(call_int(ir, "classify", {int64_t(0)}) == 1);
	assert(call_int(ir, "classify", {int64_t(4)}) == 2);
	assert(call_int(ir, "classify", {int64_t(50)}) == 3);
	assert(call_int(ir, "test") == 123);

	check_unoptimized_and_optimized(source, "classify", {-5, 0, 4, 50});

	std::cout << "  ✓ A declining guard moves on to the next arm" << std::endl;
}

static void test_a_guarded_value_pattern_falls_through() {
	std::cout << "Testing a guard on a value pattern..." << std::endl;

	// The same value twice, told apart by the guard. The second arm is reachable
	// only if a declining guard continues the chain instead of leaving the match.
	const std::string source = R"(
func pick(n, flag):
	match n:
		1 when flag:
			return 10
		1:
			return 20
		_:
			return 30

func test():
	return pick(1, true) + pick(1, false) + pick(2, true)
)";

	IRProgram ir = compile_to_ir(source);
	assert(call_int(ir, "test") == 60);

	std::cout << "  ✓ A guarded value pattern still tries the arms below it" << std::endl;
}

static void test_a_guard_disqualifies_the_jump_table() {
	std::cout << "Testing that a guard keeps the arms out of a jump table..." << std::endl;

	// Six dense integer patterns, the table's target case, but one arm can
	// decline and a table entry cannot test that: the whole match keeps the chain.
	const std::string source = R"(
func dispatch(op : int, flag) -> int:
	match op:
		0:
			return 100
		1:
			return 101
		2 when flag:
			return 102
		3:
			return 103
		4:
			return 104
		_:
			return -1
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& func = find_function(ir, "dispatch");
	assert(count_opcode(func, IROpcode::SWITCH) == 0);
	assert(count_opcode(func, IROpcode::CMP_EQ) == 5);

	// Without the guard the same six arms do become a table, so the case above
	// is about the guard, not the density.
	const std::string ungarded = R"(
func dispatch(op : int) -> int:
	match op:
		0:
			return 100
		1:
			return 101
		2:
			return 102
		3:
			return 103
		4:
			return 104
		_:
			return -1
)";
	const IRProgram ungarded_ir = compile_to_ir(ungarded);
	assert(count_opcode(find_function(ungarded_ir, "dispatch"), IROpcode::SWITCH) == 1);

	std::cout << "  ✓ A guard is not something a table entry can ask" << std::endl;
}

static void test_a_guarded_wildcard_is_not_the_default() {
	std::cout << "Testing that a guarded '_' is not a catch-all..." << std::endl;

	// `_ when c` can decline, so an unmatched subject must not jump straight to
	// it: the arm below stays reachable, and a subject matching neither leaves
	// the match with the variable untouched.
	const std::string source = R"(
func pick(n, flag):
	var out = -1
	match n:
		_ when flag:
			out = 1
		2:
			out = 2
	return out

func test():
	return pick(5, true) * 100 + pick(2, false) * 10 + pick(9, false)
)";

	IRProgram ir = compile_to_ir(source);
	assert(call_int(ir, "test") == 100 + 20 - 1);

	std::cout << "  ✓ A guarded wildcard keeps the arms below it reachable" << std::endl;
}

// -= Array patterns =-

static void test_an_array_pattern_asks_type_length_and_elements() {
	std::cout << "Testing what an array pattern lowers to..." << std::endl;

	const std::string source = R"(
func shape(v):
	match v:
		[]:
			return 0
		[1, var x]:
			return x
		[var a, var b, ..]:
			return a + b
		_:
			return -1
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& func = find_function(ir, "shape");

	// One type test per array pattern: a Dictionary or integer is never asked
	// for a length.
	assert(count_opcode(func, IROpcode::TYPE_TEST) == 3);
	// One length per pattern, and one element fetch per element named.
	assert(count_syscall(func, 523) == 3);
	assert(count_syscall(func, 522) == 4);
	// `..` is the one pattern of the three whose length test is not equality.
	assert(count_opcode(func, IROpcode::CMP_GTE) == 1);

	std::cout << "  ✓ An array pattern is a type test, a length and its elements" << std::endl;
}

static void test_an_array_pattern_reaches_riscv() {
	std::cout << "Testing that container patterns compile to RISC-V..." << std::endl;

	const std::string source = R"(
func shape(v):
	match v:
		[1, var x]:
			return x
		[var a, [var b, var c]]:
			return a + b + c
		{"kind": "circle", "r": var r}:
			return r
		{"kind"}:
			return 1
		{..}:
			return 2
		_:
			return 0
)";

	IRProgram ir = compile_to_ir(source, /*optimize=*/true);
	for (const auto& func : ir.functions) {
		ir_verify(func, "the optimizer");
	}
	RISCVCodeGen codegen;
	const std::vector<uint8_t> code = codegen.generate(ir);
	assert(!code.empty());

	std::cout << "  ✓ Nested array and dictionary patterns reach the backend" << std::endl;
}

static void test_a_container_pattern_a_type_rules_out_costs_nothing() {
	std::cout << "Testing that a pattern that cannot match asks the host nothing..." << std::endl;

	// The subject is an integer, so no length or element is ever needed: the arm
	// is one jump, not destructuring behind a test known to fail.
	const std::string source = R"(
func f(n : int):
	match n:
		[1, 2]:
			return 1
		{"a": 1}:
			return 2
		var v:
			return v
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& func = find_function(ir, "f");
	assert(count_opcode(func, IROpcode::TYPE_TEST) == 0);
	assert(count_syscall(func, 523) == 0);
	assert(count_syscall(func, 522) == 0);
	assert(count_syscall(func, 524) == 0);
	assert(call_int(ir, "f", {int64_t(7)}) == 7);

	std::cout << "  ✓ A pattern the type rules out is one jump" << std::endl;
}

// -= Dictionary patterns =-

static void test_a_dictionary_pattern_asks_size_keys_and_values() {
	std::cout << "Testing what a dictionary pattern lowers to..." << std::endl;

	constexpr int64_t DICT_GET = 0;
	constexpr int64_t DICT_HAS = 3;
	constexpr int64_t DICT_GET_SIZE = 6;

	const std::string source = R"(
func shape(v):
	match v:
		{"kind": "circle", "r": var r}:
			return r
		{"kind"}:
			return 1
		{..}:
			return 2
		_:
			return 0
)";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& func = find_function(ir, "shape");

	assert(count_opcode(func, IROpcode::TYPE_TEST) == 3);
	// A closed pattern constrains the size: `{"kind"}` does not match a
	// Dictionary with a second key, while `{..}` does.
	assert(count_syscall(func, 524, DICT_GET_SIZE) == 2);
	// Every named key is asked for, and only the ones with a pattern are read.
	assert(count_syscall(func, 524, DICT_HAS) == 3);
	assert(count_syscall(func, 524, DICT_GET) == 2);

	std::cout << "  ✓ A dictionary pattern is its keys, and its size when closed" << std::endl;
}

// -= Order, and the rest of the grammar =-

static void test_the_first_matching_arm_wins() {
	std::cout << "Testing that the arms are tried in order..." << std::endl;

	const std::string source = R"(
func pick(n):
	match n:
		1, 2, 3:
			return 10
		3, 4:
			return 20
		var v:
			return v

func test():
	return pick(3) * 1000 + pick(4) * 10 + pick(9)
)";

	IRProgram ir = compile_to_ir(source);
	assert(call_int(ir, "pick", {int64_t(3)}) == 10);
	assert(call_int(ir, "pick", {int64_t(4)}) == 20);
	assert(call_int(ir, "pick", {int64_t(9)}) == 9);
	assert(call_int(ir, "test") == 10 * 1000 + 20 * 10 + 9);

	check_unoptimized_and_optimized(source, "pick", {1, 2, 3, 4, 9});

	std::cout << "  ✓ A value named by two arms takes the first" << std::endl;
}

static void test_an_arm_after_the_wildcard_is_dead_but_legal() {
	std::cout << "Testing that a wildcard is allowed to have arms after it..." << std::endl;

	// GDScript warns rather than rejects; the arms below the wildcard never run.
	const std::string source = R"(
func pick(n):
	match n:
		1:
			return 1
		_:
			return 2
		3:
			return 3

func test():
	return pick(3)
)";

	assert(call_int(compile_to_ir(source), "test") == 2);

	std::cout << "  ✓ An unreachable arm is unreachable, not an error" << std::endl;
}

static void test_break_and_continue_inside_an_arm() {
	std::cout << "Testing that break and continue still mean the loop..." << std::endl;

	// A match is not a loop and has no fallthrough: `break` and `continue` in an
	// arm belong to the enclosing loop.
	const std::string source = R"(
func test():
	var total = 0
	var i = 0
	while i < 10:
		i = i + 1
		match i:
			3:
				continue
			var v when v > 7:
				break
			_:
				total = total + i
	return total
)";

	// 1 + 2 + 4 + 5 + 6 + 7 -- 3 is skipped, and 8 leaves the loop.
	assert(call_int(compile_to_ir(source), "test") == 25);
	assert(call_int(compile_to_ir(source, /*optimize=*/true), "test") == 25);

	std::cout << "  ✓ break and continue reach the loop, not the match" << std::endl;
}

static void test_the_grammar_is_enforced() {
	std::cout << "Testing the errors a malformed pattern earns..." << std::endl;

	// A binding cannot share an arm: the other pattern may be the one that
	// matched, leaving the name unbound.
	const std::string shared_binding = R"(
func f(n):
	match n:
		1, var v:
			return v
)";
	assert(compile_error(shared_binding).find("cannot share an arm") != std::string::npos);

	// `..` means "and the rest", so nothing may follow it.
	const std::string rest_in_the_middle = R"(
func f(n):
	match n:
		[.., 1]:
			return 1
)";
	assert(compile_error(rest_in_the_middle).find("last entry") != std::string::npos);

	const std::string dictionary_rest_in_the_middle = R"(
func f(n):
	match n:
		{.., "a": 1}:
			return 1
)";
	assert(compile_error(dictionary_rest_in_the_middle).find("last entry") != std::string::npos);

	const std::string nameless_binding = R"(
func f(n):
	match n:
		var:
			return 1
)";
	assert(!compile_error(nameless_binding).empty());

	std::cout << "  ✓ The grammar's edges are reported" << std::endl;
}

int main() {
	std::cout << "=== Match pattern tests ===" << std::endl;

	try {
		test_a_binding_names_the_subject();
		test_a_binding_is_a_copy();
		test_a_binding_lives_only_in_its_arm();

		test_a_guard_can_decline_an_arm();
		test_a_guarded_value_pattern_falls_through();
		test_a_guard_disqualifies_the_jump_table();
		test_a_guarded_wildcard_is_not_the_default();

		test_an_array_pattern_asks_type_length_and_elements();
		test_an_array_pattern_reaches_riscv();
		test_a_container_pattern_a_type_rules_out_costs_nothing();
		test_a_dictionary_pattern_asks_size_keys_and_values();

		test_the_first_matching_arm_wins();
		test_an_arm_after_the_wildcard_is_dead_but_legal();
		test_break_and_continue_inside_an_arm();
		test_the_grammar_is_enforced();
	} catch (const CompilerException& e) {
		std::cerr << "Unexpected compiler error: " << e.what() << std::endl;
		return 1;
	}

	std::cout << "=== All match pattern tests passed ===" << std::endl;
	return 0;
}
