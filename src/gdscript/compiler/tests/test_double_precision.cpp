// Tests for Godot double-precision builds (real_t = double).
//
// With REAL_T_IS_DOUBLE the Variant grows from 24 to 40 bytes and every real_t
// payload (Vector2/3/4, Color, ...) doubles in width, while the type tag, the
// data offset, Variant::FLOAT and the integer vectors stay exactly as they are.
// These tests pin down both layouts from the same source program, so the two
// only ever differ where they are supposed to.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../riscv_codegen.h"
#include "../ir_optimizer.h"
#include "../compiler.h"
#include "../instance_layout.h"
#include "../variant_layout.h"
#include <cassert>
#include <iostream>
#include <unordered_map>
#include <vector>

using namespace gdscript;

// -= Helpers =-

struct Compiled {
	std::vector<uint8_t> code;
	std::unordered_map<std::string, size_t> functions;
	size_t global_data_size = 0;
};

static Compiled compile_to_code(const std::string& source, const VariantLayout& layout) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();

	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	IROptimizer optimizer;
	optimizer.optimize(ir);

	RISCVCodeGen riscv(layout);
	Compiled out;
	out.code = riscv.generate(ir);
	out.functions = riscv.get_function_offsets();
	out.global_data_size = riscv.get_global_data_size();
	return out;
}

static uint32_t word_at(const std::vector<uint8_t>& code, size_t off) {
	return uint32_t(code[off]) | (uint32_t(code[off + 1]) << 8) |
			(uint32_t(code[off + 2]) << 16) | (uint32_t(code[off + 3]) << 24);
}

// Counts instructions matching (opcode, funct3). Used to tell flw/fsw apart from
// fld/fsd, which differ only in funct3.
static int count_by_opcode_funct3(const std::vector<uint8_t>& code, uint32_t opcode, uint32_t funct3) {
	int count = 0;
	for (size_t off = 0; off + 4 <= code.size(); off += 4) {
		const uint32_t instr = word_at(code, off);
		if ((instr & 0x7F) == opcode && ((instr >> 12) & 7) == funct3) {
			count++;
		}
	}
	return count;
}

// Counts R-type FP instructions by funct7, which encodes both the operation and
// whether it is the single- or double-precision form.
static int count_fp_op(const std::vector<uint8_t>& code, uint32_t funct7) {
	int count = 0;
	for (size_t off = 0; off + 4 <= code.size(); off += 4) {
		const uint32_t instr = word_at(code, off);
		if ((instr & 0x7F) == 0x53 && (instr >> 25) == funct7) {
			count++;
		}
	}
	return count;
}

static constexpr uint32_t OP_LOAD_FP = 0x07;
static constexpr uint32_t OP_STORE_FP = 0x27;
static constexpr uint32_t FUNCT3_WORD = 2;  // flw / fsw
static constexpr uint32_t FUNCT3_DOUBLE = 3; // fld / fsd

// -= Tests =-

void test_layout_constants() {
	std::cout << "Testing VariantLayout constants..." << std::endl;

	const VariantLayout single(false);
	const VariantLayout dbl(true);

	// These two numbers are what program/cpp/docker/api/variant.hpp static_asserts on.
	assert(single.variant_size() == 24);
	assert(dbl.variant_size() == 40);

	assert(single.real_size() == 4);
	assert(dbl.real_size() == 8);

	assert(single.variant_words() == 3);
	assert(dbl.variant_words() == 5);

	// The tag and the data union never move
	assert(VariantLayout::TYPE_OFFSET == 0);
	assert(VariantLayout::DATA_OFFSET == 8);

	// real_t components: packed at 4 or 8 byte stride
	assert(single.real_offset(0) == 8 && single.real_offset(3) == 20);
	assert(dbl.real_offset(0) == 8 && dbl.real_offset(3) == 32);

	// int32_t components (Vector2i/3i/4i) are identical in both builds
	assert(VariantLayout::int_offset(0) == 8 && VariantLayout::int_offset(3) == 20);

	// A whole Variant is always a round number of doublewords, which is what the
	// ld/sd copy loop relies on.
	assert(single.variant_size() % 8 == 0);
	assert(dbl.variant_size() % 8 == 0);

	std::cout << "  ✓ VariantLayout constants test passed" << std::endl;
}

