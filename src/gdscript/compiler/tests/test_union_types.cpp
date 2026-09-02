#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_verifier.h"
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
	IRProgram ir = codegen.generate(program);
	ir_verify(ir, "union test");
	return ir;
}

static std::string rejection(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException& error) {
		return error.what();
	}
	return {};
}

static const IRFunction& function(const IRProgram& ir, const std::string& name) {
	for (const IRFunction& candidate : ir.functions) {
		if (candidate.name == name) return candidate;
	}
	throw std::runtime_error("missing function " + name);
}

static const IRGlobalVar& global(const IRProgram& ir, const std::string& name) {
	for (const IRGlobalVar& candidate : ir.globals) {
		if (candidate.name == name) return candidate;
	}
	throw std::runtime_error("missing global " + name);
}

static int count(const IRFunction& func, IROpcode opcode) {
	int result = 0;
	for (const IRInstruction& instruction : func.instructions) {
		result += instruction.opcode == opcode;
	}
	return result;
}

static int typed_count(const IRFunction& func, IROpcode opcode,
	IRInstruction::TypeHint type) {
	int result = 0;
	for (const IRInstruction& instruction : func.instructions) {
		result += instruction.opcode == opcode && instruction.type_hint == type;
	}
	return result;
}

static void test_parser_shape() {
	Lexer lexer(
		"var value: int | String | null\n"
		"func f(a: int | StringName) -> Dictionary | null:\n"
		"\treturn null\n"
		"func accepts(x):\n"
		"\treturn x is int | String\n");
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	assert(program.globals[0].type_hint.names == std::vector<std::string>({"int", "String"}));
	assert(program.globals[0].type_hint.nullable);
	assert(program.functions[0].parameters[0].type_hint.is_union());
	assert(program.functions[0].return_type.nullable);
	auto* returned = dynamic_cast<ReturnStmt*>(program.functions[1].body[0].get());
	auto* test = dynamic_cast<TypeTestExpr*>(returned->value.get());
	assert(test != nullptr && test->type.is_union());
	assert(test->type.names.size() == 2);
	std::cout << "  ✓ parser carries declaration and is-expression unions\n";
}

static void test_diagnostics() {
	assert(!rejection("func f():\n\tvar x: int |\n").empty());
	assert(rejection("func f():\n\tvar x: Variant | int\n").find("Variant") != std::string::npos);
	assert(rejection("func f():\n\tvar x: int | int\n").find("repeats") != std::string::npos);
	assert(rejection("func f():\n\tvar x: int | null | null\n").find("null") != std::string::npos);
	assert(rejection("func f():\n\tvar x: null\n").find("alone") != std::string::npos);
	assert(!rejection("func f(x):\n\treturn x as int | String\n").empty());
	assert(rejection("@export var x: int | String\n").find("export") != std::string::npos);
	assert(rejection("struct Point:\n\tvar x = 0\nvar p: Point | int\n").find("cannot be used") != std::string::npos);
	assert(rejection("signal changed(value: missing | int)\n").find("Unknown type") != std::string::npos);
	assert(rejection("struct Unused:\n\tvar value: missing | int\n").find("Unknown type") != std::string::npos);
	std::cout << "  ✓ malformed and unsupported unions have diagnostics\n";
}

static void test_assignment_guards() {
	compile_to_ir("func f():\n\tvar x: int | String = 1\n\tx = \"ok\"\n");
	assert(rejection("func f():\n\tvar x: int | String = 1.5\n").find("FLOAT") != std::string::npos);
	assert(rejection("func f():\n\tvar x: int | float = true\n").find("BOOL") != std::string::npos);
	assert(rejection("func f():\n\tvar x: Node | Control = null\n").find("NIL") != std::string::npos);

	const IRProgram guarded = compile_to_ir("func f(value):\n\tvar x: int | String = value\n");
	assert(count(function(guarded, "f"), IROpcode::TYPE_TEST_MASK) == 1);
	assert(count(function(guarded, "f"), IROpcode::THROW) == 1);
	const IRProgram shared_tag = compile_to_ir(
		"func f(value):\n\tvar x: Node | Control = value\n");
	assert(count(function(shared_tag, "f"), IROpcode::TYPE_TEST_MASK) == 1);
	const IRProgram preserved = compile_to_ir("func f():\n\tvar x: int | float = 1\n\treturn x\n");
	assert(count(function(preserved, "f"), IROpcode::CONVERT) == 0);

	const IRProgram parameter = compile_to_ir("func f(value: int | String):\n\treturn 1\n");
	assert(count(function(parameter, "f"), IROpcode::TYPE_TEST_MASK) == 1);
	assert(function(parameter, "f").param_sets[0] ==
		((uint64_t(1) << Variant::INT) | (uint64_t(1) << Variant::STRING)));

	const IRProgram returned = compile_to_ir("func f(value) -> int | String:\n\treturn value\n");
	assert(count(function(returned, "f"), IROpcode::TYPE_TEST_MASK) == 1);
	assert(function(returned, "f").return_set != 0);
	assert(rejection("func f() -> int | String:\n\treturn\n").find("bare return") != std::string::npos);
	compile_to_ir("func f() -> int | null:\n\treturn\n");
	std::cout << "  ✓ assignments, parameters, and returns guard unknown values\n";
}

