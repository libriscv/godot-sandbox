// LOAD_RESOURCE vs LOAD_RESOURCE_VAR lowering and DCE immunity.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include "../syscall_numbers.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

using namespace gdscript;


static IRProgram compile_to_ir(const std::string& source, bool optimize = false) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
		ir_verify(ir, "the optimizer");
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

static bool refuses(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException&) {
		return true;
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

static std::string embedded_path(const IRFunction& func) {
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::LOAD_RESOURCE) {
			return std::get<std::string>(instr.operands[1].value);
		}
	}
	throw std::runtime_error("No LOAD_RESOURCE in " + func.name);
}

static std::vector<uint8_t> machine_code(const std::string& source) {
	IRProgram ir = compile_to_ir(source, /*optimize=*/true);
	RISCVCodeGen backend;
	std::vector<uint8_t> code = backend.generate(ir);
	assert(!code.empty());
	return code;
}

static int count_instruction(const std::vector<uint8_t>& code, uint32_t instruction) {
	int count = 0;
	for (size_t i = 0; i + 4 <= code.size(); i += 4) {
		uint32_t word = 0;
		std::memcpy(&word, &code[i], 4);
		if (word == instruction) {
			count++;
		}
	}
	return count;
}

static constexpr uint32_t li(int rd, int imm) {
	return (static_cast<uint32_t>(imm & 0xFFF) << 20) | (static_cast<uint32_t>(rd) << 7) | 0x13;
}

static constexpr int REG_A1 = 11;
static constexpr int REG_A7 = 17;

static void test_literal_path_is_embedded() {
	std::cout << "Testing that a literal path costs no String Variant..." << std::endl;

	const std::string source = "func test():\n\treturn load(\"res://icon.svg\")\n";
	const IRProgram ir = compile_to_ir(source);
	const IRFunction& test = find_function(ir, "test");

	assert(count_opcode(test, IROpcode::LOAD_RESOURCE) == 1);
	assert(embedded_path(test) == "res://icon.svg");

	assert(count_opcode(test, IROpcode::LOAD_STRING) == 0);
	assert(count_opcode(test, IROpcode::LOAD_RESOURCE_VAR) == 0);
	assert(count_opcode(test, IROpcode::VCALL) == 0);
	assert(count_opcode(test, IROpcode::CALL) == 0);

	for (const auto& instr : test.instructions) {
		if (instr.opcode == IROpcode::LOAD_RESOURCE) {
			assert(instr.type_hint == Variant::OBJECT);
		}
	}
}

static void test_const_path_is_embedded() {
	std::cout << "Testing that a const path folds into the instruction..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"const ICON = \"res://icon.svg\"\n"
		"func test():\n"
		"\treturn load(ICON)\n");
	const IRFunction& test = find_function(ir, "test");

	assert(count_opcode(test, IROpcode::LOAD_RESOURCE) == 1);
	assert(embedded_path(test) == "res://icon.svg");
	assert(count_opcode(test, IROpcode::LOAD_RESOURCE_VAR) == 0);

	// Local shadows the const.
	const IRProgram shadowed = compile_to_ir(
		"const ICON = \"res://icon.svg\"\n"
		"func test():\n"
		"\tvar ICON = \"res://other.svg\"\n"
		"\treturn load(ICON)\n");
	const IRFunction& shadowed_fn = find_function(shadowed, "test");
	assert(count_opcode(shadowed_fn, IROpcode::LOAD_RESOURCE) == 0);
	assert(count_opcode(shadowed_fn, IROpcode::LOAD_RESOURCE_VAR) == 1);

	const IRProgram number = compile_to_ir(
		"const N = 5\n"
		"func test():\n"
		"\treturn load(N)\n");
	assert(count_opcode(find_function(number, "test"), IROpcode::LOAD_RESOURCE) == 0);
	assert(count_opcode(find_function(number, "test"), IROpcode::LOAD_RESOURCE_VAR) == 1);
}

