#include "../codegen.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "scope_stub.h"
#include "../../../syscalls.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
	if (condition) {
		return;
	}
	std::cerr << "FAILED: " << what << std::endl;
	failures++;
}

IRProgram compile_to_ir(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program ast = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(ast);
	IROptimizer optimizer;
	optimizer.optimize(ir);
	ir_verify(ir, "the optimizer");
	return ir;
}

bool holds_object(const IRProgram& ir, const std::string& name) {
	for (const auto& global : ir.globals) {
		if (global.name == name) {
			return global.holds_object;
		}
	}
	std::cerr << "FAILED: no global named " << name << std::endl;
	failures++;
	return false;
}

// Scan for `li a7, <number>` + `ecall` pairs in the instruction stream.
int count_syscalls(const std::vector<uint8_t>& code, int syscall) {
	if (syscall >= 2048) {
		std::cerr << "FAILED: syscall " << syscall << " no longer fits one addi" << std::endl;
		failures++;
		return -1;
	}
	const uint32_t li_a7 = (uint32_t(syscall) << 20) | (17u << 7) | 0x13u;
	const uint32_t ecall = 0x00000073u;

	int count = 0;
	for (size_t i = 0; i + 8 <= code.size(); i += 2) {
		uint32_t first = 0;
		uint32_t second = 0;
		std::memcpy(&first, code.data() + i, 4);
		std::memcpy(&second, code.data() + i + 4, 4);
		count += (first == li_a7 && second == ecall) ? 1 : 0;
	}
	return count;
}

int syscalls_in(const std::string& source, int syscall) {
	IRProgram ir = compile_to_ir(source);
	RISCVCodeGen backend;
	return count_syscalls(backend.generate(ir), syscall);
}

int retains_in(const std::string& source) {
	IRProgram ir = compile_to_ir(source);
	RISCVCodeGen backend;
	const std::vector<uint8_t> code = backend.generate(ir);
	return count_syscalls(code, ECALL_OBJ_RETAIN) + count_syscalls(code, ECALL_VSTORE_GLOBAL);
}

void test_a_class_typed_global_holds_an_object() {
	std::cout << "Testing which declarations are known to hold an object..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"var player: Node\n"
		"var res: Resource\n"
		"var count: int = 0\n"
		"var text: String = \"\"\n"
		"var items: Array = []\n"
		"var where: Vector2 = Vector2(0, 0)\n"
		"func test():\n\treturn count\n");

	check(holds_object(ir, "player"), "a Node-typed member holds an object");
	check(holds_object(ir, "res"), "a Resource-typed member holds an object");
	check(!holds_object(ir, "count"), "an int does not");
	check(!holds_object(ir, "text"), "a String does not");
	check(!holds_object(ir, "items"), "an Array does not");
	check(!holds_object(ir, "where"), "a Vector2 does not");

	std::cout << "  ✓ Class-typed declarations" << std::endl;
}

void test_what_is_declared_in_the_file_is_not_a_class() {
	std::cout << "Testing that structs, enums and nested classes are not objects..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"enum Mode { IDLE, RUN }\n"
		"struct Acct:\n"
		"\tvar balance = 0\n"
		"class Inner:\n"
		"\tvar x = 1\n"
		"var mode: Mode = Mode.IDLE\n"
		"var acct: Acct\n"
		"var inner: Inner\n"
		"func test():\n\treturn mode\n");

	check(!holds_object(ir, "mode"), "an enum-typed member is an integer");
	check(!holds_object(ir, "acct"), "a struct-typed member is a Dictionary");
	check(!holds_object(ir, "inner"), "a nested-class member is a Dictionary");

	std::cout << "  ✓ Declared-in-file types" << std::endl;
}

void test_an_untyped_global_is_learned_from_its_stores() {
	std::cout << "Testing that a store of a known object marks the global..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"var player = null\n"
		"var counter = null\n"
		"func setup():\n"
		"\tplayer = get_node(\"Player\")\n"
		"func bump():\n"
		"\tcounter = 1\n");

	check(holds_object(ir, "player"), "a global assigned a node holds an object");
	check(!holds_object(ir, "counter"), "a global assigned an integer does not");

	std::cout << "  ✓ Learned from stores" << std::endl;
}

