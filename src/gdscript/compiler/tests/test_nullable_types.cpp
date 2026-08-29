#include "../codegen.h"
#include "../compiler.h"
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
	ir_verify(ir, "nullable test");
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

static void test_parser_and_spelling() {
	Lexer lexer(
		"var value: Vector2?\n"
		"var array: Array[int]?\n"
		"func f(v: Node? = null) -> Vector3?:\n"
		"\treturn null\n"
		"func accepts(x):\n"
		"\treturn x is Vector2?\n");
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	assert(program.globals[0].type_hint.names == std::vector<std::string>({"Vector2"}));
	assert(program.globals[0].type_hint.nullable);
	assert(program.globals[0].type_hint.spelled_nullable);
	assert(program.globals[0].type_hint.to_string() == "Vector2?");
	assert(program.globals[1].type_hint.to_string() == "Array?");
	assert(program.functions[0].parameters[0].type_hint.to_string() == "Node?");
	assert(program.functions[0].return_type.to_string() == "Vector3?");
	auto* returned = dynamic_cast<ReturnStmt*>(program.functions[1].body[0].get());
	auto* tested = dynamic_cast<TypeTestExpr*>(returned->value.get());
	assert(tested != nullptr && tested->type.to_string() == "Vector2?");

	const IRProgram suffix = compile_to_ir("func f(x: Vector2?):\n\treturn x\n");
	const IRProgram union_form = compile_to_ir("func f(x: Vector2 | null):\n\treturn x\n");
	assert(function(suffix, "f").param_sets == function(union_form, "f").param_sets);
	std::cout << "  ✓ postfix nullable types parse and normalize to the union mask\n";
}

static void test_diagnostics() {
	assert(rejection("func f():\n\tvar x: Variant?\n").find("already includes") != std::string::npos);
	assert(rejection("func f():\n\tvar x: null?\n").find("already includes") != std::string::npos);
	assert(rejection("func f():\n\tvar x: int??\n").find("second") != std::string::npos);
	assert(rejection("func f():\n\tvar x: int | String?\n").find("union member") != std::string::npos);
	assert(rejection("func f():\n\tvar x: int? | String\n").find("come last") != std::string::npos);
	assert(rejection("func f():\n\tvar x: int ?\n").find("directly follow") != std::string::npos);
	assert(rejection("func f(x):\n\treturn x as Node?\n").find("already nullable") != std::string::npos);
	assert(rejection("func f():\n\tvar x: Vector2 = null\n").find("Cannot assign null") != std::string::npos);
	assert(rejection("var x: Vector2 = null\n").find("Cannot assign null") != std::string::npos);
	compile_to_ir("var x: Node = null\nfunc f():\n\tvar y: Node = null\n");
	std::cout << "  ✓ malformed suffixes and null mismatches have targeted diagnostics\n";
}

static void test_coercion_defaults_and_storage() {
	const IRProgram coercions = compile_to_ir(
		"func f(value):\n"
		"\tvar number: float? = 1\n"
		"\tvar packed: PackedInt32Array? = [1, 2]\n"
		"\tvar guarded: Vector2? = value\n"
		"\treturn number\n");
	const IRFunction& f = function(coercions, "f");
	assert(typed_count(f, IROpcode::CONVERT, Variant::FLOAT) == 1);
	assert(count(f, IROpcode::MAKE_PACKED_INT32_ARRAY) == 1);
	assert(count(f, IROpcode::TYPE_TEST_MASK) == 1);
	assert(count(f, IROpcode::THROW) == 1);
	assert(rejection("func f():\n\tvar x: Vector2? = \"bad\"\n").find("Vector2?") != std::string::npos);

	const IRProgram defaults = compile_to_ir(
		"struct Point:\n\tvar x: int = 0\n"
		"var vector: Vector2?\n"
		"var point: Point?\n"
		"func f():\n"
		"\tvar local: Vector3?\n"
		"\tvar shape: Point?\n"
		"\tshape = Point(7)\n"
		"\treturn shape.x\n");
	assert(global(defaults, "vector").init_type == IRGlobalVar::InitType::NULL_VAL);
	assert(global(defaults, "vector").value_type == IRInstruction::TypeHint_NONE);
	assert(global(defaults, "vector").declared_set != 0);
	assert(global(defaults, "point").init_type == IRGlobalVar::InitType::NULL_VAL);
	assert(count(function(defaults, "f"), IROpcode::LOAD_NIL) >= 2);
	assert(count(function(defaults, "f"), IROpcode::VGET_INLINE) == 0);
	std::cout << "  ✓ nullable coercion, NIL defaults, structs, and untyped storage work\n";
}

