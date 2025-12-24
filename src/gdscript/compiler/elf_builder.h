#pragma once
#include "ir.h"
#include <vector>
#include <cstdint>
#include <string>

namespace gdscript {

// ELF file format structures for RV64
class ElfBuilder {
public:
	ElfBuilder();

	// Build a complete ELF file from IR program
	std::vector<uint8_t> build(const IRProgram& program);

private:
	// ELF header structures
	struct Elf64_Ehdr {
		uint8_t e_ident[16];    // ELF identification
		uint16_t e_type;        // Object file type
		uint16_t e_machine;     // Machine type
		uint32_t e_version;     // Object file version
		uint64_t e_entry;       // Entry point address
		uint64_t e_phoff;       // Program header offset
		uint64_t e_shoff;       // Section header offset
		uint32_t e_flags;       // Processor-specific flags
		uint16_t e_ehsize;      // ELF header size
		uint16_t e_phentsize;   // Size of program header entry
		uint16_t e_phnum;       // Number of program header entries
		uint16_t e_shentsize;   // Size of section header entry
		uint16_t e_shnum;       // Number of section header entries
		uint16_t e_shstrndx;    // Section name string table index
	};

	struct Elf64_Phdr {
		uint32_t p_type;        // Segment type
		uint32_t p_flags;       // Segment flags
		uint64_t p_offset;      // Segment offset in file
		uint64_t p_vaddr;       // Virtual address
		uint64_t p_paddr;       // Physical address
		uint64_t p_filesz;      // Size in file
		uint64_t p_memsz;       // Size in memory
		uint64_t p_align;       // Alignment
	};

	struct Elf64_Shdr {
		uint32_t sh_name;       // Section name (string table index)
		uint32_t sh_type;       // Section type
		uint64_t sh_flags;      // Section flags
		uint64_t sh_addr;       // Address in memory
		uint64_t sh_offset;     // Offset in file
		uint64_t sh_size;       // Section size
		uint32_t sh_link;       // Link to other section
		uint32_t sh_info;       // Additional information
		uint64_t sh_addralign;  // Alignment
		uint64_t sh_entsize;    // Entry size for fixed-size entries
	};

	void write_elf_header(std::vector<uint8_t>& data, uint64_t entry_point);
	void write_program_headers(std::vector<uint8_t>& data);
	void write_section_headers(std::vector<uint8_t>& data);
	void write_code_section(std::vector<uint8_t>& data, const std::vector<uint8_t>& code);

	// Helper to write data
	template<typename T>
	void write_value(std::vector<uint8_t>& data, T value) {
		const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
		data.insert(data.end(), bytes, bytes + sizeof(T));
	}

	// Generate minimal RISC-V code from IR (simplified version)
	std::vector<uint8_t> generate_minimal_code(const IRProgram& program);

	static constexpr uint16_t ET_EXEC = 2;
	static constexpr uint16_t EM_RISCV = 243;
	static constexpr uint32_t EV_CURRENT = 1;
	static constexpr uint64_t BASE_ADDR = 0x10000;
};

} // namespace gdscript
