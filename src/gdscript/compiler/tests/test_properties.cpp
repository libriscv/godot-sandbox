// Property accessors: inline (`set(v): ... get: ...`) and named (`set = f, get = g`).
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
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

static bool has_function(const IRProgram& ir, const std::string& name) {
	for (const auto& func : ir.functions) {
		if (func.name == name) {
			return true;
		}
	}
	return false;
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

// Direct calls to `name`, by the callee in operand 0.
static int count_calls(const IRProgram& ir, const IRFunction& func, const std::string& name) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL && !instr.operands.empty() &&
			ir.strings[instr.operands[0].string_id] == name) {
			count++;
		}
	}
	return count;
}

// The callees of `func`, in order, so a get-then-set can be told from a
// set-then-get.
static std::vector<std::string> call_sequence(const IRProgram& ir, const IRFunction& func) {
	std::vector<std::string> names;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::CALL && !instr.operands.empty()) {
			names.push_back(ir.strings[instr.operands[0].string_id]);
		}
	}
	return names;
}

static const IRGlobalVar& find_global(const IRProgram& ir, const std::string& name) {
	for (const auto& global : ir.globals) {
		if (global.name == name) {
			return global;
		}
	}
	throw std::runtime_error("Global not found: " + name);
}

// The whole pipeline, so an accessor the backend cannot lower fails here
// rather than in a Godot project.
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

static CompilerException compile_failure(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException& e) {
		return e;
	}
	assert(false && "expected this source to be refused");
	throw std::runtime_error("unreachable");
}

// -= Tests =-

static void test_inline_bodies_are_lifted() {
	std::cout << "Testing that an inline accessor is lifted..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"var store = 0\n"
		"var hp = 10:\n"
		"\tset(v):\n"
		"\t\tstore = v\n"
		"\tget:\n"
		"\t\treturn store\n"
		"func test():\n"
		"\thp = 3\n"
		"\treturn hp\n");

	assert(has_function(ir, "@hp_setter"));
	assert(has_function(ir, "@hp_getter"));

	const IRFunction& test = find_function(ir, "test");
	assert(count_calls(ir, test, "@hp_setter") == 1);
	assert(count_calls(ir, test, "@hp_getter") == 1);
	assert(count_opcode(test, IROpcode::LOAD_GLOBAL) == 0);
	assert(count_opcode(test, IROpcode::STORE_GLOBAL) == 0);

	assert(find_function(ir, "@hp_setter").parameters.size() == 1);
	assert(find_function(ir, "@hp_getter").parameters.empty());

	std::cout << "  inline accessors are lifted" << std::endl;
}

static void test_named_accessors() {
	std::cout << "Testing 'set = f, get = g'..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"var store = 0\n"
		"var mp = 5:\n"
		"\tset = _set_mp,\n"
		"\tget = _get_mp\n"
		"func _set_mp(v):\n"
		"\tstore = v\n"
		"func _get_mp():\n"
		"\treturn store\n"
		"func test():\n"
		"\tmp = 4\n"
		"\treturn mp\n");

	assert(!has_function(ir, "@mp_setter"));
	assert(!has_function(ir, "@mp_getter"));

	const IRFunction& test = find_function(ir, "test");
	assert(count_calls(ir, test, "_set_mp") == 1);
	assert(count_calls(ir, test, "_get_mp") == 1);

	// One-line spelling.
	const IRProgram one_line = compile_to_ir(
		"var store = 0\n"
		"var mp: get = _get_mp, set = _set_mp\n"
		"func _set_mp(v):\n"
		"\tstore = v\n"
		"func _get_mp():\n"
		"\treturn store\n"
		"func test():\n"
		"\tmp = 4\n"
		"\treturn mp\n");
	const IRFunction& one_line_test = find_function(one_line, "test");
	assert(count_calls(one_line, one_line_test, "_set_mp") == 1);
	assert(count_calls(one_line, one_line_test, "_get_mp") == 1);

	std::cout << "  named accessors are called as written" << std::endl;
}

static void test_a_property_means_its_storage_inside_its_own_accessor() {
	std::cout << "Testing that an accessor sees the storage..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"var hp = 1:\n"
		"\tset(v):\n"
		"\t\thp = v * 2\n"
		"\tget:\n"
		"\t\treturn hp + 1\n"
		"func test():\n"
		"\thp = 5\n"
		"\treturn hp\n");

	const IRFunction& setter = find_function(ir, "@hp_setter");
	assert(count_opcode(setter, IROpcode::STORE_GLOBAL) == 1);
	assert(count_calls(ir, setter, "@hp_setter") == 0);

	const IRFunction& getter = find_function(ir, "@hp_getter");
	assert(count_opcode(getter, IROpcode::LOAD_GLOBAL) == 1);
	assert(count_calls(ir, getter, "@hp_getter") == 0);

	// `set = f` also gets direct storage access.
	const IRProgram named = compile_to_ir(
		"var ind = 5:\n"
		"\tset = _si\n"
		"func _si(v):\n"
		"\tind = v\n"
		"func test():\n"
		"\tind = 1\n");
	const IRFunction& si = find_function(named, "_si");
	assert(count_opcode(si, IROpcode::STORE_GLOBAL) == 1);
	assert(count_calls(named, si, "_si") == 0);

	// Cross-property access goes through the accessor.
	const IRProgram other = compile_to_ir(
		"var a = 1:\n"
		"\tget:\n"
		"\t\treturn 7\n"
		"var b = 2:\n"
		"\tget:\n"
		"\t\treturn a\n");
	assert(count_calls(other, find_function(other, "@b_getter"), "@a_getter") == 1);

	std::cout << "  an accessor reaches the storage behind its own property" << std::endl;
}

