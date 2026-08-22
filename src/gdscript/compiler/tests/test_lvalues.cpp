// Inline member stores (VSET_INLINE) and lvalue write-back chains.
// Value types (Vector2/3/4, Color) need copy-back; handles do not.
// Untyped paths branch on the tag at run time.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include "../syscall_numbers.h"
#include <cassert>
#include <iostream>
#include <string>

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

// Count ECALL_DICTIONARY_OPS with GET.
static int count_dict_gets(const IRFunction& func) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 3 &&
			std::get<int64_t>(instr.operands[1].value) == ECALL_DICTIONARY_OPS &&
			std::get<int64_t>(instr.operands[2].value) == 0) {
			count++;
		}
	}
	return count;
}

static int count_vcalls(const IRFunction& func, const std::string& method) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::VCALL && instr.operands.size() >= 3 &&
			std::get<std::string>(instr.operands[2].value) == method) {
			count++;
		}
	}
	return count;
}

static bool refuses(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException&) {
		return true;
	}
	return false;
}

// The register a VSET_INLINE writes into.
static int inline_set_target(const IRFunction& func) {
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::VSET_INLINE) {
			return std::get<int>(instr.operands[0].value);
		}
	}
	return -1;
}

// The type hint a VSET_INLINE was given.
static int64_t inline_set_type(const IRFunction& func) {
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::VSET_INLINE) {
			return std::get<int64_t>(instr.operands[2].value);
		}
	}
	return -1;
}

// Machine code, so that a lowering nothing emits is caught here and not by a
// "not yet implemented" throw in someone's project.
static std::vector<uint8_t> compile_to_riscv(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	IROptimizer optimizer;
	optimizer.optimize(ir);
	RISCVCodeGen backend;
	return backend.generate(ir);
}

// -= A known inline type =-

static void test_known_inline_member_write() {
	std::cout << "Testing that a component of a known inline type is a payload write..." << std::endl;

	const IRProgram vec2 = compile_to_ir(
		"func test():\n\tvar v : Vector2 = Vector2(1, 2)\n\tv.x = 5.0\n\treturn v\n");
	const IRFunction& f = find_function(vec2, "test");
	assert(count_opcode(f, IROpcode::VSET_INLINE) == 1);
	// VSET (ECALL_OBJ_PROP_SET) throws on Vector2.
	assert(count_opcode(f, IROpcode::VSET) == 0);
	assert(inline_set_type(f) == Variant::VECTOR2);

	// Store target is the MAKE_VECTOR2 register, not a dropped copy.
	int target = inline_set_target(f);
	bool target_is_the_variable = false;
	for (const auto& instr : f.instructions) {
		if (instr.opcode == IROpcode::MAKE_VECTOR2 &&
			std::get<int>(instr.operands[0].value) == target) {
			target_is_the_variable = true;
		}
	}
	assert(target_is_the_variable);

	// Integer vectors and Color differ only in payload slot.
	const IRProgram vec3i = compile_to_ir(
		"func test():\n\tvar v : Vector3i = Vector3i(1, 2, 3)\n\tv.z = 9\n\treturn v\n");
	assert(inline_set_type(find_function(vec3i, "test")) == Variant::VECTOR3I);

	const IRProgram color = compile_to_ir(
		"func test():\n\tvar c : Color = Color(1, 0, 0)\n\tc.g = 0.5\n\treturn c\n");
	assert(inline_set_type(find_function(color, "test")) == Variant::COLOR);
	assert(count_opcode(find_function(color, "test"), IROpcode::VSET) == 0);

	// Compound assignment desugars to VGET_INLINE + VSET_INLINE.
	const IRProgram compound = compile_to_ir(
		"func test():\n\tvar v : Vector2 = Vector2(1, 2)\n\tv.y += 1.0\n\treturn v\n");
	assert(count_opcode(find_function(compound, "test"), IROpcode::VSET_INLINE) == 1);
	assert(count_opcode(find_function(compound, "test"), IROpcode::VGET_INLINE) == 1);

	std::cout << "  ✓ a component of a known inline type is a payload write" << std::endl;
}

// -= An unknown type =-