static void test_narrowing() {
	const IRProgram ir = compile_to_ir(
		"func checked(x: Vector2?):\n"
		"\tif x == null:\n\t\treturn 0.0\n"
		"\treturn x.x\n"
		"func truthy(x: Vector2?):\n"
		"\tif x:\n\t\treturn x.x\n"
		"\treturn x.x\n"
		"func asserted(x: Vector2?):\n"
		"\tassert(x != null)\n"
		"\treturn x.x\n"
		"func conjunction(x: Vector2?):\n"
		"\treturn x != null and x.x > 0.0\n"
		"func by_is(x: int?):\n"
		"\tif x is int:\n\t\treturn x + 1\n"
		"\treturn 0\n"
		"func matched(x: Vector2?):\n"
		"\tmatch x:\n"
		"\t\tnull:\n\t\t\treturn 0.0\n"
		"\t\t_:\n\t\t\treturn x.x\n"
		"func after_break(x: Vector2?):\n"
		"\twhile true:\n"
		"\t\tif x == null:\n\t\t\t\tbreak\n"
		"\t\treturn x.x\n"
		"\treturn 0.0\n"
		"func reassigned(x: Vector2?):\n"
		"\tx = Vector2(2, 3)\n"
		"\treturn x.x\n");
	assert(count(function(ir, "checked"), IROpcode::VGET_INLINE) == 1);
	// The then arm has one direct Vector2 read; the rest goes through the
	// untyped multi-type dispatch because a falsy Vector2.ZERO is not NIL.
	assert(count(function(ir, "truthy"), IROpcode::VGET_INLINE) > 1);
	assert(count(function(ir, "asserted"), IROpcode::VGET_INLINE) == 1);
	assert(count(function(ir, "conjunction"), IROpcode::VGET_INLINE) == 1);
	assert(typed_count(function(ir, "by_is"), IROpcode::ADD, Variant::INT) == 1);
	assert(count(function(ir, "matched"), IROpcode::VGET_INLINE) == 1);
	assert(count(function(ir, "after_break"), IROpcode::VGET_INLINE) == 1);
	assert(count(function(ir, "reassigned"), IROpcode::VGET_INLINE) == 1);

	const IRProgram nullable_is = compile_to_ir("func f(x):\n\treturn x is Vector2?\n");
	assert(count(function(nullable_is, "f"), IROpcode::TYPE_TEST_MASK) == 1);
	std::cout << "  ✓ null, truthiness, assert, conjunction, and is checks narrow soundly\n";
}

static void test_reflection() {
	const IRProgram ir = compile_to_ir(
		"@export var value: Vector2?\n"
		"@export var texture: Texture2D?\n"
		"func f(v: Vector2?, n: Node?) -> Vector3?:\n"
		"\treturn null\n");
	assert(ir.signatures[0].parameters[0].type == FunctionParameter::ANY_TYPE);
	assert(ir.signatures[0].parameters[1].type == Variant::OBJECT);
	assert(ir.signatures[0].return_type == FunctionParameter::ANY_TYPE);

	Compiler compiler;
	CompilerOptions options;
	options.output_elf = false;
	compiler.compile(
		"@export var value: Vector2?\n"
		"@export var texture: Texture2D?\n", options);
	assert(!compiler.get_error_info().has_error);
	const auto& properties = compiler.get_property_signatures();
	assert(properties.size() == 2);
	assert(properties[0].type == -1);
	assert(properties[0].default_kind == PropertyDefaultKind::NIL);
	assert(properties[1].type == Variant::OBJECT);
	assert(properties[1].class_name == "Texture2D");
	std::cout << "  ✓ nullable value and object types publish compatible reflection\n";
}

// The mask a `T?` parameter is guarded with, or 0 when it is not guarded.
static uint64_t guard_mask(const IRFunction& func) {
	for (const IRInstruction& instruction : func.instructions) {
		if (instruction.opcode == IROpcode::TYPE_TEST_MASK) {
			return uint64_t(instruction.operands.at(2).immediate());
		}
	}
	return 0;
}