void test_the_retain_reaches_the_instruction_stream() {
	std::cout << "Testing that the store emits the retain..." << std::endl;

	check(retains_in(
			  "var player: Node\n"
			  "func setup():\n"
			  "\tplayer = get_node(\"Player\")\n") == 1,
			"one retain per store into an object member");

	check(retains_in(
			  "var player: Node\n"
			  "func setup():\n"
			  "\tplayer = get_node(\"A\")\n"
			  "func again():\n"
			  "\tplayer = get_node(\"B\")\n") == 2,
			"one per store, so the second releases what the first held");

	check(retains_in(
			  "var count: int = 0\n"
			  "func tick():\n"
			  "\tcount += 1\n") == 0,
			"an int member never names an object");

	check(retains_in(
			  "var text: String = \"\"\n"
			  "func tick():\n"
			  "\ttext = \"hello\"\n") == 0,
			"nor does a String, which the host already owns");

	check(retains_in(
			  "var items: Array = []\n"
			  "func tick():\n"
			  "\titems.append(1)\n") == 0,
			"a container is a handle the host already owns");

	std::cout << "  ✓ Emitted where it is owed, and nowhere else" << std::endl;
}

void test_an_object_store_is_a_raw_move() {
	std::cout << "Testing that an object store does not go through VASSIGN..." << std::endl;

	// VASSIGN reads a 32-bit scoped-variant index, which drops OBJECT_HANDLE_TAG.
	const std::string unannotated =
		"var res = Resource.new()\n"
		"func get_it():\n"
		"\treturn res\n";
	check(syscalls_in(unannotated, ECALL_VASSIGN) == 0,
			"a global typed OBJECT from its initializer is stored without VASSIGN");
	check(syscalls_in(unannotated, ECALL_OBJ_RETAIN) == 1,
			"and the retain still reaches the slot the move just wrote");

	check(syscalls_in(
			  "var text = \"\"\n"
			  "func set_it(s):\n"
			  "\ttext = s\n",
			  ECALL_VASSIGN) >= 1,
			"a String global still takes VASSIGN");

	std::cout << "  ✓ Object stores are raw moves" << std::endl;
}

void test_an_untyped_slot_stores_through_the_host() {
	std::cout << "Testing which stores go through ECALL_VSTORE_GLOBAL..." << std::endl;

	const std::string object_member =
			  "var player: Node\n"
			  "func setup():\n"
			  "\tplayer = get_node(\"Player\")\n";
	check(syscalls_in(object_member, ECALL_VSTORE_GLOBAL) == 1,
			"a class-typed slot is untyped as far as the Variant tag goes");

	const std::string nullable_object_member =
			  "var player: Node?\n"
			  "func setup():\n"
			  "\tplayer = get_node(\"Player\")\n";
	check(syscalls_in(nullable_object_member, ECALL_VSTORE_GLOBAL) ==
			syscalls_in(object_member, ECALL_VSTORE_GLOBAL),
			"a nullable class slot uses the same host Variant store path");
	check(retains_in(nullable_object_member) == retains_in(object_member),
			"and nullable spelling does not add an object retain");

	check(syscalls_in(
			  "var anything = null\n"
			  "func setup():\n"
			  "\tanything = 1\n"
			  "func other():\n"
			  "\tanything = \"text\"\n",
			  ECALL_VSTORE_GLOBAL) == 2,
			"one per store into a slot whose type can change between calls");

	check(syscalls_in(
			  "var count: int = 0\n"
			  "var text: String = \"\"\n"
			  "var items: Array = []\n"
			  "func tick():\n"
			  "\tcount += 1\n"
			  "\ttext = \"hello\"\n"
			  "\titems.append(1)\n",
			  ECALL_VSTORE_GLOBAL) == 0,
			"a typed slot never needs the host to look at the tags");

	std::cout << "  \u2713 Untyped stores" << std::endl;
}

void test_a_local_holding_an_object_is_not_retained() {
	std::cout << "Testing that a local is not retained..." << std::endl;

	check(retains_in(
			  "func setup():\n"
			  "\tvar player = get_node(\"Player\")\n"
			  "\treturn player.get_name()\n") == 0,
			"a local object needs no retain");

	std::cout << "  ✓ Locals" << std::endl;
}

} // namespace

int main() {
	std::cout << "=== Object retain tests ===" << std::endl;
	test_a_class_typed_global_holds_an_object();
	test_what_is_declared_in_the_file_is_not_a_class();
	test_an_untyped_global_is_learned_from_its_stores();
	test_the_retain_reaches_the_instruction_stream();
	test_an_object_store_is_a_raw_move();
	test_an_untyped_slot_stores_through_the_host();
	test_a_local_holding_an_object_is_not_retained();

	if (failures > 0) {
		std::cerr << failures << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "All object retain tests passed" << std::endl;
	return 0;
}
