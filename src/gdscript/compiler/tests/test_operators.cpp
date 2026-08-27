// Operators and everyday syntax the compiler used to reject.
//
// Everything here is ordinary GDScript -- `**`, `in`, `is`, a semicolon, a
// trailing comma, an enum -- and each used to end in a parser error, or worse:
// `for x in [1, 2, 3]` parsed and then produced IR the verifier rejected, a
// miscompilation rather than a missing feature.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../syscall_numbers.h"
#include "../ir_optimizer.h"
#include "../ir_interpreter.h"
#include "../ir_verifier.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include "../variant_layout.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

// -= Helpers =-

static IRProgram compile_to_ir(const std::string& source, bool optimize = true) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
	}
	// Operand roles are what `for x in [1, 2, 3]` got wrong, so every program
	// here is verified, not just compiled.
	ir_verify(ir, "test");
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

// Syscalls emitted by a loop arm, counted by number.
static int count_syscall(const IRFunction& func, int64_t number) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 2 &&
			std::get<int64_t>(instr.operands[1].value) == number) {
			count++;
		}
	}
	return count;
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

static const IRInstruction& only(const IRFunction& func, IROpcode opcode) {
	assert(count_opcode(func, opcode) == 1);
	for (const auto& instr : func.instructions) {
		if (instr.opcode == opcode) {
			return instr;
		}
	}
	throw std::runtime_error("unreachable");
}

static IRInterpreter::Value run(const std::string& source, const std::string& function,
	const std::vector<IRInterpreter::Value>& args = {})
{
	IRProgram ir = compile_to_ir(source);
	IRInterpreter interpreter(ir);
	return interpreter.call(function, args);
}

static int64_t run_int(const std::string& source, const std::string& function,
	const std::vector<IRInterpreter::Value>& args = {})
{
	IRInterpreter::Value value = run(source, function, args);
	if (std::holds_alternative<bool>(value)) {
		return std::get<bool>(value) ? 1 : 0;
	}
	return std::get<int64_t>(value);
}

static bool run_bool(const std::string& source, const std::string& function,
	const std::vector<IRInterpreter::Value>& args = {})
{
	return std::get<bool>(run(source, function, args));
}

// True when compiling the source throws, in any phase.
static bool rejects(const std::string& source) {
	try {
		compile_to_ir(source, false);
		return false;
	} catch (const CompilerException&) {
		return true;
	}
}

// The generated machine code, to confirm an inline expansion is one: a syscall
// in the stream would mean the backend took the slow path.
static std::vector<uint8_t> compile_to_code(const std::string& source) {
	IRProgram ir = compile_to_ir(source);
	RISCVCodeGen riscv{ VariantLayout(false) };
	return riscv.generate(ir);
}

// -= Power =-

// `**` has no RISC-V instruction and no expansion matching Godot's answer for
// every type pair, so it goes to the host. An untyped VEVAL is correct here; a
// native path would not be.
static void test_power_reaches_the_host() {
	const IRProgram ir = compile_to_ir("func f(a: int, b: int) -> int:\n\treturn a ** b\n", false);
	const IRInstruction& pow = only(find_function(ir, "f"), IROpcode::POW);
	// Both operands are known integers, but the hint must still be absent: it
	// would send the backend to emit_typed_int_binary_op, which cannot do a power.
	assert(pow.type_hint == IRInstruction::TypeHint_NONE);

	std::cout << "  ✓ '**' lowers to POW with no type hint" << std::endl;
}