static void test_runtime_path_is_a_variant() {
	std::cout << "Testing that a computed path goes as a Variant..." << std::endl;

	static const char* sources[] = {
		"func test(p):\n\treturn load(p)\n",
		"func test(dir):\n\treturn load(dir + \"/icon.svg\")\n",
		"func test(a : Array):\n\treturn load(a[0])\n",
	};

	for (const char* source : sources) {
		const IRProgram ir = compile_to_ir(source);
		const IRFunction& test = find_function(ir, "test");
		assert(count_opcode(test, IROpcode::LOAD_RESOURCE_VAR) == 1);
		assert(count_opcode(test, IROpcode::LOAD_RESOURCE) == 0);
		for (const auto& instr : test.instructions) {
			if (instr.opcode == IROpcode::LOAD_RESOURCE_VAR) {
				assert(instr.type_hint == Variant::OBJECT);
			}
		}
	}
}

static void test_call_survives_the_optimizer() {
	std::cout << "Testing that an unused load() is still performed..." << std::endl;

	// Side-effectful: DCE must not delete either form.
	const IRProgram literal = compile_to_ir(
		"func test():\n\tload(\"res://icon.svg\")\n\treturn 1\n", /*optimize=*/true);
	assert(count_opcode(find_function(literal, "test"), IROpcode::LOAD_RESOURCE) == 1);

	const IRProgram computed = compile_to_ir(
		"func test(p):\n\tload(p)\n\treturn 1\n", /*optimize=*/true);
	assert(count_opcode(find_function(computed, "test"), IROpcode::LOAD_RESOURCE_VAR) == 1);

	// Duplicate paths are distinct calls.
	const IRProgram twice = compile_to_ir(
		"func test():\n"
		"\tvar a = load(\"res://icon.svg\")\n"
		"\tvar b = load(\"res://icon.svg\")\n"
		"\treturn a == b\n", /*optimize=*/true);
	assert(count_opcode(find_function(twice, "test"), IROpcode::LOAD_RESOURCE) == 2);
}

static void test_emitted_syscall() {
	std::cout << "Testing the emitted ECALL_LOAD..." << std::endl;

	const std::vector<uint8_t> literal =
		machine_code("func test():\n\treturn load(\"res://icon.svg\")\n");
	assert(count_instruction(literal, li(REG_A7, ECALL_LOAD)) == 1);
	// A1 = length (characters).
	assert(count_instruction(literal, li(REG_A1, 14)) == 1);
	assert(count_instruction(literal, li(REG_A1, -1)) == 0);

	const std::vector<uint8_t> computed = machine_code("func test(p):\n\treturn load(p)\n");
	assert(count_instruction(computed, li(REG_A7, ECALL_LOAD)) == 1);
	// A1 = -1 (Variant path).
	assert(count_instruction(computed, li(REG_A1, -1)) == 1);
}

static void test_refusals() {
	std::cout << "Testing what load() will not compile..." << std::endl;

	assert(refuses("func test():\n\treturn load()\n"));
	assert(refuses("func test():\n\treturn load(\"res://a.tscn\", \"res://b.tscn\")\n"));

	assert(refuses("func test():\n\treturn preload(\"res://a.tscn\")\n"));

	// Local function shadows the global.
	const IRProgram own = compile_to_ir(
		"func load(x):\n\treturn x\n"
		"func test():\n\treturn load(\"res://icon.svg\")\n");
	const IRFunction& test = find_function(own, "test");
	assert(count_opcode(test, IROpcode::LOAD_RESOURCE) == 0);
	assert(count_opcode(test, IROpcode::LOAD_RESOURCE_VAR) == 0);
	assert(count_opcode(test, IROpcode::CALL) == 1);
}

int main() {
	std::cout << "=== load() tests ===" << std::endl;

	test_literal_path_is_embedded();
	test_const_path_is_embedded();
	test_runtime_path_is_a_variant();
	test_call_survives_the_optimizer();
	test_emitted_syscall();
	test_refusals();

	std::cout << "All load() tests passed" << std::endl;
	return 0;
}
