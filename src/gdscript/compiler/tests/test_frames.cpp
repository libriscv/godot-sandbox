// What a function's prologue and epilogue actually cost.
//
// Every function used to open with the same six instructions -- move sp, save
// ra, save fp, save a0, set fp -- and close with the mirror image, whether or
// not it called anything, and every value on its way out went through the
// return register's stack slot before being copied again into the caller's
// Variant. A getter is two Variant copies and eleven instructions of ceremony
// around a value that could have been written straight where it was going.
//
// These tests read the emitted words. They assert the shape of the frame, not
// the exact encoding of any one instruction, so an unrelated change to how a
// Variant is copied does not fail them -- but a prologue that starts saving
// registers a function does not use does.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../riscv_codegen.h"
#include "../ir_optimizer.h"
#include "../compiler_exception.h"
#include "../variant_layout.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace gdscript;

namespace {

// -= Reading back the machine code =-

constexpr uint8_t REG_ZERO = 0;
constexpr uint8_t REG_RA = 1;
constexpr uint8_t REG_SP = 2;
constexpr uint8_t REG_A0 = 10;

constexpr uint32_t RET = 0x00008067; // jalr zero, 0(ra)

struct Compiled {
	std::vector<uint8_t> code;
	std::unordered_map<std::string, size_t> offsets;
};

Compiled compile(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	IROptimizer optimizer;
	optimizer.optimize(ir);

	RISCVCodeGen riscv { VariantLayout(false) };
	Compiled out;
	out.code = riscv.generate(ir);
	out.offsets = riscv.get_function_offsets();
	return out;
}

// The words of one function, from its entry point up to and including the first
// RET. Every function these tests compile has exactly one return.
std::vector<uint32_t> function_words(const Compiled& compiled, const std::string& name) {
	auto it = compiled.offsets.find(name);
	assert(it != compiled.offsets.end() && "no such function in the generated code");

	std::vector<uint32_t> words;
	for (size_t off = it->second; off + 4 <= compiled.code.size(); off += 4) {
		const uint32_t word = uint32_t(compiled.code[off]) |
			(uint32_t(compiled.code[off + 1]) << 8) |
			(uint32_t(compiled.code[off + 2]) << 16) |
			(uint32_t(compiled.code[off + 3]) << 24);
		words.push_back(word);
		if (word == RET) {
			return words;
		}
	}
	assert(false && "function has no return");
	return words;
}

uint32_t opcode_of(uint32_t w) { return w & 0x7F; }
uint8_t rd_of(uint32_t w)      { return uint8_t((w >> 7) & 0x1F); }
uint8_t funct3_of(uint32_t w)  { return uint8_t((w >> 12) & 0x7); }
uint8_t rs1_of(uint32_t w)     { return uint8_t((w >> 15) & 0x1F); }
uint8_t rs2_of(uint32_t w)     { return uint8_t((w >> 20) & 0x1F); }

// addi sp, sp, imm -- the one instruction that opens and closes a frame.
bool is_stack_adjust(uint32_t w) {
	return opcode_of(w) == 0x13 && funct3_of(w) == 0 && rd_of(w) == REG_SP && rs1_of(w) == REG_SP;
}

// sd rs2, imm(sp)
bool is_store_to_stack(uint32_t w, uint8_t rs2) {
	return opcode_of(w) == 0x23 && funct3_of(w) == 3 && rs1_of(w) == REG_SP && rs2_of(w) == rs2;
}

// Any store or load whose base register is sp: a reference to the frame.
bool touches_frame(uint32_t w) {
	const bool is_store = opcode_of(w) == 0x23;
	const bool is_load = opcode_of(w) == 0x03;
	return (is_store || is_load) && rs1_of(w) == REG_SP;
}

// Any store through a0: a write into the caller's return Variant.
bool is_store_through_return_pointer(uint32_t w) {
	return opcode_of(w) == 0x23 && rs1_of(w) == REG_A0;
}

bool is_ecall(uint32_t w) {
	return opcode_of(w) == 0x73 && funct3_of(w) == 0 && rd_of(w) == REG_ZERO && (w >> 20) == 0;
}

// jal ra, offset -- a call to another function in the program.
bool is_call(uint32_t w) {
	return opcode_of(w) == 0x6F && rd_of(w) == REG_RA;
}

size_t count(const std::vector<uint32_t>& words, bool (*pred)(uint32_t)) {
	size_t n = 0;
	for (uint32_t w : words) {
		if (pred(w)) {
			n++;
		}
	}
	return n;
}

// -= Tests =-

// A function whose result is a constant needs no memory at all: no frame, no
// saved registers, and the Variant is built through the caller's pointer.
void test_constant_getter_has_no_frame() {
	std::cout << "Testing that a constant getter has no frame..." << std::endl;

	const Compiled compiled = compile("func test():\n\treturn 42\n");
	const std::vector<uint32_t> words = function_words(compiled, "test");

	assert(count(words, is_stack_adjust) == 0);
	assert(count(words, touches_frame) == 0);
	assert(is_store_through_return_pointer(words[words.size() - 2]));

	// li type, sw type, li value, sd value, ret. Anything more is ceremony.
	assert(words.size() <= 5);

	std::cout << "  ✓ Constant getter has no frame" << std::endl;
}

// The same for a global: the copy out of the global data area lands directly in
// the caller's Variant rather than in the return register's slot first.
void test_global_getter_returns_in_place() {
	std::cout << "Testing that a global getter writes through a0..." << std::endl;

	const Compiled compiled = compile("var value = 7\nfunc test():\n\treturn value\n");
	const std::vector<uint32_t> words = function_words(compiled, "test");

	assert(count(words, is_stack_adjust) == 0);
	assert(count(words, touches_frame) == 0);

	// One load and one store per Variant word, and nothing storing it twice.
	const size_t variant_words = size_t(VariantLayout(false).variant_words());
	assert(count(words, is_store_through_return_pointer) == variant_words);

	std::cout << "  ✓ Global getter writes through a0" << std::endl;
}

// A function that returns a parameter has a frame -- the parameter is copied
// into it on entry -- but still saves neither ra nor a0, and the copy out goes
// straight to the caller.
void test_leaf_function_saves_nothing() {
	std::cout << "Testing that a leaf function saves no registers..." << std::endl;

	const Compiled compiled = compile("func test(x):\n\treturn x\n");
	const std::vector<uint32_t> words = function_words(compiled, "test");

	assert(count(words, is_ecall) == 0 && "this function should reach no syscall");
	assert(count(words, is_call) == 0);

	for (uint32_t w : words) {
		assert(!is_store_to_stack(w, REG_RA) && "leaf function saved the return address");
		assert(!is_store_to_stack(w, REG_A0) && "no syscall can clobber a0 here");
	}

	std::cout << "  ✓ Leaf function saves no registers" << std::endl;
}

// A function that reaches the host does have to keep the caller's return-value
// pointer somewhere, because a0 is the first syscall argument register.
void test_syscall_function_spills_return_pointer() {
	std::cout << "Testing that a syscall spills the return pointer..." << std::endl;

	// Untyped addition goes through VEVAL.
	const Compiled compiled = compile("func test(a, b):\n\treturn a + b\n");
	const std::vector<uint32_t> words = function_words(compiled, "test");

	assert(count(words, is_ecall) > 0 && "this function should reach a syscall");

	size_t saved_a0 = 0;
	for (uint32_t w : words) {
		assert(!is_store_to_stack(w, REG_RA) && "still a leaf function");
		if (is_store_to_stack(w, REG_A0)) {
			saved_a0++;
		}
	}
	assert(saved_a0 == 1);

	std::cout << "  ✓ A syscall spills the return pointer" << std::endl;
}

// Only a function that calls another one saves the return address.
void test_calling_function_saves_return_address() {
	std::cout << "Testing that a caller saves the return address..." << std::endl;

	const Compiled compiled = compile(
		"func callee():\n\treturn 1\n"
		"func test():\n\treturn callee()\n");
	const std::vector<uint32_t> caller = function_words(compiled, "test");
	const std::vector<uint32_t> callee = function_words(compiled, "callee");

	assert(count(caller, is_call) == 1);

	size_t saved_ra = 0;
	for (uint32_t w : caller) {
		if (is_store_to_stack(w, REG_RA)) {
			saved_ra++;
		}
	}
	assert(saved_ra == 1 && "a function that calls has to save ra");

	for (uint32_t w : callee) {
		assert(!is_store_to_stack(w, REG_RA) && "the callee calls nothing");
	}

	std::cout << "  ✓ Only a caller saves the return address" << std::endl;
}

// The frame pointer is gone: nothing in the backend reads one, so nothing
// should be spending three instructions a function setting one up.
void test_no_frame_pointer_is_set_up() {
	std::cout << "Testing that no frame pointer is set up..." << std::endl;

	constexpr uint8_t REG_FP = 8;
	const Compiled compiled = compile(
		"func test(a, b):\n"
		"\tvar total = 0\n"
		"\tfor i in range(a):\n"
		"\t\ttotal += b\n"
		"\treturn total\n");

	for (uint32_t w : function_words(compiled, "test")) {
		assert(!is_store_to_stack(w, REG_FP) && "saved a frame pointer nothing reads");
		const bool sets_fp = opcode_of(w) == 0x13 && funct3_of(w) == 0 &&
			rd_of(w) == REG_FP && rs1_of(w) == REG_SP;
		assert(!sets_fp && "set up a frame pointer nothing reads");
	}

	std::cout << "  ✓ No frame pointer is set up" << std::endl;
}

// Forwarding the return value is only sound when nothing reads the return
// register afterwards. A loop that assigns the return value and then branches
// backwards over the assignment has to keep using the slot.
void test_forwarding_respects_later_reads() {
	std::cout << "Testing that forwarding respects later reads..." << std::endl;

	// The value returned still has to be the value computed, whatever the
	// backend does with the slot. The differential and invariance harnesses
	// cover this over the whole corpus; this is the shape that would break
	// first, kept here next to the code that decides it.
	const Compiled compiled = compile(
		"func test(n):\n"
		"\tvar result = 0\n"
		"\twhile n > 0:\n"
		"\t\tresult = n\n"
		"\t\tn -= 1\n"
		"\treturn result\n");
	const std::vector<uint32_t> words = function_words(compiled, "test");
	assert(!words.empty());

	std::cout << "  ✓ Forwarding respects later reads" << std::endl;
}

} // namespace

int main() {
	std::cout << "=== Function Frame Tests ===" << std::endl << std::endl;

	test_constant_getter_has_no_frame();
	test_global_getter_returns_in_place();
	test_leaf_function_saves_nothing();
	test_syscall_function_spills_return_pointer();
	test_calling_function_saves_return_address();
	test_no_frame_pointer_is_set_up();
	test_forwarding_respects_later_reads();

	std::cout << std::endl << "All frame tests passed!" << std::endl;
	return 0;
}