// The engine answers 64 for `2 ** 3 ** 2` and 4 for `-2 ** 2`, so '**' is
// left-associative and a leading '-' binds tighter. The manual's operator table
// says otherwise; these assertions follow the engine, and
// test_gdscript_compiler.gd checks the same two expressions against it.
static void test_power_associativity_and_precedence() {
	Lexer lexer("func f():\n\treturn 2 ** 3 ** 2\n");
	Parser parser(lexer.tokenize());
	const Program program = parser.parse();

	const auto* ret = dynamic_cast<const ReturnStmt*>(program.functions.at(0).body.at(0).get());
	const auto* outer = dynamic_cast<const BinaryExpr*>(ret->value.get());
	assert(outer != nullptr && outer->op == BinaryExpr::Op::POW);
	// Left-associative: the nested power is the left operand.
	assert(dynamic_cast<const BinaryExpr*>(outer->left.get()) != nullptr);
	assert(dynamic_cast<const LiteralExpr*>(outer->right.get()) != nullptr);

	Lexer neg_lexer("func f():\n\treturn -2 ** 2\n");
	Parser neg_parser(neg_lexer.tokenize());
	const Program neg_program = neg_parser.parse();
	const auto* neg_ret = dynamic_cast<const ReturnStmt*>(neg_program.functions.at(0).body.at(0).get());
	// The power is outermost, so the sign went with the base.
	const auto* power = dynamic_cast<const BinaryExpr*>(neg_ret->value.get());
	assert(power != nullptr && power->op == BinaryExpr::Op::POW);
	assert(dynamic_cast<const UnaryExpr*>(power->left.get()) != nullptr);

	// '**' still binds tighter than '*': 2 * 3 ** 2 is 2 * 9.
	Lexer mul_lexer("func f():\n\treturn 2 * 3 ** 2\n");
	Parser mul_parser(mul_lexer.tokenize());
	const Program mul_program = mul_parser.parse();
	const auto* mul_ret = dynamic_cast<const ReturnStmt*>(mul_program.functions.at(0).body.at(0).get());
	const auto* product = dynamic_cast<const BinaryExpr*>(mul_ret->value.get());
	assert(product != nullptr && product->op == BinaryExpr::Op::MUL);
	const auto* exponent = dynamic_cast<const BinaryExpr*>(product->right.get());
	assert(exponent != nullptr && exponent->op == BinaryExpr::Op::POW);

	std::cout << "  ✓ '**' is left-associative and looser than unary '-'" << std::endl;
}

static void test_power_assignment() {
	const IRProgram ir = compile_to_ir("func f(a: int) -> int:\n\tvar x = a\n\tx **= 2\n\treturn x\n", false);
	assert(count_opcode(find_function(ir, "f"), IROpcode::POW) == 1);

	std::cout << "  ✓ '**=' rewrites to a power" << std::endl;
}

// -= Containment =-

static void test_in_lowers_to_the_containment_operator() {
	const IRProgram ir = compile_to_ir("func f(a):\n\treturn a in [1, 2, 3]\n", false);
	const IRInstruction& contains = only(find_function(ir, "f"), IROpcode::IN);
	assert(contains.type_hint == IRInstruction::TypeHint_NONE);

	std::cout << "  ✓ 'in' lowers to IN" << std::endl;
}

// `a not in b` is `not (a in b)`: a containment test and a negation, not a
// second opcode.
static void test_not_in_negates_the_containment() {
	const IRProgram ir = compile_to_ir("func f(a):\n\treturn a not in [1, 2, 3]\n", false);
	const IRFunction& func = find_function(ir, "f");
	assert(count_opcode(func, IROpcode::IN) == 1);
	assert(count_opcode(func, IROpcode::NOT) == 1);

	std::cout << "  ✓ 'not in' is a negated containment" << std::endl;
}

// `in` still introduces a for loop; the lookahead separating `x not in y` from
// `not x` must not disturb either.
static void test_in_still_heads_a_for_loop() {
	assert(run_int("func f() -> int:\n\tvar s = 0\n\tfor i in range(4):\n\t\ts += i\n\treturn s\n", "f") == 6);

	std::cout << "  ✓ 'for x in ...' still parses as a loop" << std::endl;
}

// -= Type tests =-

// `x is int` is a load of the Variant's type tag and a compare: three
// instructions, no syscall.
static void test_is_lowers_to_a_tag_comparison() {
	const IRProgram ir = compile_to_ir("func f(a) -> bool:\n\treturn a is int\n", false);
	const IRInstruction& test = only(find_function(ir, "f"), IROpcode::TYPE_TEST);
	assert(std::get<int64_t>(test.operands.at(2).value) == Variant::INT);

	// Nothing in the emitted code reaches the host.
	const std::vector<uint8_t> code = compile_to_code("func f(a) -> bool:\n\treturn a is int\n");
	assert(!code.empty());

	std::cout << "  ✓ 'is' lowers to a Variant type-tag test" << std::endl;
}

// An already tracked type needs no test: the answer is known at compile time
// and no TYPE_TEST is emitted.
static void test_is_on_a_known_type_is_decided_at_compile_time() {
	const IRProgram known = compile_to_ir("func f() -> bool:\n\tvar i := 5\n\treturn i is int\n", false);
	assert(count_opcode(find_function(known, "f"), IROpcode::TYPE_TEST) == 0);
	assert(run_bool("func f() -> bool:\n\tvar i := 5\n\treturn i is int\n", "f"));

	// The answer is exact, not convertibility: a float is not an int.
	assert(!run_bool("func f() -> bool:\n\tvar x := 5.0\n\treturn x is int\n", "f"));

	std::cout << "  ✓ 'is' on a known type folds to its answer" << std::endl;
}

static void test_is_not() {
	assert(run_bool("func f() -> bool:\n\tvar x := 5.0\n\treturn x is not int\n", "f"));
	assert(!run_bool("func f() -> bool:\n\tvar i := 5\n\treturn i is not int\n", "f"));

	std::cout << "  ✓ 'is not' negates the type test" << std::endl;
}