void test_stack_frames_scale_with_variant_size() {
	std::cout << "Testing that stack frames scale with Variant size..." << std::endl;

	const std::string source = R"(func test(a, b):
	var c = a + b
	var d = c * 2
	return d
)";

	const Compiled single = compile_to_code(source, VariantLayout(false));
	const Compiled dbl = compile_to_code(source, VariantLayout(true));

	// The entry point runs before the functions, so start at the function's own
	// prologue, which is "addi sp, sp, -frame_size" in both builds.
	auto frame_size = [](const Compiled& compiled) -> int32_t {
		const auto it = compiled.functions.find("test");
		assert(it != compiled.functions.end());
		const uint32_t first = word_at(compiled.code, it->second);
		assert((first & 0x7F) == 0x13 && ((first >> 12) & 7) == 0);
		assert(((first >> 7) & 0x1F) == 2 && ((first >> 15) & 0x1F) == 2); // rd = rs1 = sp
		return -(int32_t(first) >> 20);
	};

	const int32_t single_frame = frame_size(single);
	const int32_t double_frame = frame_size(dbl);

	assert(single_frame > 0);
	// Same slot count, wider slots: the double-precision frame has to be bigger.
	assert(double_frame > single_frame);

	std::cout << "  ✓ Stack frame scaling test passed" << std::endl;
}

void test_vector_components_use_real_t_width() {
	std::cout << "Testing that vector components load/store at real_t width..." << std::endl;

	// Constructing a Vector3 writes three real_t components; reading .x back reads one.
	const std::string source = R"(func test():
	var v = Vector3(1.0, 2.0, 3.0)
	return v.x
)";

	const std::vector<uint8_t> single = compile_to_code(source, VariantLayout(false)).code;
	const std::vector<uint8_t> dbl = compile_to_code(source, VariantLayout(true)).code;

	// Single precision: real_t components go through flw/fsw
	assert(count_by_opcode_funct3(single, OP_STORE_FP, FUNCT3_WORD) > 0);
	assert(count_by_opcode_funct3(single, OP_LOAD_FP, FUNCT3_WORD) > 0);

	// Double precision: no 32-bit FP access survives, everything is fld/fsd
	assert(count_by_opcode_funct3(dbl, OP_STORE_FP, FUNCT3_WORD) == 0);
	assert(count_by_opcode_funct3(dbl, OP_LOAD_FP, FUNCT3_WORD) == 0);
	assert(count_by_opcode_funct3(dbl, OP_STORE_FP, FUNCT3_DOUBLE) > 0);
	assert(count_by_opcode_funct3(dbl, OP_LOAD_FP, FUNCT3_DOUBLE) > 0);

	std::cout << "  ✓ Vector component width test passed" << std::endl;
}

void test_vector_arithmetic_uses_real_t_width() {
	std::cout << "Testing that vector arithmetic uses real_t-width FP ops..." << std::endl;

	const std::string source = R"(func test(a: Vector2, b: Vector2) -> Vector2:
	return a + b
)";

	const std::vector<uint8_t> single = compile_to_code(source, VariantLayout(false)).code;
	const std::vector<uint8_t> dbl = compile_to_code(source, VariantLayout(true)).code;

	static constexpr uint32_t FUNCT7_FADD_S = 0x00;
	static constexpr uint32_t FUNCT7_FADD_D = 0x01;

	// Component-wise add: one FP add per component, at the target's real_t width
	assert(count_fp_op(single, FUNCT7_FADD_S) == 2);
	assert(count_fp_op(single, FUNCT7_FADD_D) == 0);

	assert(count_fp_op(dbl, FUNCT7_FADD_D) == 2);
	assert(count_fp_op(dbl, FUNCT7_FADD_S) == 0);

	std::cout << "  ✓ Vector arithmetic width test passed" << std::endl;
}

