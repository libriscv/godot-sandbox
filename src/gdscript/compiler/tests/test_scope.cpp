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
#include <set>
#include <string>
#include <vector>

using namespace gdscript;

static int failures = 0;

static void check(bool condition, const std::string& what) {
	if (!condition) {
		std::cerr << "FAILED: " << what << std::endl;
		failures++;
	}
}

static IRProgram compile_to_ir(const std::string& source, bool optimize) {
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

static size_t index_of(const IRFunction& func, IROpcode opcode) {
	for (size_t i = 0; i < func.instructions.size(); i++) {
		if (func.instructions[i].opcode == opcode) {
			return i;
		}
	}
	return func.instructions.size();
}

static void test_every_loop_form_takes_a_scope() {
	struct Case {
		const char* what;
		const char* source;
		int scopes;
	};
	const Case cases[] = {
		{ "for over range", "func test():\n\tfor i in range(4):\n\t\tvar d = {\"a\": i}\n\treturn 1\n", 1 },
		{ "for over an array", "func test():\n\tfor v in [1, 2, 3]:\n\t\tvar d = {\"a\": v}\n\treturn 1\n", 1 },
		// A string walk takes two: one holding the batch of characters, one
		// holding what the body makes from each of them.
		{ "for over a string", "func test():\n\tfor c in \"abc\":\n\t\tvar s = c + \"!\"\n\treturn 1\n", 2 },
		{ "while", "func test():\n\tvar i = 0\n\twhile i < 4:\n\t\tvar d = {\"a\": i}\n\t\ti += 1\n\treturn 1\n", 1 },
	};
	for (const Case& c : cases) {
		const IRProgram ir = compile_to_ir(c.source, /*optimize=*/true);
		const IRFunction& func = find_function(ir, "test");
		check(count_opcode(func, IROpcode::SCOPE_MARK) == c.scopes,
			std::string(c.what) + ": marks");
		check(count_opcode(func, IROpcode::SCOPE_RELEASE) == c.scopes,
			std::string(c.what) + ": releases");
	}
}

static void test_release_is_the_first_thing_in_the_loop() {
	const IRProgram ir = compile_to_ir(
		"func test():\n"
		"\tvar n = 0\n"
		"\tfor i in range(8):\n"
		"\t\tif i == 2:\n"
		"\t\t\tcontinue\n"
		"\t\tvar d = {\"a\": i}\n"
		"\t\tn += 1\n"
		"\treturn n\n", /*optimize=*/true);
	const IRFunction& func = find_function(ir, "test");

	const size_t mark = index_of(func, IROpcode::SCOPE_MARK);
	const size_t release = index_of(func, IROpcode::SCOPE_RELEASE);
	check(mark < func.instructions.size(), "a mark was emitted");
	check(release < func.instructions.size(), "a release was emitted");
	if (mark >= func.instructions.size() || release >= func.instructions.size()) {
		return;
	}
	check(func.instructions[mark + 1].opcode == IROpcode::LABEL,
		"the mark sits directly above the loop label");
	check(release == mark + 2, "the release is the first instruction after the label");

	const std::string loop_label = std::get<std::string>(func.instructions[mark + 1].operands[0].value);
	bool jumps_back = false;
	for (const auto& instr : func.instructions) {
		if (instr.opcode != IROpcode::JUMP) {
			continue;
		}
		if (std::get<std::string>(instr.operands[0].value) == loop_label) {
			jumps_back = true;
		}
	}
	check(jumps_back, "the back edge targets the label the release stands under");
}

static void test_the_mark_stays_below_hoisted_code() {
	const std::string source =
		"func test():\n"
		"\tvar n = 0\n"
		"\tfor i in range(8):\n"
		"\t\tvar d = {\"key\": i}\n"
		"\t\tn += d[\"key\"]\n"
		"\treturn n\n";
	const IRProgram ir = compile_to_ir(source, /*optimize=*/true);
	const IRFunction& func = find_function(ir, "test");
	const size_t mark = index_of(func, IROpcode::SCOPE_MARK);
	check(mark < func.instructions.size(), "a mark survived the pipeline");
	if (mark >= func.instructions.size()) {
		return;
	}
	check(func.instructions[mark + 1].opcode == IROpcode::LABEL,
		"nothing stands between the mark and the loop label");
	bool string_above = false;
	for (size_t i = 0; i < mark; i++) {
		if (func.instructions[i].opcode == IROpcode::LOAD_STRING) {
			string_above = true;
		}
	}
	check(string_above, "the hoisted constant was left above the mark");
}

static void test_nested_loops_take_distinct_ids() {
	const IRProgram ir = compile_to_ir(
		"func test():\n"
		"\tvar n = 0\n"
		"\tfor i in range(3):\n"
		"\t\tfor j in range(3):\n"
		"\t\t\tvar d = {\"a\": j}\n"
		"\t\t\tn += 1\n"
		"\treturn n\n", /*optimize=*/true);
	const IRFunction& func = find_function(ir, "test");
	std::set<int64_t> ids;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::SCOPE_MARK) {
			ids.insert(std::get<int64_t>(instr.operands[0].value));
		}
	}
	check(ids.size() == 2, "two loops, two mark slots");
	check(count_opcode(func, IROpcode::SCOPE_RELEASE) == 2, "two releases");
}