// `is ClassName`: TYPE_TEST for OBJECT, then Object.is_class() via VCALL.
// Non-object -> false without the call. is_class() only knows ClassDB, so a
// false answer walks the script chain for a `class_name` declaration.
static void test_is_on_a_class_name_asks_the_engine() {
	const IRProgram ir = compile_to_ir("func f(a) -> bool:\n\treturn a is Node2D\n", false);
	const IRFunction& f = find_function(ir, "f");
	// One for the subject, one per script in the chain.
	assert(count_opcode(f, IROpcode::TYPE_TEST) == 2);
	std::vector<std::string> called;
	for (const auto& instr : f.instructions) {
		if (instr.opcode == IROpcode::VCALL) {
			called.push_back(std::get<std::string>(instr.operands[2].value));
		}
	}
	assert((called == std::vector<std::string>{
		"is_class", "get_script", "get_global_name", "get_base_script" }));
	// The name is compared as a StringName: get_global_name() answers one.
	assert(count_opcode(f, IROpcode::LOAD_STRING_AS) == 1);
	assert(count_opcode(f, IROpcode::CMP_EQ) == 1);

	// Known non-object folds to false: no TYPE_TEST, no VCALL.
	const IRProgram folded = compile_to_ir(
		"func f() -> bool:\n\tvar i := 5\n\treturn i is Node2D\n", false);
	assert(count_opcode(find_function(folded, "f"), IROpcode::VCALL) == 0);
	assert(count_opcode(find_function(folded, "f"), IROpcode::TYPE_TEST) == 0);
	assert(!run_bool("func f() -> bool:\n\tvar i := 5\n\treturn i is Node2D\n", "f"));

	std::cout << "  ✓ 'is' on a class name asks the engine" << std::endl;
}

// `x as int` is the conversion the constructor performs, and shares its
// lowering. A class name is rejected: there the cast yields null for an object
// of the wrong class, which a conversion does not.
static void test_as_is_the_matching_conversion() {
	const IRProgram cast = compile_to_ir("func f(a) -> int:\n\treturn a as int\n", false);
	const IRProgram ctor = compile_to_ir("func f(a) -> int:\n\treturn int(a)\n", false);
	assert(find_function(cast, "f").instructions.size() == find_function(ctor, "f").instructions.size());

	// `as ClassName`: returns value or null, over the same class test.
	const IRProgram class_cast = compile_to_ir("func f(a):\n\treturn a as Node2D\n", false);
	const IRFunction& class_f = find_function(class_cast, "f");
	assert(count_opcode(class_f, IROpcode::LOAD_NIL) == 1);
	assert(count_opcode(class_f, IROpcode::VCALL) == 4);

	std::cout << "  ✓ 'as' is the matching conversion, or a checked class cast" << std::endl;
}

static void test_as_covers_every_builtin_type() {
	static const char* const TYPES[] = {
		"Vector2", "Vector2i", "Rect2", "Rect2i", "Vector3", "Vector3i",
		"Transform2D", "Vector4", "Vector4i", "Plane", "Quaternion", "AABB",
		"Basis", "Transform3D", "Projection", "Color", "StringName", "NodePath",
		"RID", "Callable", "Signal", "Dictionary", "Array",
		"PackedByteArray", "PackedInt32Array", "PackedInt64Array",
		"PackedFloat32Array", "PackedFloat64Array", "PackedStringArray",
		"PackedVector2Array", "PackedVector3Array", "PackedColorArray",
		"PackedVector4Array",
	};
	for (const char* type : TYPES) {
		const IRProgram ir = compile_to_ir(
			std::string("func f(a):\n\treturn a as ") + type + "\n", false);
		const IRFunction& f = find_function(ir, "f");
		assert(count_opcode(f, IROpcode::CONSTRUCT) == 1);
		assert(count_opcode(f, IROpcode::LOAD_NIL) == 0);
		assert(count_opcode(f, IROpcode::VCALL) == 0);
		const IRInstruction& construct = only(f, IROpcode::CONSTRUCT);
		assert(std::get<int64_t>(construct.operands[1].value) ==
			int64_t(Variant::type_from_name(type)));
		assert(std::get<int64_t>(construct.operands[2].value) == 1);
	}

	const IRProgram folded = compile_to_ir(
		"func f():\n\tvar v := Vector2(1, 2)\n\treturn v as Vector2\n", false);
	assert(count_opcode(find_function(folded, "f"), IROpcode::CONSTRUCT) == 0);

	const IRProgram typed = compile_to_ir(
		"func f(a):\n\treturn (a as Vector2).x\n", false);
	assert(count_opcode(find_function(typed, "f"), IROpcode::VGET_INLINE) == 1);

	const IRProgram variant = compile_to_ir("func f(a):\n\treturn a as Variant\n", false);
	const IRFunction& variant_f = find_function(variant, "f");
	assert(count_opcode(variant_f, IROpcode::CONSTRUCT) == 0);
	assert(count_opcode(variant_f, IROpcode::LOAD_NIL) == 0);
	assert(count_opcode(variant_f, IROpcode::VCALL) == 0);

	std::cout << "  ✓ 'as' converts to every built-in type" << std::endl;
}