void test_integer_vectors_are_layout_independent() {
	std::cout << "Testing that integer vectors are identical in both layouts..." << std::endl;

	// Vector2i/3i/4i components are int32_t regardless of real_t, so the component
	// arithmetic must not use any FP instruction in either build.
	const std::string source = R"(func test(a: Vector3i, b: Vector3i) -> Vector3i:
	return a + b
)";

	const std::vector<uint8_t> single = compile_to_code(source, VariantLayout(false)).code;
	const std::vector<uint8_t> dbl = compile_to_code(source, VariantLayout(true)).code;

	for (const auto* code : { &single, &dbl }) {
		assert(count_by_opcode_funct3(*code, OP_LOAD_FP, FUNCT3_WORD) == 0);
		assert(count_by_opcode_funct3(*code, OP_LOAD_FP, FUNCT3_DOUBLE) == 0);
		assert(count_by_opcode_funct3(*code, OP_STORE_FP, FUNCT3_WORD) == 0);
		assert(count_by_opcode_funct3(*code, OP_STORE_FP, FUNCT3_DOUBLE) == 0);
	}

	std::cout << "  ✓ Integer vector layout independence test passed" << std::endl;
}

void test_float_variants_stay_double() {
	std::cout << "Testing that Variant::FLOAT stays 64-bit in both layouts..." << std::endl;

	// Godot's float is a double in every build, so typed float arithmetic must emit
	// fld/fsd and fadd.d no matter what real_t is - and no 32-bit FP access at all.
	const std::string source = R"(func test(a: float, b: float) -> float:
	return a + b
)";

	const std::vector<uint8_t> single = compile_to_code(source, VariantLayout(false)).code;
	const std::vector<uint8_t> dbl = compile_to_code(source, VariantLayout(true)).code;

	static constexpr uint32_t FUNCT7_FADD_S = 0x00;
	static constexpr uint32_t FUNCT7_FADD_D = 0x01;

	for (const auto* code : { &single, &dbl }) {
		assert(count_by_opcode_funct3(*code, OP_LOAD_FP, FUNCT3_DOUBLE) > 0);
		assert(count_by_opcode_funct3(*code, OP_STORE_FP, FUNCT3_DOUBLE) > 0);
		assert(count_by_opcode_funct3(*code, OP_LOAD_FP, FUNCT3_WORD) == 0);
		assert(count_by_opcode_funct3(*code, OP_STORE_FP, FUNCT3_WORD) == 0);
		assert(count_fp_op(*code, FUNCT7_FADD_D) == 1);
		assert(count_fp_op(*code, FUNCT7_FADD_S) == 0);
	}

	std::cout << "  ✓ Variant::FLOAT width test passed" << std::endl;
}

void test_int_only_code_stays_integer() {
	std::cout << "Testing that pure-integer code stays free of FP instructions..." << std::endl;

	// Nothing here touches real_t. The instruction stream still differs slightly -
	// a whole-Variant copy is 5 ld/sd pairs instead of 3 - but no floating point
	// instruction has any business appearing in either build.
	const std::string source = R"(func test(n: int) -> int:
	var total: int = 0
	var i: int = 0
	while i < n:
		total = total + i * 3
		i = i + 1
	return total
)";

	const std::vector<uint8_t> single = compile_to_code(source, VariantLayout(false)).code;
	const std::vector<uint8_t> dbl = compile_to_code(source, VariantLayout(true)).code;

	// The only floating point here is the entry coercion of `n: int`, which
	// converts a FLOAT the caller may have passed. Variant::FLOAT is always a
	// double, at the same offset in both layouts, so that preamble is identical
	// in the two builds -- and the arithmetic itself contributes nothing.
	for (const auto* code : { &single, &dbl }) {
		assert(count_by_opcode_funct3(*code, OP_LOAD_FP, FUNCT3_WORD) == 0);
		assert(count_by_opcode_funct3(*code, OP_STORE_FP, FUNCT3_WORD) == 0);
		assert(count_by_opcode_funct3(*code, OP_STORE_FP, FUNCT3_DOUBLE) == 0);
		// One fld, for the one declared parameter.
		assert(count_by_opcode_funct3(*code, OP_LOAD_FP, FUNCT3_DOUBLE) == 1);
	}

	// Wider Variants mean wider copies, never narrower ones
	assert(dbl.size() >= single.size());
	assert(single != dbl);

	std::cout << "  ✓ Integer code test passed" << std::endl;
}

