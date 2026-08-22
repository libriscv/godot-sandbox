// Instruction encoding limits.
//
// `emit_addi(rd, rs, 4776)` used to silently produce `addi rd, rs, 680`: the
// immediate is 12 bits signed and the encoder just masked it. Three call sites
// computing global addresses that way produced wrong code once a program had
// enough globals -- with no diagnostic, no failing test, and nothing to look at
// but a program that read the wrong variable.
//
// These tests pin down the two halves of the fix: the range checks live inside
// the encoders, and the address of a global is computed in one place that folds
// the index into the relocation instead of adding it afterwards.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

namespace {

std::vector<uint8_t> compile_to_code(const std::string& source, RISCVCodeGen& codegen, bool optimize = true) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator gen;
	IRProgram ir = gen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
	}
	return codegen.generate(ir);
}

uint32_t word_at(const std::vector<uint8_t>& code, size_t offset) {
	uint32_t word = 0;
	std::memcpy(&word, code.data() + offset, 4);
	return word;
}

void test_range_boundaries() {
	std::cout << "Testing immediate range boundaries..." << std::endl;

	// 12 bits signed is -2048 .. 2047. The old inline checks tested `< 2048`
	// and forgot `>= -2048`, so the negative boundary is the interesting one.
	assert(RISCVCodeGen::fits_in_signed(2047, 12));
	assert(!RISCVCodeGen::fits_in_signed(2048, 12));
	assert(RISCVCodeGen::fits_in_signed(-2048, 12));
	assert(!RISCVCodeGen::fits_in_signed(-2049, 12));
	assert(RISCVCodeGen::fits_in_signed(0, 12));

	// 13 bits signed for a branch, 21 for a jump.
	assert(RISCVCodeGen::fits_in_signed(4094, 13));
	assert(!RISCVCodeGen::fits_in_signed(4096, 13));
	assert(RISCVCodeGen::fits_in_signed(1048574, 21));
	assert(!RISCVCodeGen::fits_in_signed(1048576, 21));

	std::cout << "  Boundaries OK" << std::endl;
}

void expect_rejected(const std::string& what, int64_t value, int bits, bool displacement) {
	try {
		if (displacement) {
			RISCVCodeGen::check_displacement(what, value, bits);
		} else {
			RISCVCodeGen::check_immediate(what, value, bits);
		}
	} catch (const CompilerException& e) {
		const std::string message = e.what();
		// The diagnostic has to name the value, otherwise it does not help
		// anyone find the instruction that produced it.
		assert(message.find(std::to_string(value)) != std::string::npos);
		return;
	}
	std::cerr << "Accepted an out-of-range immediate: " << value << " in " << bits << " bits" << std::endl;
	assert(false && "out-of-range immediate accepted");
}

void test_out_of_range_is_an_error() {
	std::cout << "Testing that an out-of-range immediate is an error..." << std::endl;

	// The exact value from the bug: 4776 masked to 680.
	expect_rejected("I-type", 4776, RISCVCodeGen::I_TYPE_IMM_BITS, false);
	expect_rejected("I-type", -2049, RISCVCodeGen::I_TYPE_IMM_BITS, false);
	expect_rejected("S-type (store)", 100000, RISCVCodeGen::S_TYPE_IMM_BITS, false);
	expect_rejected("B-type (branch)", 8192, RISCVCodeGen::B_TYPE_IMM_BITS, true);
	expect_rejected("J-type (jump)", 4194304, RISCVCodeGen::J_TYPE_IMM_BITS, true);

	// A branch to an odd address cannot be encoded at all.
	expect_rejected("B-type (branch)", 3, RISCVCodeGen::B_TYPE_IMM_BITS, true);

	// In range is accepted, and says nothing.
	RISCVCodeGen::check_immediate("I-type", 2047, RISCVCodeGen::I_TYPE_IMM_BITS);
	RISCVCodeGen::check_immediate("I-type", -2048, RISCVCodeGen::I_TYPE_IMM_BITS);
	RISCVCodeGen::check_displacement("B-type (branch)", -4096, RISCVCodeGen::B_TYPE_IMM_BITS);

	std::cout << "  Out-of-range is an error OK" << std::endl;
}

// Build a program with `count` globals whose `test()` reads the first and the
// last one. The addresses of those two globals have to be exactly
// (count - 1) * variant_size() apart, however many globals there are.
std::string program_with_globals(int count) {
	std::string source;
	for (int i = 0; i < count; i++) {
		source += "var g" + std::to_string(i) + " = " + std::to_string(i) + "\n";
	}
	source += "\nfunc test():\n\treturn g0 + g" + std::to_string(count - 1) + "\n";
	return source;
}