static void test_defaults_and_reflection() {
	const IRProgram ir = compile_to_ir(
		"var number: int | String\n"
		"var maybe: Node | null\n"
		"var object: Node | int\n"
		"var complex: Dictionary | int\n"
		"func f(value: int | String) -> float | null:\n"
		"\treturn null\n");
	assert(global(ir, "number").declared_set != 0);
	assert(global(ir, "number").type_hint == IRInstruction::TypeHint_NONE);
	assert(global(ir, "number").value_type == IRInstruction::TypeHint_NONE);
	assert(global(ir, "number").init_type == IRGlobalVar::InitType::RUNTIME);
	assert(global(ir, "maybe").init_type == IRGlobalVar::InitType::NULL_VAL);
	assert(global(ir, "object").init_type == IRGlobalVar::InitType::RUNTIME);
	assert(global(ir, "complex").init_type == IRGlobalVar::InitType::RUNTIME);
	assert(typed_count(ir.member_init, IROpcode::MAKE_DICTIONARY, Variant::DICTIONARY) == 1);
	assert(ir.signatures[0].parameters[0].type == FunctionParameter::ANY_TYPE);
	assert(ir.signatures[0].return_type == FunctionParameter::ANY_TYPE);

	const IRProgram local_defaults = compile_to_ir(
		"func f():\n"
		"\tvar number: int | String\n"
		"\tvar maybe: int | null\n"
		"\tvar complex: Dictionary | int\n");
	assert(count(function(local_defaults, "f"), IROpcode::LOAD_IMM) >= 1);
	assert(count(function(local_defaults, "f"), IROpcode::LOAD_NIL) >= 1);
	assert(count(function(local_defaults, "f"), IROpcode::MAKE_DICTIONARY) == 1);

	const IRProgram signal = compile_to_ir("signal changed(value: int | String)\n");
	assert(signal.signals[0].parameters[0].type == FunctionParameter::ANY_TYPE);
	std::cout << "  ✓ defaults preserve the first member and reflection publishes Variant\n";
}