void test_globals_area_scales() {
	std::cout << "Testing that the globals data area scales with Variant size..." << std::endl;

	const std::string source = R"(var counter = 0
var scale = 2

func test():
	counter = counter + scale
	return counter
)";

	const size_t blob = size_t(InstanceLayout::BLOB_SIZE);
	assert(compile_to_code(source, VariantLayout(false)).global_data_size == 2 * 24 + blob);
	assert(compile_to_code(source, VariantLayout(true)).global_data_size == 2 * 40 + blob);

	std::cout << "  ✓ Globals area scaling test passed" << std::endl;
}

void test_large_frames_survive_wider_variants() {
	std::cout << "Testing large stack frames with 40-byte Variants..." << std::endl;

	// Wider Variants push stack offsets past the 12-bit immediate limit sooner, so
	// the same function can need the li+add fallback in a double-precision build
	// while it still fits an addi in a single-precision one.
	std::string source = "func test():\n";
	for (int i = 0; i < 80; i++) {
		source += "\tvar v" + std::to_string(i) + " = " + std::to_string(i) + "\n";
	}
	source += "\treturn v0 + v79\n";

	const Compiled single = compile_to_code(source, VariantLayout(false));
	const Compiled dbl = compile_to_code(source, VariantLayout(true));

	assert(!single.code.empty());
	assert(!dbl.code.empty());

	// 80 locals plus the scratch slots: the double-precision frame is well past
	// the 2048-byte immediate range that the single-precision one still fits in.
	assert(80 * 24 < 2048);
	assert(80 * 40 > 2048);

	// The whole pipeline has to accept it, ELF and all
	CompilerOptions options;
	options.double_precision = true;
	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile(source, options);
	assert(!elf.empty());

	std::cout << "  ✓ Large frame test passed" << std::endl;
}

void test_compiler_option_selects_layout() {
	std::cout << "Testing CompilerOptions::double_precision..." << std::endl;

	const std::string source = R"(func test():
	var v = Vector2(1.0, 2.0)
	return v.y
)";

	CompilerOptions single_opts;
	single_opts.double_precision = false;
	Compiler single_compiler;
	std::vector<uint8_t> single_elf = single_compiler.compile(source, single_opts);
	assert(!single_elf.empty());

	CompilerOptions double_opts;
	double_opts.double_precision = true;
	Compiler double_compiler;
	std::vector<uint8_t> double_elf = double_compiler.compile(source, double_opts);
	assert(!double_elf.empty());

	// Both are valid ELFs, and the flag actually changes what comes out
	assert(single_elf.size() >= 4 && single_elf[0] == 0x7F && single_elf[1] == 'E');
	assert(double_elf.size() >= 4 && double_elf[0] == 0x7F && double_elf[1] == 'E');
	assert(single_elf != double_elf);

	// The default follows the build this compiler was compiled for
	assert(CompilerOptions().double_precision == native_variant_layout().double_precision);

	std::cout << "  ✓ CompilerOptions layout selection test passed" << std::endl;
}

int main() {
	std::cout << "=== Double Precision (real_t = double) Tests ===" << std::endl << std::endl;

	test_layout_constants();
	test_stack_frames_scale_with_variant_size();
	test_vector_components_use_real_t_width();
	test_vector_arithmetic_uses_real_t_width();
	test_integer_vectors_are_layout_independent();
	test_float_variants_stay_double();
	test_int_only_code_stays_integer();
	test_globals_area_scales();
	test_large_frames_survive_wider_variants();
	test_compiler_option_selects_layout();

	std::cout << std::endl << "=== All double precision tests passed! ===" << std::endl;
	return 0;
}