// Decode an AUIPC + ADDI pair at `offset` into the address it computes,
// relative to the address of the AUIPC.
bool decode_address_pair(const std::vector<uint8_t>& code, size_t offset, int64_t& relative_address) {
	if (offset + 8 > code.size()) {
		return false;
	}
	const uint32_t auipc = word_at(code, offset);
	const uint32_t addi = word_at(code, offset + 4);
	if ((auipc & 0x7F) != 0x17 || (addi & 0x7F) != 0x13) {
		return false;
	}
	const uint8_t auipc_rd = (auipc >> 7) & 0x1F;
	const uint8_t addi_rd = (addi >> 7) & 0x1F;
	const uint8_t addi_rs1 = (addi >> 15) & 0x1F;
	const uint8_t addi_funct3 = (addi >> 12) & 0x7;
	if (auipc_rd != addi_rd || addi_rs1 != auipc_rd || addi_funct3 != 0) {
		return false;
	}

	// AUIPC adds a sign-extended 20-bit upper immediate to the pc; ADDI then
	// adds a sign-extended 12-bit immediate.
	const int64_t upper = static_cast<int32_t>(auipc & 0xFFFFF000);
	const int64_t lower = (static_cast<int32_t>(addi) >> 20);
	relative_address = upper + lower;
	return true;
}

void test_global_addressing_does_not_truncate() {
	std::cout << "Testing global addressing past the 12-bit immediate..." << std::endl;

	// 85 globals is where a 24-byte Variant stride runs out of 12-bit signed
	// immediate, which is where the original bug started producing wrong code.
	for (int count : { 4, 85, 86, 200 }) {
		RISCVCodeGen codegen;
		const std::vector<uint8_t> code = compile_to_code(program_with_globals(count), codegen);
		assert(!code.empty());

		const auto& functions = codegen.get_function_offsets();
		auto it = functions.find("test");
		assert(it != functions.end());

		// Find the first two global addresses computed in test(). LOAD_GLOBAL
		// emits an AUIPC + ADDI pair for each.
		std::vector<int64_t> addresses;
		for (size_t offset = it->second; offset + 8 <= code.size() && addresses.size() < 2; offset += 4) {
			int64_t relative = 0;
			if (decode_address_pair(code, offset, relative)) {
				addresses.push_back(static_cast<int64_t>(offset) + relative);
				offset += 4; // Skip the ADDI half of the pair
			}
		}

		assert(addresses.size() == 2 && "test() should compute two global addresses");
		const int64_t stride = codegen.get_layout().variant_size();
		const int64_t expected = static_cast<int64_t>(count - 1) * stride;
		const int64_t actual = addresses[1] - addresses[0];
		if (actual != expected) {
			std::cerr << "With " << count << " globals: g0 and g" << (count - 1)
				<< " are " << actual << " bytes apart, expected " << expected << std::endl;
			assert(false && "global address truncated");
		}
	}

	std::cout << "  Global addressing OK" << std::endl;
}

void test_large_frames_still_compile() {
	std::cout << "Testing a function whose frame is past the 12-bit immediate..." << std::endl;

	// Every local is a Variant slot, so enough of them pushes the stack frame
	// and the slot offsets past 2047 and onto emit_add_offset's wide path.
	// Compiled unoptimized on purpose: the point is the wide offsets, and the
	// optimizer would fold most of these locals away before the backend sees
	// them.
	std::string source = "func test(n):\n\tvar v0 = n\n";
	for (int i = 1; i < 300; i++) {
		source += "\tvar v" + std::to_string(i) + " = v" + std::to_string(i - 1) + " + n\n";
	}
	source += "\treturn v0 + v299\n";

	RISCVCodeGen codegen;
	const std::vector<uint8_t> code = compile_to_code(source, codegen, /*optimize=*/false);
	assert(!code.empty());

	// 300 Variant slots is well past the 2047 an addi can reach, so this
	// program only encodes at all because the wide path exists.
	assert(300 * codegen.get_layout().variant_size() > 2047);

	std::cout << "  Large frames OK (" << code.size() << " bytes of code)" << std::endl;
}