static void test_union_is_and_narrowing() {
	const IRProgram union_is = compile_to_ir("func f(value):\n\treturn value is int | String\n");
	assert(count(function(union_is, "f"), IROpcode::TYPE_TEST_MASK) == 1);

	const IRProgram narrowed = compile_to_ir(
		"func by_is(x: int | String):\n"
		"\tif x is int:\n\t\treturn x + 1\n\telse:\n\t\treturn x + \"!\"\n"
		"func negated(x: int | String):\n"
		"\tif x is not int:\n\t\treturn x + \"!\"\n\treturn x + 1\n"
		"func nullable(x: int | null):\n"
		"\tif x == null:\n\t\treturn 0\n\treturn x + 1\n"
		"func matched(x: int | String):\n"
		"\tmatch typeof(x):\n"
		"\t\tTYPE_INT:\n\t\t\treturn x + 1\n"
		"\t\tTYPE_STRING:\n\t\t\treturn x + \"!\"\n"
		"func reassigned(x: int | String):\n"
		"\tif x is int:\n\t\tx = \"now string\"\n\t\treturn x + \"!\"\n\treturn x\n");
	assert(typed_count(function(narrowed, "by_is"), IROpcode::ADD, Variant::INT) == 1);
	assert(typed_count(function(narrowed, "by_is"), IROpcode::ADD, Variant::STRING) == 1);
	assert(typed_count(function(narrowed, "negated"), IROpcode::ADD, Variant::INT) == 1);
	assert(typed_count(function(narrowed, "negated"), IROpcode::ADD, Variant::STRING) == 1);
	assert(typed_count(function(narrowed, "nullable"), IROpcode::ADD, Variant::INT) == 1);
	assert(typed_count(function(narrowed, "matched"), IROpcode::ADD, Variant::INT) == 1);
	assert(typed_count(function(narrowed, "matched"), IROpcode::ADD, Variant::STRING) == 1);
	assert(typed_count(function(narrowed, "reassigned"), IROpcode::ADD, Variant::STRING) == 1);

	const IRProgram and_narrowed = compile_to_ir(
		"func f(x: int | String):\n"
		"\tif x is int and x + 1 > 0:\n\t\treturn x + 1\n\treturn false\n");
	assert(typed_count(function(and_narrowed, "f"), IROpcode::ADD, Variant::INT) == 2);

	const IRProgram member = compile_to_ir(
		"var value: int | String = 1\n"
		"func touch():\n\treturn 1\n"
		"func safe():\n\tif value is int:\n\t\treturn value + 1\n\treturn 0\n"
		"func assigned():\n\tif value is int:\n\t\tvalue = 2\n\t\treturn value + 1\n\treturn 0\n"
		"func called():\n\tif value is int:\n\t\ttouch()\n\t\treturn value + 1\n\treturn 0\n");
	assert(typed_count(function(member, "safe"), IROpcode::ADD, Variant::INT) == 1);
	assert(typed_count(function(member, "assigned"), IROpcode::ADD, Variant::INT) == 0);
	assert(typed_count(function(member, "called"), IROpcode::ADD, Variant::INT) == 0);

	const IRProgram matched_member = compile_to_ir(
		"var value: int | String = 1\n"
		"func touch():\n\treturn 1\n"
		"func safe():\n\tmatch typeof(value):\n\t\tTYPE_INT:\n\t\t\treturn value + 1\n\treturn 0\n"
		"func called():\n\tmatch typeof(value):\n\t\tTYPE_INT:\n\t\t\ttouch()\n\t\t\treturn value + 1\n\treturn 0\n");
	assert(typed_count(function(matched_member, "safe"), IROpcode::ADD, Variant::INT) == 1);
	assert(typed_count(function(matched_member, "called"), IROpcode::ADD, Variant::INT) == 0);
	std::cout << "  ✓ is, null, match, early-exit, and safe member narrowing work\n";
}

