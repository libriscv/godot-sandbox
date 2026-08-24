// Element access on an Array or a Dictionary.
//
// `a[i]`, `a[i] = v` and `a.append(v)` used to be Variant::call("get"), ("set")
// and ("append"): a StringName built from the method name and a builtin-method
// lookup, per element touched, which is where a fetch-decode-execute loop over
// an Array spends nearly all of its time. On a known Array with a known int
// index all three lower to one system call instead.
//
// What "known" means is the subject of half of this file: the backend reads the
// container's scoped index and the element index straight out of the Variants,
// with no type check, so anything less certain has to keep the VCALL.
//
// A negative index -- `a[-1]` is the last element -- is normalised in the guest,
// since ECALL_ARRAY_AT reads a negative index as a write. That part is machine
// code rather than IR, so it is covered in tests/tests/test_gdscript_compiler.gd
// against the engine's own answer for the same subscript.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
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

// ECALL_DICTIONARY_OPS calls, by Dictionary_Op.
static int count_dict_ops(const IRFunction& func, int64_t op) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() >= 3 &&
			std::get<int64_t>(instr.operands[1].value) == 524 &&
			std::get<int64_t>(instr.operands[2].value) == op) {
			count++;
		}
	}
	return count;
}

// The whole pipeline, so an opcode the backend cannot expand fails here rather
// than in a Godot project.
static void compile_to_machine_code(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	IROptimizer optimizer;
	optimizer.optimize(ir);
	ir_verify(ir, "the optimizer");
	RISCVCodeGen backend;
	const std::vector<uint8_t> code = backend.generate(ir);
	assert(!code.empty());
}

// -= Tests =-

static void test_array_element_access() {
	std::cout << "Testing that a known Array indexes without a VCALL..." << std::endl;

	const std::string source =
		"func read(a : Array, i : int):\n"
		"\treturn a[i]\n"
		"\n"
		"func write(a : Array, i : int, v):\n"
		"\ta[i] = v\n"
		"\n"
		"func both(a : Array, i : int, j : int):\n"
		"\ta[i] = a[j]\n";

	const IRProgram ir = compile_to_ir(source);

	assert(count_opcode(find_function(ir, "read"), IROpcode::ARRAY_GET) == 1);
	assert(count_vcalls(find_function(ir, "read"), "get") == 0);

	assert(count_opcode(find_function(ir, "write"), IROpcode::ARRAY_SET) == 1);
	assert(count_vcalls(find_function(ir, "write"), "set") == 0);

	const IRFunction& both = find_function(ir, "both");
	assert(count_opcode(both, IROpcode::ARRAY_GET) == 1);
	assert(count_opcode(both, IROpcode::ARRAY_SET) == 1);

	// An Array literal is as known as a declared one.
	const IRProgram literal = compile_to_ir(
		"func test(i : int):\n"
		"\tvar a = [1, 2, 3]\n"
		"\treturn a[i]\n");
	assert(count_opcode(find_function(literal, "test"), IROpcode::ARRAY_GET) == 1);

	compile_to_machine_code(source);

	std::cout << "  ✓ a known Array indexes without a VCALL" << std::endl;
}