// A store whose offset does not fit in the immediate has to compute the address
// somewhere. Computing it in a temporary the surrounding instruction was
// already using destroys the value being stored -- `sd t2, 2048(sp)` became
// "compute sp+2048 into t2, then store t2", writing a stack address into the
// Variant. A frame over 2047 bytes was enough to hit it, which is around eighty
// locals.
void test_wide_stores_keep_their_value() {
	std::cout << "Testing that a wide store stores the value, not the address..." << std::endl;

	// Enough locals that the upper Variant slots are past the 12-bit immediate.
	std::string source = "func test():\n";
	for (int i = 0; i < 120; i++) {
		source += "\tvar v" + std::to_string(i) + " = " + std::to_string(i) + "\n";
	}
	source += "\treturn v0 + v119\n";

	RISCVCodeGen codegen;
	const std::vector<uint8_t> code = compile_to_code(source, codegen, /*optimize=*/false);
	assert(!code.empty());
	assert(120 * codegen.get_layout().variant_size() > 2047);

	// The address scratch is reserved, so no store may name it as the value it
	// is storing. That is exactly the shape the bug produced.
	size_t stores = 0;
	for (size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
		const uint32_t instr = word_at(code, offset);
		if ((instr & 0x7F) != 0x23) {
			continue; // Not a store
		}
		stores++;
		const uint8_t rs2 = (instr >> 20) & 0x1F;
		if (rs2 == RISCVCodeGen::REG_WIDE_SCRATCH) {
			std::cerr << "A store at offset " << offset << " stores the wide-address register"
				<< std::endl;
			assert(false && "a wide store clobbered the value it was storing");
		}
	}
	assert(stores > 0 && "no stores were generated, so this test proves nothing");

	std::cout << "  Wide stores OK (" << stores << " stores checked)" << std::endl;
}

// A B-type branch reaches +-4KB. A function whose body outgrows that used to
// get a masked displacement -- a branch to somewhere else entirely. The encoder
// now refuses to emit one, and the code generator turns it into an inverted
// branch over a jump instead, which reaches +-1MB.
void test_far_branches_are_relaxed() {
	std::cout << "Testing that a branch too far to encode is relaxed..." << std::endl;

	// A loop whose body is thousands of instructions long, so the exit branch
	// cannot reach the end of the loop.
	std::string source = "func test():\n\tvar total = 0\n\tvar i = 0\n\twhile i < 3:\n";
	for (int k = 0; k < 240; k++) {
		source += "\t\tvar a" + std::to_string(k) + " = i + " + std::to_string(k) + "\n";
	}
	source += "\t\ttotal = total";
	for (int k = 0; k < 240; k++) {
		source += " + a" + std::to_string(k);
	}
	source += "\n\t\ti = i + 1\n\treturn total\n";

	RISCVCodeGen codegen;
	const std::vector<uint8_t> code = compile_to_code(source, codegen);
	assert(!code.empty());

	// Every branch in the result has to be in range: relaxation ran, and ran to
	// a fixpoint. Decoding the displacement is the check -- a masked one would
	// look in range while pointing somewhere else, so this also confirms the
	// relaxed form is what was emitted.
	size_t branches = 0;
	size_t relaxed = 0;
	for (size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
		const uint32_t instr = word_at(code, offset);
		if ((instr & 0x7F) != 0x63) {
			continue;
		}
		branches++;
		const int32_t imm =
			(static_cast<int32_t>(instr) >> 31 << 12) |
			static_cast<int32_t>(((instr >> 7) & 1) << 11) |
			static_cast<int32_t>(((instr >> 25) & 0x3F) << 5) |
			static_cast<int32_t>(((instr >> 8) & 0xF) << 1);
		assert(RISCVCodeGen::fits_in_signed(imm, RISCVCodeGen::B_TYPE_IMM_BITS));

		// The relaxed form: a branch over the following jump.
		if (imm == 8 && offset + 8 <= code.size() && (word_at(code, offset + 4) & 0x7F) == 0x6F) {
			relaxed++;
		}
	}
	assert(branches > 0 && "no branches were generated, so this test proves nothing");
	assert(relaxed > 0 && "no branch was relaxed, so this test is not exercising relaxation");

	std::cout << "  Far branches OK (" << branches << " branches, " << relaxed
		<< " relaxed, " << code.size() << " bytes of code)" << std::endl;
}

} // namespace

int main() {
	std::cout << "=== Instruction encoding limits ===" << std::endl;

	test_range_boundaries();
	test_out_of_range_is_an_error();
	test_global_addressing_does_not_truncate();
	test_large_frames_still_compile();
	test_wide_stores_keep_their_value();
	test_far_branches_are_relaxed();

	std::cout << "All encoder tests passed!" << std::endl;
	return 0;
}