static void test_a_coroutine_takes_no_scope() {
	const IRProgram ir = compile_to_ir(
		"signal ready\n"
		"func test():\n"
		"\tfor i in range(4):\n"
		"\t\tvar d = {\"a\": i}\n"
		"\t\tawait ready\n"
		"\treturn 1\n", /*optimize=*/true);
	const IRFunction& func = find_function(ir, "test");
	check(func.is_coroutine, "the function is a coroutine");
	check(count_opcode(func, IROpcode::SCOPE_MARK) == 0, "a coroutine takes no mark");
	check(count_opcode(func, IROpcode::SCOPE_RELEASE) == 0, "a coroutine takes no release");
}

static void test_a_loopless_function_is_untouched() {
	const IRProgram ir = compile_to_ir(
		"func test():\n\tvar d = {\"a\": 1}\n\treturn d[\"a\"]\n", /*optimize=*/true);
	const IRFunction& func = find_function(ir, "test");
	check(count_opcode(func, IROpcode::SCOPE_MARK) == 0, "no loop, no mark");
	check(count_opcode(func, IROpcode::SCOPE_RELEASE) == 0, "no loop, no release");
}

static void test_the_ir_verifies() {
	const std::string source =
		"var total = 0\n"
		"func test():\n"
		"\tfor i in range(8):\n"
		"\t\tvar d = {\"a\": i}\n"
		"\t\ttotal += d[\"a\"]\n"
		"\treturn total\n";
	IRProgram ir = compile_to_ir(source, /*optimize=*/false);
	for (const auto& func : ir.functions) {
		ir_verify(func, "codegen");
	}
	IROptimizer optimizer;
	optimizer.optimize(ir);
	for (const auto& func : ir.functions) {
		ir_verify(func, "the optimizer");
	}
	check(true, "the IR verifies before and after the optimizer");
}