static void test_unknown_member_write_tests_the_tag() {
	std::cout << "Testing that an unknown member write picks its store at run time..." << std::endl;

	const IRProgram chained = compile_to_ir(
		"func test(n):\n\tn.position.x = 5\n");
	const IRFunction& f = find_function(chained, "test");

	// One VSET_INLINE per inline type + three Dictionary arms (read, write, write-back).
	const int arms = count_opcode(f, IROpcode::VSET_INLINE);
	assert(arms > 0);
	assert(count_opcode(f, IROpcode::TYPE_TEST) == arms + 3);
	// One VSET for the Object fallback, one to write `position` back.
	assert(count_opcode(f, IROpcode::VSET) == 2);
	// Chain evaluated once: one VGET for `position`.
	assert(count_opcode(f, IROpcode::VGET) == 1);
	// Dictionary arms: one element read, two element writes.
	assert(count_dict_gets(f) == 1);
	assert(count_opcode(f, IROpcode::DICT_SET) == 2);

	// Read path: VGET_INLINE branches, same issue (VGET throws on Vector2).
	const IRProgram read = compile_to_ir("func test(n):\n\treturn n.position.x\n");
	const IRFunction& r = find_function(read, "test");
	assert(count_opcode(r, IROpcode::VGET_INLINE) > 0);
	assert(count_opcode(r, IROpcode::TYPE_TEST) == count_opcode(r, IROpcode::VGET_INLINE) + 2);
	// VGET fallback for Objects that carry `.x` as a property.
	assert(count_opcode(r, IROpcode::VGET) == 2);
	assert(count_dict_gets(r) == 2);

	// Non-inline member: Dictionary arm + VSET fallback only.
	const IRProgram plain_ir = compile_to_ir("func test(n):\n\tn.visible = true\n");
	const IRFunction& plain = find_function(plain_ir, "test");
	assert(count_opcode(plain, IROpcode::TYPE_TEST) == 1);
	assert(count_opcode(plain, IROpcode::DICT_SET) == 1);
	assert(count_opcode(plain, IROpcode::VSET) == 1);
	assert(count_opcode(plain, IROpcode::VSET_INLINE) == 0);

	std::cout << "  ✓ an unknown member write picks its store at run time" << std::endl;
}

// -= Write-back through each kind of container =-

static void test_the_copy_travels_back() {
	std::cout << "Testing that a mutated copy is written back to its container..." << std::endl;

	// Array element: ARRAY_GET, mutate, ARRAY_SET.
	const IRProgram element = compile_to_ir(
		"func test():\n\tvar a : Array = []\n\tvar i : int = 0\n\ta[i].x = 1.0\n");
	const IRFunction& e = find_function(element, "test");
	assert(count_opcode(e, IROpcode::ARRAY_GET) == 1);
	assert(count_opcode(e, IROpcode::ARRAY_SET) == 1);

	// Dictionary value: DICT_SET for the element store + DICT_SET for the write-back.
	const IRProgram entry = compile_to_ir(
		"func test():\n\tvar d : Dictionary = {}\n\td[\"p\"].x = 1.0\n");
	assert(count_opcode(find_function(entry, "test"), IROpcode::DICT_SET) == 2);

	// Global inline type: write-back via STORE_GLOBAL.
	const IRProgram global = compile_to_ir(
		"var origin : Vector2 = Vector2(0, 0)\nfunc test():\n\torigin.x = 1.0\n");
	const IRFunction& g = find_function(global, "test");
	assert(count_opcode(g, IROpcode::VSET_INLINE) == 1);
	assert(count_opcode(g, IROpcode::STORE_GLOBAL) == 1);

	// Call result: no write-back (Object handle); call evaluated once.
	const IRProgram call_base = compile_to_ir(
		"func node():\n\treturn 1\nfunc test():\n\tnode().position = 5\n");
	assert(count_opcode(find_function(call_base, "test"), IROpcode::CALL) == 1);

	std::cout << "  ✓ a mutated copy is written back to its container" << std::endl;
}

// -= The optimizer =-

static void test_the_optimizer_keeps_the_copy() {
	std::cout << "Testing that the optimizer does not alias a copy onto its source..." << std::endl;

	// Copy propagation must not fold the MOVE into VSET_INLINE's INOUT operand;
	// doing so would alias `copy` back onto `v`.
	const IRProgram aliased = compile_to_ir(
		"func test():\n\tvar v : Vector2 = Vector2(1.0, 1.0)\n"
		"\tvar copy : Vector2 = v\n\tcopy.x = 5.0\n\treturn v\n",
		/*optimize=*/true);
	const IRFunction& f = find_function(aliased, "test");

	int source = -1;
	for (const auto& instr : f.instructions) {
		if (instr.opcode == IROpcode::MAKE_VECTOR2) {
			source = std::get<int>(instr.operands[0].value);
		}
	}
	assert(source >= 0);
	assert(inline_set_target(f) != source);

	// Verifier: INOUT must pass both def-before-read and operand-role checks.
	ir_verify(aliased, "optimized");

	std::cout << "  ✓ the optimizer does not alias a copy onto its source" << std::endl;
}

// -= The backend =-