// A union is one Variant slot with a set of allowed tags, so every value whose
// tag the compiler does not already know needs exactly one guard -- and a value
// whose tag it does know needs none.
static void test_union_call_boundaries() {
	const IRProgram ir = compile_to_ir(
		"func takes(v: int | String):\n\treturn v\n"
		"func answers() -> int | String:\n\treturn 1\n"
		"func forwards(value) -> int | String:\n\treturn value\n"
		"func known():\n\treturn takes(1)\n"
		"func unknown(value):\n\treturn takes(value)\n"
		"func consumed():\n\tvar x = answers()\n\treturn x\n");
	// The callee guards its own parameter once, on entry.
	assert(count(function(ir, "takes"), IROpcode::TYPE_TEST_MASK) == 1);
	// The caller does not guard again -- neither for a value it can prove nor
	// for one it cannot, because the entry guard covers both.
	assert(count(function(ir, "known"), IROpcode::TYPE_TEST_MASK) == 0);
	assert(count(function(ir, "unknown"), IROpcode::TYPE_TEST_MASK) == 0);
	// A union-returning call is guarded where it returns, not where it lands --
	// and a return the compiler can already prove is not guarded at all.
	assert(count(function(ir, "answers"), IROpcode::TYPE_TEST_MASK) == 0);
	assert(count(function(ir, "forwards"), IROpcode::TYPE_TEST_MASK) == 1);
	assert(count(function(ir, "consumed"), IROpcode::TYPE_TEST_MASK) == 0);

	// A ternary is two known arms and one unknown result, so the assignment to
	// the declared union carries the single guard.
	const IRProgram ternary = compile_to_ir(
		"func f(c):\n\tvar x: int | String = 1 if c else \"s\"\n\treturn x\n");
	assert(count(function(ternary, "f"), IROpcode::TYPE_TEST_MASK) == 1);
	assert(count(function(ternary, "f"), IROpcode::THROW) == 1);

	// Union to union: the source's own declared set is the proof, so a
	// destination that lists every one of its tags guards nothing again.
	const IRProgram same = compile_to_ir(
		"func f(a: int | String):\n\tvar b: int | String = a\n\treturn b\n");
	assert(count(function(same, "f"), IROpcode::TYPE_TEST_MASK) == 1);
	const IRProgram widened = compile_to_ir(
		"func by_param(a: int | String) -> int | String | null:\n\treturn a\n"
		"func by_local(a: int | String) -> int | String | null:\n"
		"\tvar b: int | String = a\n\treturn b\n"
		"func narrows(a: int | String | null) -> int | String:\n\treturn a\n");
	assert(count(function(widened, "by_param"), IROpcode::TYPE_TEST_MASK) == 1);
	assert(count(function(widened, "by_local"), IROpcode::TYPE_TEST_MASK) == 1);
	// A destination that drops a tag the source allows is not proved by it.
	assert(count(function(widened, "narrows"), IROpcode::TYPE_TEST_MASK) == 2);

	// A slot outlives one value, so an untyped variable does not keep the union
	// its initializer was proved against.
	const IRProgram reassigned = compile_to_ir(
		"func f(a: int | String):\n\tvar b = a\n\tb = 1.5\n"
		"\tvar c: int | String = b\n\treturn c\n");
	assert(count(function(reassigned, "f"), IROpcode::TYPE_TEST_MASK) == 2);
	std::cout << "  ✓ one guard per union boundary, and none where the tag is known\n";
}

// Without an initializer a union slot takes the first member's normal default,
// or null when the union admits it. The members themselves are checked with
// the plain type's rules minus its conversions: a union states which tags the
// slot may hold, so silently storing another one would defeat the point.
static void test_union_defaults_and_conversions() {
	const IRProgram defaults = compile_to_ir(
		"func strings():\n\tvar x: String | int\n\treturn x\n"
		"func arrays():\n\tvar x: Array | int\n\treturn x\n"
		"func dictionaries():\n\tvar x: Dictionary | int\n\treturn x\n"
		"func leading_null():\n\tvar x: null | int\n\treturn x\n"
		"func objects():\n\tvar x: Node | null\n\treturn x\n");
	assert(count(function(defaults, "strings"), IROpcode::LOAD_STRING) == 1);
	assert(count(function(defaults, "arrays"), IROpcode::MAKE_ARRAY) == 1);
	assert(count(function(defaults, "dictionaries"), IROpcode::MAKE_DICTIONARY) == 1);
	assert(count(function(defaults, "leading_null"), IROpcode::LOAD_NIL) == 1);
	assert(count(function(defaults, "objects"), IROpcode::LOAD_NIL) == 1);

	// int -> float widening and Array -> packed construction are conversions a
	// plain declaration performs; a union declaration refuses them, because the
	// value it was handed is not one of the tags it lists.
	assert(rejection("func f():\n\tvar x: float | String = 1\n").find("INT") != std::string::npos);
	assert(rejection("func f():\n\tvar x: PackedInt32Array | int = [1, 2]\n")
		.find("ARRAY") != std::string::npos);
	// The same two assignments through the nullable spelling do convert, since
	// there the plain type is the only non-null member.
	assert(typed_count(function(compile_to_ir("func f():\n\tvar x: float? = 1\n\treturn x\n"), "f"),
		IROpcode::CONVERT, Variant::FLOAT) == 1);
	assert(count(function(
		compile_to_ir("func f():\n\tvar x: PackedInt32Array? = [1, 2]\n\treturn x\n"), "f"),
		IROpcode::MAKE_PACKED_INT32_ARRAY) == 1);
	std::cout << "  ✓ union defaults follow the first member, conversions do not apply\n";
}

int main() {
	std::cout << "=== Union Type Tests ===\n";
	test_parser_shape();
	test_diagnostics();
	test_assignment_guards();
	test_defaults_and_reflection();
	test_union_is_and_narrowing();
	test_union_call_boundaries();
	test_union_defaults_and_conversions();
	std::cout << "All union type tests passed.\n";
	return 0;
}