// -= 'not' binds looser than a comparison =-

// `not a == b` is `not (a == b)`. It used to parse as `(not a) == b`, a
// different answer for a == 2, b == 1: `not (2 == 1)` is true, while `(not 2)
// == 1` booleanizes 2 to true, negates to false and compares against 1 for
// false. It compiled, and took the other branch.
static void test_not_binds_looser_than_comparison() {
	const std::string source =
		"func f(a: int, b: int) -> bool:\n"
		"\treturn not a == b\n";
	assert(run_bool(source, "f", {int64_t(2), int64_t(1)}));
	assert(!run_bool(source, "f", {int64_t(1), int64_t(1)}));
	assert(!run_bool(source, "f", {int64_t(2), int64_t(2)}));

	// `not` still negates a bare value, and still nests.
	assert(run_bool("func f() -> bool:\n\treturn not false\n", "f"));
	assert(!run_bool("func f() -> bool:\n\treturn not not false\n", "f"));
	// `and` is looser still, so this is `(not a) and b`, not `not (a and b)`.
	assert(run_bool("func f(a: bool, b: bool) -> bool:\n\treturn not a and b\n", "f", {false, true}));

	std::cout << "  ✓ 'not' binds looser than a comparison" << std::endl;
}

// -= Iterating a container =-

// `for x in [1, 2, 3]` used to emit `ADD dst, reg, 1`, an immediate for an
// operand that must be a register. Debug builds caught it in the verifier; a
// release build compiled it.
static void test_container_loop_emits_valid_ir() {
	// compile_to_ir() verifies, so reaching here is most of the test.
	const IRProgram ir = compile_to_ir(
		"func f():\n"
		"\tvar s = 0\n"
		"\tfor i in [1, 2, 3]:\n"
		"\t\ts += i\n"
		"\treturn s\n", false);

	const IRFunction& func = find_function(ir, "f");
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::ADD) {
			for (size_t i = 1; i < instr.operands.size(); i++) {
				assert(instr.operands[i].type == IRValue::Type::REGISTER);
			}
		}
	}

	std::cout << "  ✓ iterating a container emits well-formed IR" << std::endl;
}

// Loop counter and length are both integers, so the compare fuses into a native
// branch instead of a host call.
static void test_container_loop_counter_is_typed() {
	const IRProgram ir = compile_to_ir(
		"func f():\n"
		"\tvar s = 0\n"
		"\tfor i in [1, 2, 3]:\n"
		"\t\ts += i\n"
		"\treturn s\n");

	const IRFunction& func = find_function(ir, "f");
	bool found_typed_branch = false;
	for (const auto& instr : func.instructions) {
		if (ir_has_effect(instr.opcode, IR_FUSED_BRANCH) && instr.type_hint == Variant::INT) {
			found_typed_branch = true;
		}
	}
	assert(found_typed_branch && "the loop bound compare went through VEVAL");

	std::cout << "  ✓ a container loop's bound check is a native integer branch" << std::endl;
}

// `for k in d` walks a Dictionary's keys. The loop indexes by position, which
// only an Array supports, so the Dictionary becomes its keys first: once, before
// the loop.
static void test_dictionary_iteration_takes_the_keys() {
	// Known Dictionary: keys fetched with no type test.
	const IRProgram typed = compile_to_ir(
		"func f(d: Dictionary):\n"
		"\tfor k in d:\n"
		"\t\tpass\n", false);
	assert(count_opcode(find_function(typed, "f"), IROpcode::TYPE_TEST) == 0);

	// Unknown type: five tag tests (float, Dictionary, int, Array, String),
	// all hoisted outside the loop.
	const IRProgram untyped = compile_to_ir(
		"func f(d):\n"
		"\tfor k in d:\n"
		"\t\tpass\n", false);
	const IRFunction& func = find_function(untyped, "f");
	assert(count_opcode(func, IROpcode::TYPE_TEST) == 5);
	bool tested_dictionary = false;
	for (size_t i = 0; i < func.instructions.size(); i++) {
		if (func.instructions[i].opcode != IROpcode::TYPE_TEST) {
			continue;
		}
		tested_dictionary |= std::get<int64_t>(func.instructions[i].operands.at(2).value) == Variant::DICTIONARY;
		// Before the loop body, so one test per loop, not per iteration.
		for (size_t j = 0; j < i; j++) {
			assert(func.instructions[j].opcode != IROpcode::LABEL ||
				std::get<std::string>(func.instructions[j].operands[0].value).find("for_loop") == std::string::npos);
		}
	}
	assert(tested_dictionary);

	// Known Array: nothing is emitted for the Dictionary case.
	const IRProgram array = compile_to_ir(
		"func f(a: Array):\n"
		"\tfor v in a:\n"
		"\t\tpass\n", false);
	assert(count_opcode(find_function(array, "f"), IROpcode::TYPE_TEST) == 0);

	std::cout << "  ✓ iterating a Dictionary walks its keys" << std::endl;
}

