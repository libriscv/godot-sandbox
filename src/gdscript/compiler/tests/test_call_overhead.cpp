// Pin down the instruction sequences around a call: return, immediate folding,
// int chaining, non-negative subscript elision, global handle elision.
// Assertions are on emitted instruction shapes, not specific encodings.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../riscv_codegen.h"
#include "../ir_optimizer.h"
#include "../syscall_numbers.h"
#include "../compiler_exception.h"
#include "../variant_layout.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace gdscript;

namespace {

constexpr uint8_t REG_ZERO = 0;
constexpr uint8_t REG_SP = 2;
constexpr uint8_t REG_A0 = 10;
constexpr uint8_t REG_A7 = 17;

constexpr uint32_t RET = 0x00008067;

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

uint32_t word_at(const Compiled& compiled, size_t offset) {
	return uint32_t(compiled.code[offset]) |
		(uint32_t(compiled.code[offset + 1]) << 8) |
		(uint32_t(compiled.code[offset + 2]) << 16) |
		(uint32_t(compiled.code[offset + 3]) << 24);
}

// The words of one function, up to and including its `returns`th RET.
std::vector<uint32_t> function_words(const Compiled& compiled, const std::string& name,
	size_t returns = 1) {
	auto it = compiled.offsets.find(name);
	assert(it != compiled.offsets.end() && "no such function in the generated code");

	std::vector<uint32_t> words;
	size_t seen = 0;
	for (size_t off = it->second; off + 4 <= compiled.code.size(); off += 4) {
		const uint32_t word = word_at(compiled, off);
		words.push_back(word);
		if (word == RET && ++seen == returns) {
			return words;
		}
	}
	assert(false && "function does not return that many times");
	return words;
}

uint32_t opcode_of(uint32_t w) { return w & 0x7F; }
uint8_t rd_of(uint32_t w)      { return uint8_t((w >> 7) & 0x1F); }
uint8_t funct3_of(uint32_t w)  { return uint8_t((w >> 12) & 0x7); }
uint8_t rs1_of(uint32_t w)     { return uint8_t((w >> 15) & 0x1F); }
int32_t i_imm_of(uint32_t w)   { return int32_t(w) >> 20; }
// A shift's immediate field carries funct7 above the amount.
uint8_t shamt_of(uint32_t w)   { return uint8_t((w >> 20) & 0x3F); }

// An OP-IMM instruction (addi/andi/ori/xori/slli/srai) with this funct3.
bool is_op_imm(uint32_t w, uint8_t funct3) {
	return opcode_of(w) == 0x13 && funct3_of(w) == funct3;
}

bool is_andi(uint32_t w) { return is_op_imm(w, 7); }
bool is_ori(uint32_t w)  { return is_op_imm(w, 6); }
bool is_xori(uint32_t w) { return is_op_imm(w, 4); }
bool is_srai(uint32_t w) { return is_op_imm(w, 5) && (w >> 30) == 1; }
bool is_slli(uint32_t w) { return is_op_imm(w, 1); }

bool is_stack_adjust(uint32_t w) {
	return is_op_imm(w, 0) && rd_of(w) == REG_SP && rs1_of(w) == REG_SP;
}

// addi that is not the frame moving: an arithmetic immediate or an address.
bool is_addi(uint32_t w) {
	return is_op_imm(w, 0) && !is_stack_adjust(w);
}

bool is_store_to_frame(uint32_t w) {
	return opcode_of(w) == 0x23 && rs1_of(w) == REG_SP;
}

bool touches_frame(uint32_t w) {
	const bool is_store = opcode_of(w) == 0x23;
	const bool is_load = opcode_of(w) == 0x03;
	return (is_store || is_load) && rs1_of(w) == REG_SP;
}

bool is_store_through_return_pointer(uint32_t w) {
	return opcode_of(w) == 0x23 && rs1_of(w) == REG_A0;
}

bool is_ecall(uint32_t w) {
	return opcode_of(w) == 0x73 && funct3_of(w) == 0 && rd_of(w) == REG_ZERO && (w >> 20) == 0;
}

// `li a7, number` -- how a syscall says which one it is.
bool selects_syscall(uint32_t w, int number) {
	return is_op_imm(w, 0) && rd_of(w) == REG_A7 && rs1_of(w) == REG_ZERO &&
		i_imm_of(w) == number;
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

size_t count_syscall(const std::vector<uint32_t>& words, int number) {
	size_t n = 0;
	for (uint32_t w : words) {
		if (selects_syscall(w, number)) {
			n++;
		}
	}
	return n;
}

// Is there a shift by this amount?
bool has_shift(const std::vector<uint32_t>& words, bool (*form)(uint32_t), uint8_t shamt) {
	for (uint32_t w : words) {
		if (form(w) && shamt_of(w) == shamt) {
			return true;
		}
	}
	return false;
}

// Is there an immediate-form instruction carrying this value?
bool has_immediate(const std::vector<uint32_t>& words, bool (*form)(uint32_t), int32_t value) {
	for (uint32_t w : words) {
		if (form(w) && i_imm_of(w) == value) {
			return true;
		}
	}
	return false;
}

// -= Leaving =-
void test_falling_off_the_end_returns_null() {
	std::cout << "Testing that falling off the end returns null..." << std::endl;

	const Compiled compiled = compile("func f(a, b):\n\tvar c = a\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	// Type word through a0, no frame, no load.
	assert(count(words, is_stack_adjust) == 0);
	assert(count(words, touches_frame) == 0);
	assert(count(words, is_store_through_return_pointer) == 1);
	assert(words.size() <= 3);

	std::cout << "  ✓ Falling off the end is null, and free" << std::endl;
}

void test_bare_return_returns_null() {
	std::cout << "Testing that a valueless return is null..." << std::endl;

	const Compiled compiled = compile("func f(a):\n\tif a > 0:\n\t\treturn\n\treturn 2\n");
	const std::vector<uint32_t> words = function_words(compiled, "f", 2);

	// One sw for null, two for the integer -- no slot copy.
	assert(count(words, is_store_through_return_pointer) == 3);

	std::cout << "  ✓ A valueless return is null" << std::endl;
}

// Later reads of r0 are on unreachable paths; forwarding is still valid.
void test_early_return_still_forwards() {
	std::cout << "Testing that an early return writes through a0..." << std::endl;

	const Compiled compiled = compile(
		"func f(a):\n"
		"\tif a > 10:\n"
		"\t\treturn 1\n"
		"\tif a < 0:\n"
		"\t\treturn 2\n"
		"\treturn 3\n");
	const std::vector<uint32_t> words = function_words(compiled, "f", 3);

	// Type + payload per return, not a whole-Variant copy.
	const size_t variant_words = size_t(VariantLayout(false).variant_words());
	assert(count(words, is_store_through_return_pointer) == 3 * 2);
	assert(3 * 2 < 3 * variant_words);

	std::cout << "  ✓ An early return writes through a0" << std::endl;
}

// -= Decoding =-
void test_constant_operand_becomes_an_immediate() {
	std::cout << "Testing that a constant operand folds into the instruction..." << std::endl;

	const Compiled compiled = compile(
		"func f(word : int):\n"
		"\treturn (word & 255) + (word | 16) + (word ^ 3) + (word << 2) \\\n"
		"\t\t+ (word >> 4) + (word + 7) + (word - 9)\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	assert(has_immediate(words, is_andi, 255));
	assert(has_immediate(words, is_ori, 16));
	assert(has_immediate(words, is_xori, 3));
	assert(has_shift(words, is_slli, 2));
	assert(has_shift(words, is_srai, 4));
	assert(has_immediate(words, is_addi, 7));
	assert(has_immediate(words, is_addi, -9)); // SUB borrows addi

	std::cout << "  ✓ Constant operands are immediates" << std::endl;
}

// Past the 12-bit range, andi sign-extends wrongly; register form required.
void test_a_constant_too_wide_stays_in_a_register() {
	std::cout << "Testing that a constant past the immediate range is loaded..." << std::endl;

	const Compiled compiled = compile("func f(word : int):\n\treturn word & 4095\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	assert(!has_immediate(words, is_andi, 4095));
	// Falls back to a Variant in the frame.
	assert(count(words, touches_frame) > 0);

	std::cout << "  ✓ A wide constant is not folded" << std::endl;
}

void test_chained_operators_stay_in_a_register() {
	std::cout << "Testing that a decode chain stays in a register..." << std::endl;

	const Compiled compiled = compile("func f(word : int):\n\treturn (word >> 8) & 255\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	assert(has_shift(words, is_srai, 8));
	assert(has_immediate(words, is_andi, 255));

	// srai then andi, adjacent: no intermediate Variant.
	bool adjacent = false;
	for (size_t i = 0; i + 1 < words.size(); i++) {
		if (is_srai(words[i]) && is_andi(words[i + 1])) {
			adjacent = true;
		}
	}
	assert(adjacent);

	std::cout << "  ✓ A decode chain stays in a register" << std::endl;
}

// -= Subscripting =-
void test_a_masked_index_skips_the_wrap() {
	std::cout << "Testing that a bounded index skips the wrap..." << std::endl;

	const Compiled bounded = compile(
		"func f(regs : Array, word : int):\n"
		"\treturn regs[(word >> 8) & 7]\n");
	const std::vector<uint32_t> bounded_words = function_words(bounded, "f");

	assert(count_syscall(bounded_words, ECALL_ARRAY_AT) == 1);
	assert(count_syscall(bounded_words, ECALL_ARRAY_SIZE) == 0);

	// Unbounded index still wraps.
	const Compiled unbounded = compile(
		"func f(regs : Array, index : int):\n"
		"\treturn regs[index]\n");
	const std::vector<uint32_t> unbounded_words = function_words(unbounded, "f");

	assert(count_syscall(unbounded_words, ECALL_ARRAY_AT) == 1);
	assert(count_syscall(unbounded_words, ECALL_ARRAY_SIZE) == 1);

	std::cout << "  ✓ A bounded index costs one syscall, not two" << std::endl;
}

void test_a_constant_index_skips_the_wrap() {
	std::cout << "Testing that a constant index skips the wrap..." << std::endl;

	const Compiled compiled = compile("func f(regs : Array):\n\treturn regs[2]\n");
	assert(count_syscall(function_words(compiled, "f"), ECALL_ARRAY_SIZE) == 0);

	// Negative constant must still wrap.
	const Compiled from_end = compile("func f(regs : Array):\n\treturn regs[-1]\n");
	assert(count_syscall(function_words(from_end, "f"), ECALL_ARRAY_SIZE) == 1);

	std::cout << "  ✓ A constant index wraps only where it must" << std::endl;
}

// -= Global handle elision =-
void test_a_global_container_is_read_where_it_lies() {
	std::cout << "Testing that a global container is read in place..." << std::endl;

	const Compiled compiled = compile(
		"var regs : Array = []\n"
		"var pc : int = 0\n"
		"func f() -> void:\n"
		"\tregs[pc] = 1\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	// Frame holds only the return pointer and the assigned Variant (type + payload).
	const size_t variant_words = size_t(VariantLayout(false).variant_words());
	assert(count(words, is_store_to_frame) == 1 + 2);
	assert(1 + 2 < 1 + 2 + variant_words);
	assert(count_syscall(words, ECALL_ARRAY_AT) == 1);

	std::cout << "  ✓ A global container needs no copy" << std::endl;
}

// A store between the load and the use invalidates the elision.
void test_a_reassigned_global_keeps_its_copy() {
	std::cout << "Testing that an assigned global is still copied..." << std::endl;

	const Compiled compiled = compile(
		"var regs : Array = []\n"
		"var pc : int = 0\n"
		"func f(other : Array) -> void:\n"
		"\tvar first = regs\n"
		"\tregs = other\n"
		"\tfirst[pc] = 1\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	// Intervening assignment forces the frame copy.
	const size_t variant_words = size_t(VariantLayout(false).variant_words());
	assert(count(words, is_store_to_frame) >= 1 + 2 + variant_words);

	std::cout << "  ✓ An assigned global is copied before it changes" << std::endl;
}

// Read-modify-write of an int global stays in registers; no Variant built.
void test_an_int_global_round_trips_in_a_register() {
	std::cout << "Testing that an int global round-trips in a register..." << std::endl;

	const Compiled compiled = compile(
		"var pc : int = 0\n"
		"func f(n : int) -> void:\n"
		"\tpc = pc + n\n");
	const std::vector<uint32_t> words = function_words(compiled, "f");

	// Frame: the return pointer, the parameter's own Variant, and the int
	// Variant its declared type coerces that into -- a tag word and a payload.
	// The global is not among them: it is loaded from and stored to the
	// globals area, in registers, with no Variant built for it at all.
	const size_t variant_words = size_t(VariantLayout(false).variant_words());
	const size_t coerced_parameter = 2;
	// A global copied into the frame would show up as another variant_words
	// worth of stores on top of these.
	assert(count(words, is_store_to_frame) == 1 + variant_words + coerced_parameter);
	assert(count(words, is_ecall) == 0);

	std::cout << "  ✓ An int global round-trips in a register" << std::endl;
}

} // namespace

int main() {
	std::cout << "=== Call Overhead Tests ===" << std::endl << std::endl;

	test_falling_off_the_end_returns_null();
	test_bare_return_returns_null();
	test_early_return_still_forwards();

	test_constant_operand_becomes_an_immediate();
	test_a_constant_too_wide_stays_in_a_register();
	test_chained_operators_stay_in_a_register();

	test_a_masked_index_skips_the_wrap();
	test_a_constant_index_skips_the_wrap();

	test_a_global_container_is_read_where_it_lies();
	test_a_reassigned_global_keeps_its_copy();
	test_an_int_global_round_trips_in_a_register();

	std::cout << std::endl << "All call overhead tests passed!" << std::endl;
	return 0;
}
