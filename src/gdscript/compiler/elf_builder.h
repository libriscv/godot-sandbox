#pragma once
#include "ir.h"
#include "line_table.h"
#include "instance_layout.h"
#include "profiling_layout.h"
#include "variant_layout.h"
#include <vector>
#include <cstdint>
#include <string>

namespace gdscript {

class ElfBuilder {
public:
	ElfBuilder();

	std::vector<uint8_t> build(const IRProgram& program, const VariantLayout& layout = native_variant_layout(),
		bool profiling = false, ProfilingClock profiling_clock = ProfilingClock::TIME,
		bool debug_info = false, const std::vector<uint32_t>& breakpoint_lines = {});

	// Addresses rebased to ELF virtual addresses. Valid after build().
	const LineTable& get_line_table() const { return m_line_table; }
	const std::vector<uint32_t>& get_installed_breakpoints() const { return m_installed_breakpoints; }

private:
	LineTable m_line_table;
	std::vector<uint32_t> m_installed_breakpoints;

	struct Elf64_Ehdr {
		uint8_t e_ident[16];
		uint16_t e_type;
		uint16_t e_machine;
		uint32_t e_version;
		uint64_t e_entry;
		uint64_t e_phoff;
		uint64_t e_shoff;
		uint32_t e_flags;
		uint16_t e_ehsize;
		uint16_t e_phentsize;
		uint16_t e_phnum;
		uint16_t e_shentsize;
		uint16_t e_shnum;
		uint16_t e_shstrndx;
	};

	struct Elf64_Phdr {
		uint32_t p_type;
		uint32_t p_flags;
		uint64_t p_offset;
		uint64_t p_vaddr;
		uint64_t p_paddr;
		uint64_t p_filesz;
		uint64_t p_memsz;
		uint64_t p_align;
	};

	struct Elf64_Shdr {
		uint32_t sh_name;
		uint32_t sh_type;
		uint64_t sh_flags;
		uint64_t sh_addr;
		uint64_t sh_offset;
		uint64_t sh_size;
		uint32_t sh_link;
		uint32_t sh_info;
		uint64_t sh_addralign;
		uint64_t sh_entsize;
	};

	void write_elf_header(std::vector<uint8_t>& data, uint64_t entry_point);
	void write_program_headers(std::vector<uint8_t>& data);
	void write_section_headers(std::vector<uint8_t>& data);
	void write_code_section(std::vector<uint8_t>& data, const std::vector<uint8_t>& code);

	template<typename T>
	void write_value(std::vector<uint8_t>& data, T value) {
		const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
		data.insert(data.end(), bytes, bytes + sizeof(T));
	}

	std::vector<uint8_t> generate_minimal_code(const IRProgram& program);

	static constexpr uint16_t ET_EXEC = 2;
	static constexpr uint16_t EM_RISCV = 243;
	static constexpr uint32_t EV_CURRENT = 1;
	static constexpr uint64_t BASE_ADDR = 0x10000;
};

} // namespace gdscript