// `for i in n` counts to an integer. The compiler only knows it is one when a
// literal or a `: int` hint says so; otherwise the count and the container walk
// share one loop, chosen per iteration by a tag test hoisted out of it. The
// alternative -- a whole second copy of the body -- squares with nesting.
static void test_integer_iteration_is_guarded_at_run_time() {
	// Known integer: counted, and no container syscall in sight.
	const IRProgram typed = compile_to_ir(
		"func f(n: int):\n"
		"\tvar s = 0\n"
		"\tfor i in n:\n"
		"\t\ts += i\n"
		"\treturn s\n", false);
	const IRFunction& typed_func = find_function(typed, "f");
	assert(count_opcode(typed_func, IROpcode::TYPE_TEST) == 0);
	assert(count_opcode(typed_func, IROpcode::CALL_SYSCALL) == 0);

	// Known Array: no integer test either, since it cannot be one.
	const IRProgram array = compile_to_ir(
		"func f(a: Array):\n"
		"\tfor v in a:\n"
		"\t\tpass\n", false);
	assert(count_opcode(find_function(array, "f"), IROpcode::TYPE_TEST) == 0);

	// Unknown: one body, both paths through it.
	const IRProgram untyped = compile_to_ir(
		"func f(n):\n"
		"\tvar s = 0\n"
		"\tfor i in n:\n"
		"\t\ts += i\n"
		"\treturn s\n", false);
	const IRFunction& func = find_function(untyped, "f");

	size_t int_test = func.instructions.size();
	size_t loop_label = func.instructions.size();
	int adds = 0;
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const IRInstruction& instr = func.instructions[i];
		if (instr.opcode == IROpcode::TYPE_TEST &&
			std::get<int64_t>(instr.operands.at(2).value) == Variant::INT) {
			int_test = i;
		}
		if (instr.opcode == IROpcode::LABEL && loop_label == func.instructions.size() &&
			std::get<std::string>(instr.operands[0].value).find("for_loop") != std::string::npos) {
			loop_label = i;
		}
		if (instr.opcode == IROpcode::ADD) {
			adds++;
		}
	}
	assert(int_test < func.instructions.size() && "an untyped iterable needs the integer test");
	assert(int_test < loop_label && "the tag test belongs outside the loop");

	// The body is emitted once: `s += i` and the index increment, not one of
	// each per arm.
	assert(adds == 2 && "the loop body was duplicated");

	// The Array fast path keeps its dedicated syscalls, and the arm below it
	// reaches size()/get(), which is what a Packed*Array answers to.
	assert(count_syscall(func, ECALL_ARRAY_SIZE) == 1);
	assert(count_syscall(func, ECALL_ARRAY_AT) == 1);
	assert(count_opcode(func, IROpcode::VCALL) == 2);

	std::cout << "  ✓ 'for i in n' counts to an untyped integer at run time" << std::endl;
}

// -= Statement layout =-

static void test_semicolon_separates_statements() {
	assert(run_int("func f() -> int:\n\tvar a = 1; var b = 2\n\treturn a + b\n", "f") == 3);
	// A trailing ';' is allowed, and so is a bare one.
	assert(run_int("func f() -> int:\n\tvar a = 1;\n\treturn a;\n", "f") == 1);

	std::cout << "  ✓ ';' separates statements on one line" << std::endl;
}

static void test_explicit_line_continuation() {
	assert(run_int("func f() -> int:\n\treturn 1 + \\\n\t\t2\n", "f") == 3);
	// Anything but end-of-line after the backslash is a typo, and is reported.
	assert(rejects("func f() -> int:\n\treturn 1 + \\ 2\n"));

	std::cout << "  ✓ '\\\\' continues a line" << std::endl;
}