// A nullable slot is one Variant that holds either T or NIL, so the boundary
// guard is a two-tag mask -- and once a null check has ruled NIL out, the value
// lowers exactly like the plain type it was declared with.
static void test_nullable_containers_and_structs() {
	const IRProgram ir = compile_to_ir(
		"struct Point:\n\tvar x = 0\n"
		"func arrays(a: Array[int]?):\n"
		"\tif a == null:\n\t\treturn 0\n"
		"\tvar total = 0\n\tfor v in a:\n\t\ttotal += v\n\treturn total\n"
		"func dictionaries(d: Dictionary?):\n"
		"\tif d == null:\n\t\treturn 0\n\treturn d.size()\n"
		"func structs(p: Point?):\n"
		"\tif p == null:\n\t\treturn 0\n\treturn p.x\n");

	const uint64_t nil = uint64_t(1) << Variant::NIL;
	assert(guard_mask(function(ir, "arrays")) == (nil | (uint64_t(1) << Variant::ARRAY)));
	assert(guard_mask(function(ir, "dictionaries")) == (nil | (uint64_t(1) << Variant::DICTIONARY)));
	// A struct is a Dictionary, so that is the tag its nullable form admits.
	assert(guard_mask(function(ir, "structs")) == (nil | (uint64_t(1) << Variant::DICTIONARY)));

	// After the null check each one is the plain lowering: a counted Array walk,
	// a Dictionary operation, a direct field read.
	assert(count(function(ir, "arrays"), IROpcode::CALL_SYSCALL) >= 2);
	assert(count(function(ir, "dictionaries"), IROpcode::CALL_SYSCALL) == 1);
	assert(count(function(ir, "structs"), IROpcode::DICT_GET_CONST) == 1);
	// NOTE: the tag mask is the whole boundary check for a nullable struct --
	// the exact-shape guard a plain `p: Point` parameter carries is not emitted
	// here, so any Dictionary satisfies `Point?`.
	assert(count(function(ir, "structs"), IROpcode::STRUCT_CHECK) == 0);

	// A nullable struct without an initializer is NIL rather than a fresh
	// instance: the slot's value is the declaration, not the struct.
	const IRProgram declared = compile_to_ir(
		"struct Point:\n\tvar x = 0\n"
		"func f():\n\tvar plain: Point\n\tvar maybe: Point?\n\treturn maybe\n");
	assert(count(function(declared, "f"), IROpcode::MAKE_DICTIONARY_KEYED) == 1);
	assert(count(function(declared, "f"), IROpcode::LOAD_NIL) == 1);
	std::cout << "  ✓ nullable containers and structs guard one mask and lower plainly\n";
}

// Narrowing is a fact about a value, so it travels with the value: a local
// copied out of a narrowed one is narrowed too. A member is different -- it can
// change under any call -- which is why its storage stays untyped.
static void test_nullable_narrowing_travels() {
	const IRProgram ir = compile_to_ir(
		"func copied(x: int?):\n"
		"\tif x == null:\n\t\treturn 0\n"
		"\tvar y = x\n\treturn y + 1\n"
		"func through_assert(x: Vector2?):\n"
		"\tassert(x is Vector2)\n"
		"\tvar y = x\n\treturn y.x\n"
		"func early_return(x: int?):\n"
		"\tif x == null:\n\t\treturn 0\n"
		"\treturn x + 1\n");
	assert(typed_count(function(ir, "copied"), IROpcode::ADD, Variant::INT) == 1);
	assert(count(function(ir, "through_assert"), IROpcode::VGET_INLINE) == 1);
	assert(typed_count(function(ir, "early_return"), IROpcode::ADD, Variant::INT) == 1);

	// A nullable member keeps untyped storage even where it demonstrably holds
	// T: a concrete slot type plus a stored NIL is what corrupts host reads.
	const IRProgram member = compile_to_ir(
		"var maybe: int?\n"
		"var plain: int = 0\n"
		"func f():\n\tmaybe = 5\n\treturn maybe + 1\n"
		"func g():\n\tplain = 5\n\treturn plain + 1\n");
	assert(global(member, "maybe").value_type == IRInstruction::TypeHint_NONE);
	assert(global(member, "maybe").init_type == IRGlobalVar::InitType::NULL_VAL);
	assert(typed_count(function(member, "f"), IROpcode::ADD, Variant::INT) == 0);
	// The same member without the '?' keeps its declared type through the store.
	assert(global(member, "plain").value_type == Variant::INT);
	assert(typed_count(function(member, "g"), IROpcode::ADD, Variant::INT) == 1);
	std::cout << "  ✓ narrowing travels with a value, and a nullable member stays untyped\n";
}

int main() {
	std::cout << "=== Nullable Type Tests ===\n";
	test_parser_and_spelling();
	test_diagnostics();
	test_coercion_defaults_and_storage();
	test_narrowing();
	test_reflection();
	test_nullable_containers_and_structs();
	test_nullable_narrowing_travels();
	std::cout << "All nullable type tests passed.\n";
	return 0;
}