static std::vector<uint8_t> compile_to_elf(const std::string& source) {
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

static bool contains_word(const std::vector<uint8_t>& elf, uint32_t word) {
	if (elf.size() < 4) {
		return false;
	}
	for (size_t i = 0; i + 4 <= elf.size(); i++) {
		const uint32_t at = uint32_t(elf[i]) | (uint32_t(elf[i + 1]) << 8) |
			(uint32_t(elf[i + 2]) << 16) | (uint32_t(elf[i + 3]) << 24);
		if (at == word) {
			return true;
		}
	}
	return false;
}

static uint32_t li_a7_vscope() {
	return (uint32_t(ECALL_VSCOPE) << 20) | (0u << 15) | (0u << 12) | (17u << 7) | 0x13u;
}

static uint32_t sw_zero_sp(int offset) {
	const uint32_t imm11_5 = (uint32_t(offset) >> 5) & 0x7f;
	const uint32_t imm4_0 = uint32_t(offset) & 0x1f;
	return (imm11_5 << 25) | (0u << 20) | (2u << 15) | (2u << 12) | (imm4_0 << 7) | 0x23u;
}

static void test_the_backend_emits_the_syscall_and_zeroes_the_frame() {
	// Append keeps the Dictionary alive; an unread one gets DCE'd.
	const std::vector<uint8_t> elf = compile_to_elf(
		"func test():\n"
		"\tvar n = []\n"
		"\tfor i in range(8):\n"
		"\t\tn.append({\"a\": i})\n"
		"\treturn n\n");
	check(contains_word(elf, li_a7_vscope()), "the loop reaches ECALL_VSCOPE");
	check(contains_word(elf, sw_zero_sp(16)),
		"the frame is zeroed, so an untouched slot cannot read as a handle");

	const std::vector<uint8_t> loopless = compile_to_elf(
		"func test():\n\tvar d = {\"a\": 1}\n\treturn d\n");
	check(!contains_word(loopless, li_a7_vscope()),
		"a function with no loop makes no scope syscall");
	check(!contains_word(loopless, sw_zero_sp(16)),
		"a function with no loop is not zeroed for one");
}

static void test_a_host_free_loop_is_not_scoped() {
	const std::vector<uint8_t> ints = compile_to_elf(
		"func test(n : int) -> int:\n"
		"\tvar acc : int = 0\n"
		"\tvar i : int = 0\n"
		"\twhile i < n:\n"
		"\t\tacc += i * 3 - (i >> 2)\n"
		"\t\ti += 1\n"
		"\treturn acc\n");
	check(!contains_word(ints, li_a7_vscope()), "a typed int loop makes no scope syscall");
	check(!contains_word(ints, sw_zero_sp(16)), "a typed int loop is not zeroed for one");

	const std::vector<uint8_t> floats = compile_to_elf(
		"func test(n : int) -> float:\n"
		"\tvar acc : float = 0.0\n"
		"\tvar i : int = 0\n"
		"\twhile i < n:\n"
		"\t\tacc += float(i) * 0.5 - 0.25\n"
		"\t\ti += 1\n"
		"\treturn acc\n");
	check(!contains_word(floats, li_a7_vscope()), "a typed float loop makes no scope syscall");

	// Untyped arithmetic reaches Variant::evaluate; scope stays.
	const std::vector<uint8_t> untyped = compile_to_elf(
		"func test(n, step):\n"
		"\tvar acc = 0\n"
		"\tvar i = 0\n"
		"\twhile i < n:\n"
		"\t\tacc += step\n"
		"\t\ti += 1\n"
		"\treturn acc\n");
	check(contains_word(untyped, li_a7_vscope()), "an untyped loop keeps its scope");

	const std::vector<uint8_t> stores = compile_to_elf(
		"func test(n : int) -> Dictionary:\n"
		"\tvar d : Dictionary = {}\n"
		"\tvar a : Array = []\n"
		"\tvar i : int = 0\n"
		"\twhile i < n:\n"
		"\t\td[i] = i\n"
		"\t\ta.append(i)\n"
		"\t\ta[0] = i\n"
		"\t\ti += 1\n"
		"\treturn d\n");
	check(!contains_word(stores, li_a7_vscope()), "a loop that only stores makes no scope syscall");

	const std::vector<uint8_t> nested = compile_to_elf(
		"func test(n : int) -> Array:\n"
		"\tvar out : Array = []\n"
		"\tvar i : int = 0\n"
		"\twhile i < n:\n"
		"\t\tvar acc : int = 0\n"
		"\t\tvar j : int = 0\n"
		"\t\twhile j < 4:\n"
		"\t\t\tacc += j\n"
		"\t\t\tj += 1\n"
		"\t\tout.append(str(acc))\n"
		"\t\ti += 1\n"
		"\treturn out\n");
	check(contains_word(nested, li_a7_vscope()), "the allocating outer loop keeps its scope");
}

int main() {
	try {
		test_every_loop_form_takes_a_scope();
		test_release_is_the_first_thing_in_the_loop();
		test_the_mark_stays_below_hoisted_code();
		test_nested_loops_take_distinct_ids();
		test_a_coroutine_takes_no_scope();
		test_a_loopless_function_is_untouched();
		test_the_ir_verifies();
		test_the_backend_emits_the_syscall_and_zeroes_the_frame();
		test_a_host_free_loop_is_not_scoped();
	} catch (const CompilerException& e) {
		std::cerr << "FAILED: compiler exception: " << e.what() << std::endl;
		failures++;
	} catch (const std::exception& e) {
		std::cerr << "FAILED: " << e.what() << std::endl;
		failures++;
	}

	if (failures > 0) {
		std::cerr << failures << " scope test(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All scope tests passed" << std::endl;
	return 0;
}