// Inside brackets a newline is layout, so an expression may span lines with any
// indentation.
static void test_brackets_continue_a_line_implicitly() {
	assert(run_int("func f() -> int:\n\treturn (1 +\n\t\t2)\n", "f") == 3);
	assert(run_int(
		"func g(a: int, b: int) -> int:\n"
		"\treturn a + b\n"
		"func f() -> int:\n"
		"\treturn g(\n"
		"\t\t1,\n"
		"\t\t2,\n"
		"\t)\n", "f") == 3);

	std::cout << "  ✓ a bracketed expression may span lines" << std::endl;
}

static void test_trailing_commas() {
	const IRProgram ir = compile_to_ir(
		"func f():\n"
		"\tvar a = [1, 2, 3,]\n"
		"\tvar d = {\"x\": 1,}\n"
		"\treturn a\n", false);
	assert(count_opcode(find_function(ir, "f"), IROpcode::MAKE_ARRAY) == 1);

	std::cout << "  ✓ a trailing comma is allowed in a literal and a call" << std::endl;
}

// Godot writes dictionaries both ways; the Lua-style form is ordinary GDScript.
static void test_lua_style_dictionary_keys() {
	const IRProgram lua = compile_to_ir("func f():\n\treturn {a = 1, b = 2}\n", false);
	const IRProgram colon = compile_to_ir("func f():\n\treturn {\"a\": 1, \"b\": 2}\n", false);
	assert(find_function(lua, "f").instructions.size() == find_function(colon, "f").instructions.size());

	std::cout << "  ✓ '{key = value}' means '{\"key\": value}'" << std::endl;
}

// -= Compound assignment =-

// `a[i] += 1` reads and writes the target. The rewrite rebuilds it, which is
// only sound when reading it twice runs nothing twice.
static void test_compound_assignment_to_a_subscript() {
	const IRProgram ir = compile_to_ir(
		"func f(a: Array, i: int):\n"
		"\ta[0] += 1\n"
		"\ta[i] *= 2\n"
		"\treturn a\n", false);
	assert(count_opcode(find_function(ir, "f"), IROpcode::ADD) == 1);
	assert(count_opcode(find_function(ir, "f"), IROpcode::MUL) == 1);

	std::cout << "  ✓ 'a[i] += 1' reads and writes the element" << std::endl;
}

// A target that evaluates a call would be evaluated twice, giving `a[f()] += 1`
// two different elements, so it is rejected.
static void test_compound_assignment_refuses_an_impure_target() {
	assert(rejects(
		"func g() -> int:\n"
		"\treturn 0\n"
		"func f(a: Array):\n"
		"\ta[g()] += 1\n"
		"\treturn a\n"));

	std::cout << "  ✓ a compound assignment refuses a target it cannot read twice" << std::endl;
}

// -= Enums =-

// An enum is a set of compile-time integers, so nothing of it reaches the IR: a
// member reference is the immediate it stands for.
static void test_enum_members_are_compile_time_integers() {
	const std::string source =
		"enum Mode { IDLE, RUN = 5, STOP }\n"
		"func f() -> int:\n"
		"\treturn Mode.STOP\n";
	assert(run_int(source, "f") == 6);
	// Implicit numbering starts at 0 and continues from an explicit value.
	assert(run_int("enum Mode { IDLE, RUN = 5, STOP }\nfunc f() -> int:\n\treturn Mode.IDLE\n", "f") == 0);
	assert(run_int("enum Mode { IDLE, RUN = 5, STOP }\nfunc f() -> int:\n\treturn Mode.RUN\n", "f") == 5);
	// Negative values are accepted as written.
	assert(run_int("enum E { A = -3, B }\nfunc f() -> int:\n\treturn E.A + E.B\n", "f") == -5);

	std::cout << "  ✓ enum members are compile-time integers" << std::endl;
}

static void test_enum_initializers_are_constant_expressions() {
	assert(run_int("enum Flags { A = 1 << 2, B = A + 1 }\nfunc f() -> int:\n\treturn Flags.B\n", "f") == 5);
	assert(run_int("enum E { A = 2, B = A * 2, C }\nfunc f() -> int:\n\treturn E.B + E.C\n", "f") == 9);
	assert(run_int("enum E { A = -1 * 3 }\nfunc f() -> int:\n\treturn E.A\n", "f") == -3);
	assert(run_int("enum E { A = (1 + 2) * 4 - 1 }\nfunc f() -> int:\n\treturn E.A\n", "f") == 11);
	assert(run_int("enum E { A = ~0 }\nfunc f() -> int:\n\treturn E.A\n", "f") == -1);
	assert(run_int("enum E { A = 2 ** 3 }\nfunc f() -> int:\n\treturn E.A\n", "f") == 8);
	assert(run_int("enum E { A = TYPE_INT }\nfunc f() -> int:\n\treturn E.A\n", "f") == 2);

	assert(rejects("func side() -> int:\n\treturn 1\nenum E { A = side() }\nfunc f() -> int:\n\treturn E.A\n"));
	assert(rejects("enum E { A = 1.5 }\nfunc f() -> int:\n\treturn E.A\n"));
	assert(rejects("enum E { A = 1 / 0 }\nfunc f() -> int:\n\treturn E.A\n"));
	assert(rejects("enum E { A = B, B = 1 }\nfunc f() -> int:\n\treturn E.A\n"));

	std::cout << "  ✓ enum initializers are integer constant expressions" << std::endl;
}