static void test_the_backend_emits_the_store() {
	std::cout << "Testing that every inline store reaches machine code..." << std::endl;

	// Verify backend emits VSET_INLINE (previously threw "not yet implemented").
	assert(!compile_to_riscv(
		"func test():\n\tvar v : Vector2 = Vector2(1, 2)\n\tv.x = 5.0\n\treturn v\n").empty());
	assert(!compile_to_riscv(
		"func test():\n\tvar v : Vector4i = Vector4i(1, 2, 3, 4)\n\tv.w = 9\n\treturn v\n").empty());
	assert(!compile_to_riscv(
		"func test():\n\tvar c : Color = Color(0, 0, 0)\n\tc.a = 0.25\n\treturn c\n").empty());
	assert(!compile_to_riscv("func test(n):\n\tn.position.x = 5\n").empty());
	// Cross-type stores: int->real and real->int convert, not reinterpret.
	assert(!compile_to_riscv(
		"func test():\n\tvar v : Vector2 = Vector2(0, 0)\n\tv.x = 3\n\treturn v\n").empty());
	assert(!compile_to_riscv(
		"func test():\n\tvar v : Vector2i = Vector2i(0, 0)\n\tv.x = 3.5\n\treturn v\n").empty());

	std::cout << "  ✓ every inline store reaches machine code" << std::endl;
}

// -= Freestanding calls =-

static void test_globals_do_not_become_self_calls() {
	std::cout << "Testing that a global Godot resolves is never a self-call..." << std::endl;

	// Unimplemented globals must be refused (self-call fallback is silently dropped).
	assert(refuses("func test():\n\treturn print_debug(\"x\")\n"));
	assert(refuses("func test(x):\n\treturn weakref(x)\n"));
	assert(refuses("func test():\n\treturn Quaternion(0, 0, 0, 1)\n"));
	assert(refuses("func test():\n\treturn preload(\"res://a.tscn\")\n"));
	// Excluded: mutates shared RNG state.
	assert(refuses("func test():\n\trandomize()\n"));

	// Non-global name: legitimate self-call on the owner node.
	const IRProgram self_call = compile_to_ir("func test():\n\tqueue_free()\n");
	assert(count_vcalls(find_function(self_call, "test"), "queue_free") == 1);

	// Local function shadows the global.
	const IRProgram shadowed = compile_to_ir(
		"func weakref(x):\n\treturn x\nfunc test():\n\treturn weakref(1)\n");
	assert(count_opcode(find_function(shadowed, "test"), IROpcode::CALL) == 1);

	// typeof(): guest-side tag read via TYPE_OF opcode.
	const IRProgram type_of = compile_to_ir("func test(x):\n\treturn typeof(x)\n");
	const IRFunction& t = find_function(type_of, "test");
	assert(count_opcode(t, IROpcode::TYPE_OF) == 1);
	assert(count_vcalls(t, "typeof") == 0);
	assert(refuses("func test(x):\n\treturn typeof(x, 1)\n"));

	std::cout << "  ✓ a global Godot resolves is never a self-call" << std::endl;
}

// -= Containers the Array walk cannot reach =-

static void test_iterating_a_non_array() {
	std::cout << "Testing that a packed array is walked and a String is refused..." << std::endl;

	// Packed array: VCALL size()/get(), not ECALL_ARRAY_SIZE/AT (Array-only).
	const IRProgram packed = compile_to_ir(
		"func test():\n\tvar p = PackedInt32Array([1, 2])\n\tvar t = 0\n"
		"\tfor v in p:\n\t\tt += v\n\treturn t\n");
	const IRFunction& p = find_function(packed, "test");
	assert(count_vcalls(p, "size") == 1);
	assert(count_vcalls(p, "get") == 1);
	// No CALL_SYSCALL: Array-only syscalls would throw.
	assert(count_opcode(p, IROpcode::CALL_SYSCALL) == 0);

	// Array uses syscalls, not VCALL.
	const IRProgram array = compile_to_ir(
		"func test():\n\tvar a : Array = [1, 2]\n\tfor v in a:\n\t\tpass\n");
	assert(count_vcalls(find_function(array, "test"), "size") == 0);

	// String: no size()/get(), refused at compile time.
	assert(refuses("func test():\n\tfor c in \"hello\":\n\t\tpass\n"));
	assert(refuses("func test():\n\tvar s : String = \"hi\"\n\tfor c in s:\n\t\tpass\n"));
	assert(refuses("func test():\n\tvar s : String = \"hi\"\n\treturn s[0]\n"));

	std::cout << "  ✓ a packed array is walked and a String is refused" << std::endl;
}

int main() {
	std::cout << "=== Member Write and Global Call Tests ===" << std::endl << std::endl;

	try {
		test_known_inline_member_write();
		test_unknown_member_write_tests_the_tag();
		test_the_copy_travels_back();
		test_the_optimizer_keeps_the_copy();
		test_the_backend_emits_the_store();
		test_globals_do_not_become_self_calls();
		test_iterating_a_non_array();
	} catch (const CompilerException& e) {
		std::cerr << "Unexpected compiler error: " << e.what() << std::endl;
		return 1;
	}

	std::cout << std::endl << "All lvalue tests passed." << std::endl;
	return 0;
}