static void test_the_initializer_does_not_run_the_setter() {
	std::cout << "Testing that the initializer skips the setter..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"var seen = 0\n"
		"var hp = 10:\n"
		"\tset(v):\n"
		"\t\tseen += 1\n"
		"\t\thp = v\n");
	assert(find_global(ir, "hp").init_type == IRGlobalVar::InitType::INT);
	assert(count_calls(ir, ir.member_init, "@hp_setter") == 0);

	const IRProgram runtime = compile_to_ir(
		"var base = 4\n"
		"var hp = base + 1:\n"
		"\tset(v):\n"
		"\t\thp = v\n");
	assert(find_global(runtime, "hp").init_type == IRGlobalVar::InitType::RUNTIME);
	assert(count_calls(runtime, runtime.member_init, "@hp_setter") == 0);
	assert(count_opcode(runtime.member_init, IROpcode::STORE_GLOBAL) == 1);

	std::cout << "  the declaration's own value is written, not set" << std::endl;
}

static void test_compound_assignment_gets_then_sets() {
	std::cout << "Testing 'hp += 1'..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"var store = 0\n"
		"var hp = 1:\n"
		"\tset(v):\n"
		"\t\tstore = v\n"
		"\tget:\n"
		"\t\treturn store\n"
		"func test():\n"
		"\thp += 5\n");

	const std::vector<std::string> calls = call_sequence(ir, find_function(ir, "test"));
	assert(calls.size() == 2);
	assert(calls[0] == "@hp_getter");
	assert(calls[1] == "@hp_setter");

	std::cout << "  a compound assignment reads before it writes" << std::endl;
}

static void test_one_sided_properties() {
	std::cout << "Testing a property with one accessor..." << std::endl;

	// Getter only: writes go to storage.
	const IRProgram getter_only = compile_to_ir(
		"var ro = 1:\n"
		"\tget:\n"
		"\t\treturn 99\n"
		"func test():\n"
		"\tro = 5\n"
		"\treturn ro\n");
	const IRFunction& ro_test = find_function(getter_only, "test");
	assert(count_opcode(ro_test, IROpcode::STORE_GLOBAL) == 1);
	assert(count_calls(getter_only, ro_test, "@ro_getter") == 1);

	// Setter only: reads come from storage.
	const IRProgram setter_only = compile_to_ir(
		"var wo = 2:\n"
		"\tset(v):\n"
		"\t\two = v * 3\n"
		"func test():\n"
		"\two = 4\n"
		"\treturn wo\n");
	const IRFunction& wo_test = find_function(setter_only, "test");
	assert(count_opcode(wo_test, IROpcode::LOAD_GLOBAL) == 1);
	assert(count_calls(setter_only, wo_test, "@wo_setter") == 1);

	std::cout << "  the missing half falls back to the storage" << std::endl;
}

static void test_an_export_publishes_both_accessors() {
	std::cout << "Testing @export with accessors..." << std::endl;

	// Missing half is generated for the host.
	const IRProgram ir = compile_to_ir(
		"@export var hp = 10:\n"
		"\tset(v):\n"
		"\t\thp = v\n"
		"func test():\n"
		"\treturn hp\n");

	const IRGlobalVar& hp = find_global(ir, "hp");
	assert(hp.is_property);
	assert(hp.setter_function == "@hp_setter");
	assert(hp.getter_function == "@hp_getter");
	assert(has_function(ir, "@hp_getter"));

	// Guest still reads storage directly (no getter of its own).
	assert(count_opcode(find_function(ir, "test"), IROpcode::LOAD_GLOBAL) == 1);
	assert(count_calls(ir, find_function(ir, "test"), "@hp_getter") == 0);

	// No accessors: empty names, host uses direct path.
	const IRProgram plain = compile_to_ir("@export var speed = 1.0\n");
	assert(find_global(plain, "speed").is_property);
	assert(find_global(plain, "speed").setter_function.empty());
	assert(find_global(plain, "speed").getter_function.empty());

	std::cout << "  an exported property answers Godot both ways" << std::endl;
}