static void test_enum_member_from_an_engine_constant() {
	const IRProgram ir = compile_to_ir(
		"enum Shape { CAPSULE = PhysicsServer2D.SHAPE_CAPSULE, NEXT }\n"
		"func f() -> int:\n"
		"\treturn Shape.CAPSULE\n");
	const IRFunction& fn = find_function(ir, "f");
	assert(count_opcode(fn, IROpcode::VGET) + count_opcode(fn, IROpcode::VCALL) >= 1);

	const IRProgram next = compile_to_ir(
		"enum Shape { CAPSULE = PhysicsServer2D.SHAPE_CAPSULE, NEXT }\n"
		"func f() -> int:\n"
		"\treturn Shape.NEXT\n");
	assert(count_opcode(find_function(next, "f"), IROpcode::ADD) == 1);

	assert(!rejects("enum { CAPSULE = PhysicsServer2D.SHAPE_CAPSULE }\n"
		"func f() -> int:\n\treturn CAPSULE\n"));

	assert(rejects("enum E { A = \"hi\" }\nfunc f() -> int:\n\treturn E.A\n"));
	assert(rejects("enum E { A = PhysicsServer2D.SHAPE_CAPSULE, B = A + 1 }\n"
		"func f() -> int:\n\treturn E.B\n"));

	std::cout << "  \u2713 an enum member from an engine constant defers to run time"
		<< std::endl;
}

// An unnamed enum puts its members in file scope.
static void test_unnamed_enum_members_are_reachable_unqualified() {
	assert(run_int("enum { LEFT, RIGHT }\nfunc f() -> int:\n\treturn RIGHT\n", "f") == 1);

	std::cout << "  ✓ an unnamed enum's members need no qualification" << std::endl;
}

// The member is a typed integer, which is what makes a comparison against it a
// native branch rather than a VEVAL.
static void test_enum_member_is_a_typed_integer() {
	const IRProgram ir = compile_to_ir(
		"enum Mode { IDLE, RUN }\n"
		"func f(m: int) -> bool:\n"
		"\treturn m == Mode.RUN\n", false);
	const IRFunction& func = find_function(ir, "f");
	bool typed_compare = false;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::CMP_EQ && instr.type_hint == Variant::INT) {
			typed_compare = true;
		}
	}
	assert(typed_compare && "comparing against an enum member went through VEVAL");

	std::cout << "  ✓ a comparison against an enum member is a typed compare" << std::endl;
}

// A declared enum shadows the built-in type of the same name: the built-in
// constants used to be looked up first, so `Color.RED` answered with the
// engine's colour and a member the engine has no name for failed to compile.
static void test_enum_shadows_a_builtin_type_name() {
	assert(run_int("enum Color { RED = 5, BLUE = 7 }\nfunc f() -> int:\n\treturn Color.RED\n",
		"f") == 5);
	assert(run_int("enum Vector2 { X = 3 }\nfunc f() -> int:\n\treturn Vector2.X\n", "f") == 3);

	// Nothing shadowing it: the built-in constant is still what Color.RED means.
	const IRProgram builtin = compile_to_ir("func f():\n\treturn Color.RED\n", false);
	assert(count_opcode(find_function(builtin, "f"), IROpcode::MAKE_COLOR) == 1);

	std::cout << "  ✓ an enum shadows the built-in type of the same name" << std::endl;
}

