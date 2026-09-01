#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../riscv_codegen.h"
#include "../elf_builder.h"
#include <cassert>
#include <cstring>
#include <iostream>

using namespace gdscript;

// The host resolves "fast_exit" from the ELF and makes it the address every
// vmcall returns to. Without the symbol libriscv injects a one-page trampoline
// in a second execute segment, and every return leaves translated code to
// switch segments.
static constexpr uint64_t BASE_ADDR = 0x10000;
static constexpr uint32_t STOP_INSN = 0x7ff00073;
static constexpr uint32_t JUMP_BACK_INSN = 0xffdff06f; // j -4

static std::vector<uint8_t> build(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	ElfBuilder elf;
	return elf.build(ir);
}

// Minimal ELF readers: the tests own no ELF headers, and the builder's are private.
static uint64_t read_u64(const std::vector<uint8_t>& elf, size_t offset) {
	uint64_t value = 0;
	memcpy(&value, elf.data() + offset, sizeof(value));
	return value;
}

static uint32_t read_u32(const std::vector<uint8_t>& elf, size_t offset) {
	uint32_t value = 0;
	memcpy(&value, elf.data() + offset, sizeof(value));
	return value;
}

struct Symbol {
	uint64_t value;
	uint64_t size;
	bool found;
};

// Walks .symtab for a name, using the section headers rather than assuming a layout.
static Symbol find_symbol(const std::vector<uint8_t>& elf, const char *name) {
	const uint64_t shoff = read_u64(elf, 0x28);
	const uint16_t shentsize = uint16_t(read_u32(elf, 0x3a) & 0xFFFF);
	const uint16_t shnum = uint16_t(read_u32(elf, 0x3c) & 0xFFFF);

	for (uint16_t i = 0; i < shnum; i++) {
		const size_t shdr = size_t(shoff) + size_t(i) * shentsize;
		if (read_u32(elf, shdr + 4) != 2) { // SHT_SYMTAB
			continue;
		}
		const uint64_t symtab_off = read_u64(elf, shdr + 0x18);
		const uint64_t symtab_size = read_u64(elf, shdr + 0x20);
		const uint32_t strtab_index = read_u32(elf, shdr + 0x28);
		const size_t strtab_shdr = size_t(shoff) + size_t(strtab_index) * shentsize;
		const uint64_t strtab_off = read_u64(elf, strtab_shdr + 0x18);

		for (uint64_t off = 0; off + 24 <= symtab_size; off += 24) {
			const size_t sym = size_t(symtab_off + off);
			const uint32_t st_name = read_u32(elf, sym);
			const char *sym_name = (const char *)elf.data() + strtab_off + st_name;
			if (strcmp(sym_name, name) == 0) {
				return Symbol{ read_u64(elf, sym + 8), read_u64(elf, sym + 16), true };
			}
		}
	}
	return Symbol{ 0, 0, false };
}

// Offset of a virtual address inside the file, via the first PT_LOAD segment.
static size_t file_offset_of(const std::vector<uint8_t>& elf, uint64_t vaddr) {
	const uint64_t phoff = read_u64(elf, 0x20);
	const uint64_t p_offset = read_u64(elf, size_t(phoff) + 0x08);
	const uint64_t p_vaddr = read_u64(elf, size_t(phoff) + 0x10);
	assert(vaddr >= p_vaddr);
	return size_t(p_offset + (vaddr - p_vaddr));
}

void test_fast_exit_symbol_is_exported() {
	std::cout << "Testing fast_exit is exported..." << std::endl;

	const auto elf = build("func test():\n\treturn 1\n");
	const Symbol sym = find_symbol(elf, FAST_EXIT_SYMBOL);
	assert(sym.found);
	assert(sym.value == BASE_ADDR);
	assert(sym.size == FAST_EXIT_SIZE);

	std::cout << "  ✓ fast_exit at 0x" << std::hex << sym.value << std::dec
		<< ", " << sym.size << " bytes" << std::endl;
}

void test_fast_exit_stops_and_jumps_back() {
	std::cout << "Testing fast_exit instructions..." << std::endl;

	// The jump back is what libriscv's own trampoline does: resuming a stopped
	// machine has to stop again, not fall into the entry code that follows.
	const auto elf = build("var counter := 0\n\nfunc test():\n\tcounter += 1\n\treturn counter\n");
	const size_t offset = file_offset_of(elf, BASE_ADDR);
	assert(read_u32(elf, offset) == STOP_INSN);
	assert(read_u32(elf, offset + 4) == JUMP_BACK_INSN);

	std::cout << "  ✓ STOP followed by a jump back onto it" << std::endl;
}

void test_entry_point_skips_fast_exit() {
	std::cout << "Testing entry point..." << std::endl;

	// The entry runs global init and registers properties; it must not begin on
	// the STOP, or loading the program would halt before any of that.
	const auto elf = build("@export var value := 7\n\nfunc test():\n\treturn value\n");
	assert(read_u64(elf, 0x18) == BASE_ADDR + FAST_EXIT_SIZE);

	std::cout << "  ✓ e_entry is past fast_exit" << std::endl;
}

void test_functions_start_after_the_entry() {
	std::cout << "Testing function symbols..." << std::endl;

	const auto elf = build("func first():\n\treturn 1\n\nfunc second():\n\treturn 2\n");
	for (const char *name : { "first", "second" }) {
		const Symbol sym = find_symbol(elf, name);
		assert(sym.found);
		assert(sym.value > BASE_ADDR + FAST_EXIT_SIZE);
	}

	std::cout << "  ✓ every function lives past the entry" << std::endl;
}

int main() {
	std::cout << "=== Fast Exit Tests ===" << std::endl;

	try {
		test_fast_exit_symbol_is_exported();
		test_fast_exit_stops_and_jumps_back();
		test_entry_point_skips_fast_exit();
		test_functions_start_after_the_entry();

		std::cout << std::endl;
		std::cout << "✅ All fast exit tests passed!" << std::endl;
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "❌ Test failed: " << e.what() << std::endl;
		return 1;
	}
}