static void test_accessors_reach_machine_code() {
	std::cout << "Testing the whole pipeline..." << std::endl;

	compile_to_machine_code(
		"var store = 0\n"
		"@export var hp: int = 10:\n"
		"\tset(v):\n"
		"\t\thp = v if v > 0 else 0\n"
		"\tget:\n"
		"\t\treturn hp\n"
		"@export var mp = 5:\n"
		"\tset = _set_mp\n"
		"var lazy:\n"
		"\tget(): return store + 1\n"
		"func _set_mp(v):\n"
		"\tstore = v\n"
		"func test():\n"
		"\thp += 1\n"
		"\tmp = 2\n"
		"\treturn [hp, mp, lazy]\n");

	// No type, no initializer: NIL is a valid initial storage value.
	const IRProgram ir = compile_to_ir(
		"var store = 1\n"
		"var lazy:\n"
		"\tget:\n"
		"\t\treturn store\n");
	assert(find_global(ir, "lazy").init_type == IRGlobalVar::InitType::NULL_VAL);

	std::cout << "  accessors survive the optimizer and the backend" << std::endl;
}

static void test_a_one_line_accessor_body() {
	std::cout << "Testing a one-line accessor body..." << std::endl;

	const IRProgram one_line = compile_to_ir(
		"var store = 0\n"
		"var hp = 1: set(v): store = v\n"
		"func test():\n"
		"\thp = 3\n");
	const IRProgram block = compile_to_ir(
		"var store = 0\n"
		"var hp = 1:\n"
		"\tset(v):\n"
		"\t\tstore = v\n"
		"func test():\n"
		"\thp = 3\n");

	const IRFunction& one_line_setter = find_function(one_line, "@hp_setter");
	const IRFunction& block_setter = find_function(block, "@hp_setter");
	assert(one_line_setter.instructions.size() == block_setter.instructions.size());
	for (size_t i = 0; i < block_setter.instructions.size(); i++) {
		assert(one_line_setter.instructions[i].to_string() ==
			block_setter.instructions[i].to_string());
	}

	// `get(): ...` too, and both accessors on the declaration's own line.
	compile_to_machine_code(
		"var store = 0\n"
		"var hp = 1: set(v): store = v, get(): return store\n"
		"func test():\n"
		"\thp += 1\n"
		"\treturn hp\n");

	std::cout << "  a one-line accessor is the same body" << std::endl;
}

static void test_refusals() {
	std::cout << "Testing what is refused..." << std::endl;

	// Named accessor: must exist with correct arity.
	compile_failure("var x = 1:\n\tset = missing\n");
	compile_failure("var x = 1:\n\tset = f\nfunc f(a, b):\n\treturn a\n");
	compile_failure("var x = 1:\n\tget = f\nfunc f(a):\n\treturn a\n");

	compile_failure("const C = 1:\n\tget:\n\t\treturn 2\n");

	// One of each, at most.
	compile_failure("var x = 1:\n\tget:\n\t\treturn 1\n\tget:\n\t\treturn 2\n");
	compile_failure("var x = 1:\n\tset(v):\n\t\tpass\n\tset(v):\n\t\tpass\n");

	compile_failure("var x = 1:\n\tset:\n\t\tpass\n");

	compile_failure("var x = 1:\n\tvalue = 2\n");
	compile_failure("var x = 1: pass\n");

	// No await inside an accessor.
	compile_failure("var x = 1:\n\tget:\n\t\treturn await something()\n");

	std::cout << "  refusals are refusals" << std::endl;
}

static void test_a_local_shadows_the_property() {
	std::cout << "Testing that a local shadows a property..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"var hp = 1:\n"
		"\tset(v):\n"
		"\t\thp = v\n"
		"\tget:\n"
		"\t\treturn 9\n"
		"func test():\n"
		"\tvar hp = 2\n"
		"\thp = 3\n"
		"\treturn hp\n");

	const IRFunction& test = find_function(ir, "test");
	assert(call_sequence(ir, test).empty());

	std::cout << "  a local of the same name is a local" << std::endl;
}

int main() {
	std::cout << "=== Property Accessor Tests ===" << std::endl << std::endl;

	try {
		test_inline_bodies_are_lifted();
		test_named_accessors();
		test_a_property_means_its_storage_inside_its_own_accessor();
		test_the_initializer_does_not_run_the_setter();
		test_compound_assignment_gets_then_sets();
		test_one_sided_properties();
		test_an_export_publishes_both_accessors();
		test_accessors_reach_machine_code();
		test_a_one_line_accessor_body();
		test_refusals();
		test_a_local_shadows_the_property();
	} catch (const CompilerException& e) {
		std::cerr << "Unexpected compiler error: " << e.what() << std::endl;
		return 1;
	}

	std::cout << std::endl << "All property accessor tests passed." << std::endl;
	return 0;
}