static void test_unknown_container_keeps_the_vcall() {
	std::cout << "Testing that an unknown container keeps the VCALL..." << std::endl;

	// The backend reads the container's scoped index out of the Variant without
	// checking the type tag, so a container of unknown type may not take that
	// path.
	const IRProgram untyped_object = compile_to_ir(
		"func test(a, i : int):\n\treturn a[i]\n");
	assert(count_opcode(find_function(untyped_object, "test"), IROpcode::ARRAY_GET) == 0);
	assert(count_vcalls(find_function(untyped_object, "test"), "get") == 1);

	// Same for the index, which is read as an int64 out of its payload.
	const IRProgram untyped_index = compile_to_ir(
		"func test(a : Array, i):\n\treturn a[i]\n");
	assert(count_opcode(find_function(untyped_index, "test"), IROpcode::ARRAY_GET) == 0);
	assert(count_vcalls(find_function(untyped_index, "test"), "get") == 1);

	// Packed array: has get(), falls through to VCALL.
	const IRProgram packed_index = compile_to_ir(
		"func test(p : PackedInt32Array, i : int):\n\treturn p[i]\n");
	assert(count_opcode(find_function(packed_index, "test"), IROpcode::ARRAY_GET) == 0);
	assert(count_vcalls(find_function(packed_index, "test"), "get") == 1);

	// String: own syscall, no get(). See test_strings.cpp.
	const IRProgram string_index = compile_to_ir(
		"func test(s : String, i : int):\n\treturn s[i]\n");
	assert(count_vcalls(find_function(string_index, "test"), "get") == 0);

	// And a write to an unknown container.
	const IRProgram untyped_write = compile_to_ir(
		"func test(a, i : int, v):\n\ta[i] = v\n");
	assert(count_opcode(find_function(untyped_write, "test"), IROpcode::ARRAY_SET) == 0);
	assert(count_vcalls(find_function(untyped_write, "test"), "set") == 1);

	std::cout << "  ✓ an unknown container keeps the VCALL" << std::endl;
}

static void test_array_append() {
	std::cout << "Testing that append on a known Array is one system call..." << std::endl;

	const std::string source =
		"func test(v):\n"
		"\tvar a : Array = []\n"
		"\ta.append(v)\n"
		"\ta.push_back(v)\n"
		"\treturn a\n";

	const IRProgram ir = compile_to_ir(source);
	const IRFunction& test = find_function(ir, "test");
	assert(count_opcode(test, IROpcode::ARRAY_APPEND) == 2);
	assert(count_vcalls(test, "append") == 0);
	assert(count_vcalls(test, "push_back") == 0);

	// Any other method, and any receiver that is not a known Array, is a VCALL.
	const IRProgram mixed = compile_to_ir(
		"func test(a : Array, b, v):\n"
		"\ta.sort()\n"
		"\tb.append(v)\n");
	const IRFunction& other = find_function(mixed, "test");
	assert(count_opcode(other, IROpcode::ARRAY_APPEND) == 0);
	assert(count_vcalls(other, "sort") == 1);
	assert(count_vcalls(other, "append") == 1);

	compile_to_machine_code(source);

	std::cout << "  ✓ append on a known Array is one system call" << std::endl;
}

static void test_dictionary_element_access() {
	std::cout << "Testing that a known Dictionary keys without a VCALL..." << std::endl;

	constexpr int64_t DICT_OP_GET = 0;

	const std::string source =
		"func read(d : Dictionary, k):\n"
		"\treturn d[k]\n"
		"\n"
		"func write(d : Dictionary, k, v):\n"
		"\td[k] = v\n"
		"\n"
		"func by_name(d : Dictionary):\n"
		"\td.count = 1\n"
		"\treturn d.count\n";

	const IRProgram ir = compile_to_ir(source);

	// A Dictionary is keyed by any Variant, so unlike an Array index the key
	// needs no type of its own: the host is handed a pointer either way.
	assert(count_dict_ops(find_function(ir, "read"), DICT_OP_GET) == 1);
	assert(count_vcalls(find_function(ir, "read"), "get") == 0);

	assert(count_opcode(find_function(ir, "write"), IROpcode::DICT_SET) == 1);
	assert(count_vcalls(find_function(ir, "write"), "set") == 0);

	// `d.key` means `d["key"]` in GDScript, and takes the same path.
	const IRFunction& by_name = find_function(ir, "by_name");
	assert(count_dict_ops(by_name, DICT_OP_GET) == 1);
	assert(count_opcode(by_name, IROpcode::DICT_SET) == 1);
	assert(count_opcode(by_name, IROpcode::VGET) == 0);
	assert(count_opcode(by_name, IROpcode::VSET) == 0);

	compile_to_machine_code(source);

	std::cout << "  ✓ a known Dictionary keys without a VCALL" << std::endl;
}