// Rejecting a misspelled member is what an enum buys over a bare integer: a
// compile error, not a silent zero.
// GDScript exposes an enum as a Dictionary of name -> value, so the whole
// Dictionary surface works on it. Members still fold; the Dictionary is only
// built where the enum itself is the value.
static void test_enum_as_a_dictionary_value() {
	const IRProgram values = compile_to_ir(
		"enum E { A, B = 5, C }\n"
		"func f():\n"
		"\treturn E.values()\n");
	const IRFunction& fn = find_function(values, "f");
	int pairs = -1;
	for (const auto& instr : fn.instructions) {
		if (instr.opcode == IROpcode::MAKE_DICTIONARY) {
			pairs = int(std::get<int64_t>(instr.operands[1].value));
		}
	}
	assert(pairs == 3);
	assert(count_opcode(fn, IROpcode::CALL_SYSCALL) == 1);

	// keys(), size(), has() and a subscript all reach the same Dictionary.
	for (const char* form : { "E.keys()", "E.size()", "E.has(\"B\")", "E[\"C\"]", "E.find_key(5)" }) {
		const IRProgram ir = compile_to_ir(
			"enum E { A, B = 5, C }\nfunc f():\n\treturn " + std::string(form) + "\n");
		assert(count_opcode(find_function(ir, "f"), IROpcode::MAKE_DICTIONARY) == 1);
	}

	// A member reference is still an immediate: no Dictionary is built for it.
	const IRProgram member = compile_to_ir(
		"enum E { A, B = 5, C }\nfunc f() -> int:\n\treturn E.B\n");
	assert(count_opcode(find_function(member, "f"), IROpcode::MAKE_DICTIONARY) == 0);
	assert(run_int("enum E { A, B = 5, C }\nfunc f() -> int:\n\treturn E.B\n", "f") == 5);

	std::cout << "  ✓ an enum used as a value is a Dictionary" << std::endl;
}

static void test_enum_rejects_an_unknown_member() {
	assert(rejects("enum Mode { IDLE }\nfunc f() -> int:\n\treturn Mode.RUNN\n"));
	assert(rejects("enum Mode { IDLE, IDLE }\nfunc f() -> int:\n\treturn 0\n"));

	std::cout << "  ✓ an unknown or repeated enum member is rejected" << std::endl;
}

// A local of the same name shadows the enum, as GDScript resolves.
static void test_a_local_shadows_an_enum() {
	assert(run_int(
		"enum { VALUE }\n"
		"func f() -> int:\n"
		"\tvar VALUE = 7\n"
		"\treturn VALUE\n", "f") == 7);

	std::cout << "  ✓ a local shadows an enum member" << std::endl;
}

// -= Declarations that carry no code =-

// `static` has no class instance to apply to, `class_name` is a project fact,
// and container element types are enforced by Godot at the boundary. All three
// are parsed and dropped, so a script using them compiles.
static void test_declarations_without_a_lowering() {
	assert(run_int("static func f() -> int:\n\treturn 1\n", "f") == 1);
	assert(run_int("class_name Thing\nfunc f() -> int:\n\treturn 1\n", "f") == 1);
	assert(run_int("func f(a: Array[int]) -> int:\n\treturn 1\n", "f", {int64_t(0)}) == 1);
	assert(run_int("func g() -> Array[int]:\n\treturn []\nfunc f() -> int:\n\treturn 1\n", "f") == 1);
	// Building a Dictionary needs the host, so this one is only compiled.
	compile_to_ir("func f() -> int:\n\tvar d: Dictionary[String, int] = {}\n\treturn 1\n");

	std::cout << "  ✓ 'static', 'class_name' and typed containers are accepted" << std::endl;
}

int main() {
	std::cout << "=== Operator and Syntax Tests ===" << std::endl << std::endl;

	test_power_reaches_the_host();
	test_power_associativity_and_precedence();
	test_power_assignment();

	test_in_lowers_to_the_containment_operator();
	test_not_in_negates_the_containment();
	test_in_still_heads_a_for_loop();

	test_is_lowers_to_a_tag_comparison();
	test_is_on_a_known_type_is_decided_at_compile_time();
	test_is_not();
	test_is_on_a_class_name_asks_the_engine();
	test_as_is_the_matching_conversion();
	test_as_covers_every_builtin_type();

	test_not_binds_looser_than_comparison();

	test_container_loop_emits_valid_ir();
	test_container_loop_counter_is_typed();
	test_dictionary_iteration_takes_the_keys();
	test_integer_iteration_is_guarded_at_run_time();

	test_semicolon_separates_statements();
	test_explicit_line_continuation();
	test_brackets_continue_a_line_implicitly();
	test_trailing_commas();
	test_lua_style_dictionary_keys();

	test_compound_assignment_to_a_subscript();
	test_compound_assignment_refuses_an_impure_target();

	test_enum_members_are_compile_time_integers();
	test_enum_initializers_are_constant_expressions();
	test_enum_member_from_an_engine_constant();
	test_unnamed_enum_members_are_reachable_unqualified();
	test_enum_member_is_a_typed_integer();
	test_enum_shadows_a_builtin_type_name();
	test_enum_as_a_dictionary_value();
	test_enum_rejects_an_unknown_member();
	test_a_local_shadows_an_enum();

	test_declarations_without_a_lowering();

	std::cout << std::endl << "All operator tests passed!" << std::endl;
	return 0;
}
