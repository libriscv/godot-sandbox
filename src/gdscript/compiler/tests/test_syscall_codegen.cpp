#include "compiler.h"
#include "syscall_abi.h"
#include "../../../../ext/libriscv/lib/libriscv/dyncall.hpp"
#include <elf.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <tuple>

static void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}
static uint32_t encode(unsigned number, unsigned inputs, unsigned outputs, bool floats = true) {
	return riscv::Dyncall::encode(number, inputs, outputs, floats);
}
int main(int argc, char** argv) try {
	using namespace gdscript;
	// Counts are independently stated here, including operation-specific forms.
	using Signature = std::tuple<unsigned, int64_t, unsigned, unsigned>;
	const Signature expected[] = {
		{ECALL_ARRAY_SIZE, -1, 1, 1}, {ECALL_STRING_SIZE, -1, 1, 1},
		{ECALL_STRING_AT, -1, 2, 1}, {ECALL_STRING_BATCH, -1, 3, 1},
		{ECALL_ARRAY_BATCH, -1, 4, 1}, {ECALL_STRING_CODEPOINT_BATCH, -1, 4, 1},
		{ECALL_CALLABLE_CREATE, -1, 4, 1},
		{ECALL_VCREATE, -1, 4, 0}, {ECALL_ARRAY_AT, -1, 3, 1},
		{ECALL_DICTIONARY_OPS, int(Dictionary_Op::GET), 5, 1},
		{ECALL_VSCOPE, int(Scope_Op::MARK), 1, 1},
		{ECALL_DICTIONARY_OPS, int(Dictionary_Op::GET_SIZE), 2, 1},
		{ECALL_UTILITY, int(Utility_Op::RANDI), 1, 1},
		{ECALL_UTILITY, int(Utility_Op::RANDI_RANGE), 3, 1},
		{ECALL_UTILITY, int(Utility_Op::NEAREST_PO2), 2, 1},
		{ECALL_UTILITY, int(Utility_Op::SIN), 1, 0},
	};
	for (auto [number, op, inputs, outputs] : expected) {
		for (unsigned i = 0; i < 32; i++) for (unsigned o = 0; o < 32; o++) {
			const auto word = (number << 20) | (o << 15) | (7u << 12) | (i << 7) | 0x5b;
			const bool fp = number == ECALL_UTILITY && op == int(Utility_Op::SIN);
			const bool counts = i == (16 | inputs) &&
				(o == (28 | outputs) || (!fp && o == (24 | outputs)));
			const bool general_dictionary = number == ECALL_DICTIONARY_OPS &&
				i == 21 && (o == 29 || o == 25);
			require(valid_counted_syscall(word, op) == (counts || general_dictionary), "signature validation mismatch");
		}
		require(valid_counted_syscall_encoding(encode(number, inputs, outputs)), "valid encoding refused");
	}
	require(!valid_counted_syscall(encode(ECALL_VSCOPE, 1, 1), int(Scope_Op::RELEASE)), "scope RELEASE accepted as MARK");
	require(!valid_counted_syscall(encode(ECALL_UTILITY, 1, 1), int(Utility_Op::SIN)), "FP utility accepted as GPR-only");
	require(!valid_counted_syscall(encode(ECALL_DICTIONARY_OPS, 2, 1), int(Dictionary_Op::GET)), "dictionary GET accepted as GET_SIZE");
	for (unsigned i = 0; i < 4096; i++) {
		if (i < GAME_API_BASE || i >= ECALL_LAST)
			require(!valid_counted_syscall_encoding(encode(i, 1, 1)), "unknown syscall accepted");
	}
	for (unsigned n = 0; n < 4096; n++)
		for (unsigned i = 0; i <= 8; i++) for (unsigned o = 0; o <= 2; o++)
			for (bool fp : {false, true})
				require(encode_counted_syscall(n, i, o, fp) == encode(n, i, o, fp), "compiler/upstream encoding drift");
	// Neither the old counted form nor a tag-stripped word is accepted.
	for (unsigned f = 0; f < 8; f++) for (unsigned rd = 0; rd < 32; rd++)
		for (unsigned rs = 0; rs < 32; rs++) {
			const auto w = (ECALL_ARRAY_SIZE << 20) | (rs << 15) | (f << 12) | (rd << 7) | 0x5b;
			require(valid_counted_syscall_encoding(w) == (w == encode(ECALL_ARRAY_SIZE, 1, 1) || w == encode(ECALL_ARRAY_SIZE, 1, 1, false)), "tag validation mismatch");
		}
	require(!valid_counted_syscall(encode(ECALL_UTILITY, 1, 0), int(Utility_Op::STR)), "boxed utility accepted as FP");
	require(!valid_counted_syscall(encode(ECALL_UTILITY, 1, 0), 4095), "unknown utility accepted as FP");
	require(!valid_counted_syscall_encoding(encode(ECALL_UTILITY, 1, 0, false)), "FP utility accepted without synchronization");
	std::ifstream input(SGD_SYSCALL_SOURCE);
	std::string source{std::istreambuf_iterator<char>(input), {}};
	Compiler compiler;
	auto elf = compiler.compile(source);
	if (elf.empty()) throw std::runtime_error(compiler.get_error());
	auto legacy = elf;
	auto malformed = elf;
	auto malformed_fp = elf;
	auto old_encoding = elf;
	bool changed_fp = false;
	bool changed_operation = false;
	Elf64_Ehdr header{};
	std::memcpy(&header, elf.data(), sizeof(header));
	std::set<std::tuple<unsigned, unsigned, unsigned>> found;
	std::set<unsigned> ecalls;
	for (unsigned section = 0; section < header.e_shnum; section++) {
		Elf64_Shdr sh{};
		std::memcpy(&sh, elf.data() + header.e_shoff + section * header.e_shentsize, sizeof(sh));
		if (!(sh.sh_flags & SHF_EXECINSTR)) continue;
		uint32_t previous = 0;
		for (size_t off = sh.sh_offset; off + 4 <= sh.sh_offset + sh.sh_size; off += 4) {
			uint32_t word;
			std::memcpy(&word, elf.data() + off, sizeof(word));
			if ((word & 127) == 0x5b) {
				require(valid_counted_syscall_encoding(word), "compiler emitted an invalid counted syscall");
				require(riscv::Dyncall::floats(word) == ((word >> 20) == ECALL_UTILITY && riscv::Dyncall::outputs(word) == 0), "incorrect compiler FP flag");
				found.emplace(word >> 20, riscv::Dyncall::inputs(word), riscv::Dyncall::outputs(word));
				require(previous != ((word >> 20) << 20 | (17 << 7) | 0x13), "redundant a7 setup before dyncall");
				const uint32_t uncounted = (word & 0xfff00000u) | 0x5b;
				std::memcpy(legacy.data() + off, &uncounted, sizeof(uncounted));
				const uint32_t old = ((word >> 20) << 20) | (riscv::Dyncall::outputs(word) << 15) |
					(1u << 12) | (riscv::Dyncall::inputs(word) << 7) | 0x5b;
				std::memcpy(old_encoding.data() + off, &old, sizeof(old));
				if (word == encode(ECALL_UTILITY, 1, 0)) {
					const uint32_t bad = encode(ECALL_UTILITY, 1, 0, false);
					std::memcpy(malformed_fp.data() + off, &bad, sizeof(bad));
					changed_fp = true;
				}
				if (word == encode(ECALL_UTILITY, 3, 1, false)) {
					// RANDI's valid signature cannot be used for RANDI_RANGE.
					const uint32_t bad = encode(ECALL_UTILITY, 1, 1);
					std::memcpy(malformed.data() + off, &bad, sizeof(bad));
					changed_operation = true;
				}
			} else if (word == 0x73 && (previous & 0xfffff) == ((17 << 7) | 0x13)) {
				ecalls.insert(previous >> 20);
			}
			previous = word;
		}
	}
	for (auto [number, op, inputs, outputs] : expected)
		if (!found.count({number, inputs, outputs}))
			throw std::runtime_error("missing compiler lowering " + std::to_string(number) + "/" + std::to_string(inputs) + "/" + std::to_string(outputs));
	for (unsigned number : {ECALL_UTILITY, ECALL_BREAKPOINT, ECALL_VSCOPE})
		require(ecalls.count(number), "missing required full-state ECALL");
	for (unsigned number : {ECALL_VCREATE, ECALL_ARRAY_AT})
		require(!ecalls.count(number), "eligible syscall still lowered to ECALL");
	for (unsigned number = GAME_API_BASE; number < ECALL_LAST; number++) {
		const auto abi = syscall_abi(number);
		if (abi.counted)
			require(valid_counted_syscall_encoding(encode(number, abi.inputs, abi.outputs)), "new ABI signature refused");
	}
	require(changed_fp, "missing FP mismatch fixture");
	require(changed_operation, "missing malformed operation fixture");
	if (argc > 1) {
		std::filesystem::path out(argv[1]);
		std::filesystem::create_directories(out);
		std::ofstream(out / "counted.elf", std::ios::binary).write(reinterpret_cast<const char*>(elf.data()), elf.size());
		std::ofstream(out / "legacy.elf", std::ios::binary).write(reinterpret_cast<const char*>(legacy.data()), legacy.size());
		std::ofstream(out / "malformed-fp.elf", std::ios::binary).write(reinterpret_cast<const char*>(malformed_fp.data()), malformed_fp.size());
		std::ofstream(out / "old-encoding.elf", std::ios::binary).write(reinterpret_cast<const char*>(old_encoding.data()), old_encoding.size());
		std::ofstream(out / "malformed.elf", std::ios::binary).write(reinterpret_cast<const char*>(malformed.data()), malformed.size());
	}
	std::cout << "PASS 16 compiler signatures, exhaustive ABI/tag checks, full-state fallbacks, no redundant a7 loads\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