static void test_element_access_survives_the_optimizer() {
	std::cout << "Testing that an element access is never optimized away..." << std::endl;

	// An unread write is still a write, and a read that throws on an
	// out-of-range index is still a read: IR_SIDE_EFFECTS keeps DCE off both.
	const std::string source =
		"func test(a : Array, d : Dictionary, i : int):\n"
		"\ta[i] = 1\n"
		"\td[i] = 2\n"
		"\ta.append(3)\n"
		"\tvar unused = a[i]\n"
		"\treturn 0\n";

	const IRProgram ir = compile_to_ir(source, true);
	const IRFunction& test = find_function(ir, "test");
	assert(count_opcode(test, IROpcode::ARRAY_SET) == 1);
	assert(count_opcode(test, IROpcode::DICT_SET) == 1);
	assert(count_opcode(test, IROpcode::ARRAY_APPEND) == 1);
	assert(count_opcode(test, IROpcode::ARRAY_GET) == 1);

	compile_to_machine_code(source);

	std::cout << "  ✓ an element access is never optimized away" << std::endl;
}

// `{k: v}` evaluates k; `{k = v}` is Lua-style (key is the string "k").
static void test_dictionary_literal_keys() {
	std::cout << "Testing dictionary literal keys..." << std::endl;

	// `{k: 2}` evaluates k; "k" not in the string pool.
	{
		const IRProgram ir = compile_to_ir(
			"func f():\n"
			"\tvar k = 7\n"
			"\treturn {k: 2}\n");
		const IRFunction& f = find_function(ir, "f");
		assert(count_opcode(f, IROpcode::MAKE_DICTIONARY) == 1);
		for (const auto& str : ir.string_constants) {
			assert(str != "k");
		}
	}

	// `{k = 2}` is Lua-style; "k" is in the string pool.
	{
		const IRProgram ir = compile_to_ir(
			"func f():\n"
			"\tvar k = 7\n"
			"\treturn {k = 2}\n");
		const IRFunction& f = find_function(ir, "f");
		assert(count_opcode(f, IROpcode::MAKE_DICTIONARY) == 1);
		assert(count_opcode(f, IROpcode::LOAD_STRING) == 1);
		bool found = false;
		for (const auto& str : ir.string_constants) {
			found = found || str == "k";
		}
		assert(found);
	}

	// Expression keys (not just identifiers).
	compile_to_machine_code(
		"func f():\n"
		"\tvar k = 7\n"
		"\treturn {k: 1, \"s\": 2, 1 + 1: 3, Vector2.ZERO: 4}\n");

	// String Lua-style key + trailing comma.
	compile_to_machine_code(
		"func f():\n"
		"\treturn {a = 1, \"b c\" = 2,}\n");

	// Mixing styles is refused.
	{
		bool refused = false;
		try {
			compile_to_ir("func f():\n\tvar v = 1\n\treturn {a = 1, v: 2}\n");
		} catch (const CompilerException&) {
			refused = true;
		}
		assert(refused);
	}
	{
		bool refused = false;
		try {
			compile_to_ir("func f():\n\tvar v = 1\n\treturn {v: 2, a = 1}\n");
		} catch (const CompilerException&) {
			refused = true;
		}
		assert(refused);
	}
	// A Lua-style key has to be a name, not any expression.
	{
		bool refused = false;
		try {
			compile_to_ir("func f():\n\treturn {1 = 2}\n");
		} catch (const CompilerException&) {
			refused = true;
		}
		assert(refused);
	}

	std::cout << "  dictionary literal keys OK" << std::endl;
}

int main() {
	std::cout << "=== Container Element Access Tests ===" << std::endl << std::endl;

	try {
		test_array_element_access();
		test_unknown_container_keeps_the_vcall();
		test_array_append();
		test_dictionary_element_access();
		test_dictionary_literal_keys();
		test_element_access_survives_the_optimizer();
	} catch (const CompilerException& e) {
		std::cerr << "Unexpected compiler error: " << e.what() << std::endl;
		return 1;
	}

	std::cout << std::endl << "All container tests passed." << std::endl;
	return 0;
}
