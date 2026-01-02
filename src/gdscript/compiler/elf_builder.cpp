#include "elf_builder.h"
#include "riscv_codegen.h"
#include <cstring>
#include <stdexcept>

namespace gdscript {

ElfBuilder::ElfBuilder() {}

std::vector<uint8_t> ElfBuilder::build(const IRProgram& program) {
	// Generate RISC-V machine code from IR
	RISCVCodeGen codegen;
	std::vector<uint8_t> code = codegen.generate(program);
	auto func_offsets = codegen.get_function_offsets();
	auto const_pool = codegen.get_constant_pool();
	auto global_data_size = codegen.get_global_data_size();

	// Build complete ELF file
	std::vector<uint8_t> elf_data;

	// Calculate offsets
	size_t ehdr_size = sizeof(Elf64_Ehdr);
	size_t phdr_size = sizeof(Elf64_Phdr);
	size_t shdr_size = sizeof(Elf64_Shdr);

	// Determine if we need separate .text and .data segments
	// If we have global variables, we need two PT_LOAD segments:
	// 1. .text segment (R+X) - code and constants
	// 2. .data segment (R+W) - global variables
	bool has_globals = global_data_size > 0;
	size_t num_phdrs = has_globals ? 2 : 1;

	// We'll have 5 or 6 sections depending on whether we have globals
	// NULL, .text, [.data], .symtab, .strtab, .shstrtab
	size_t num_sections = has_globals ? 6 : 5;

	// Calculate section sizes
	// The code vector includes: code + constant pool + global data (if any)
	// We need to split it if we have globals
	size_t text_size = code.size() - global_data_size;
	size_t data_size = global_data_size;

	// Build string tables
	std::vector<std::string> section_names;
	if (has_globals) {
		section_names = {"", ".text", ".data", ".symtab", ".strtab", ".shstrtab"};
	} else {
		section_names = {"", ".text", ".symtab", ".strtab", ".shstrtab"};
	}
	std::vector<uint8_t> shstrtab;
	shstrtab.reserve(1 + (1 + section_names.size()) * 10); // Rough estimate
	std::vector<size_t> section_name_offsets;
	section_name_offsets.reserve(section_names.size());

	for (const auto& name : section_names) {
		section_name_offsets.push_back(shstrtab.size());
		shstrtab.insert(shstrtab.end(), name.begin(), name.end());
		shstrtab.push_back(0);
	}

	// Build symbol string table
	std::vector<uint8_t> strtab;
	strtab.push_back(0); // First byte is always null

	std::vector<std::string> symbol_names;
	symbol_names.reserve(program.functions.size());
	std::vector<size_t> symbol_name_offsets;
	symbol_name_offsets.reserve(program.functions.size());

	for (const auto& func : program.functions) {
		symbol_names.push_back(func.name);
		symbol_name_offsets.push_back(strtab.size());
		strtab.insert(strtab.end(), func.name.begin(), func.name.end());
		strtab.push_back(0);
	}

	// Build symbol table
	// NOTE: Keep this struct defined here to avoid any issues with alignment/packing
	struct alignas(8) Elf64_Sym {
		uint32_t st_name;
		uint8_t st_info;
		uint8_t st_other;
		uint16_t st_shndx;
		uint64_t st_value;
		uint64_t st_size;
	};

	static_assert(sizeof(Elf64_Sym) == 24, "Elf64_Sym must be 24 bytes");

	std::vector<Elf64_Sym> symtab;
	symtab.reserve(1 + program.functions.size());

	// First symbol is always null
	Elf64_Sym null_sym = {};
	memset(&null_sym, 0, sizeof(null_sym));  // Ensure all bytes are zero
	symtab.push_back(null_sym);

	// Add function symbols
	for (size_t i = 0; i < program.functions.size(); i++) {
		const auto& func = program.functions[i];
		size_t func_offset = func_offsets.at(func.name);

		// Calculate function size (distance to next function or end of text section)
		size_t func_size = text_size - func_offset;
		if (i + 1 < program.functions.size()) {
			const auto& next_func = program.functions[i + 1];
			size_t next_offset = func_offsets.at(next_func.name);
			func_size = next_offset - func_offset;
		}

		Elf64_Sym sym = {};
		memset(&sym, 0, sizeof(sym));  // Ensure all bytes are zero
		sym.st_name = static_cast<uint32_t>(symbol_name_offsets[i]);
		sym.st_info = (1 << 4) | 2; // STB_GLOBAL (1) << 4 | STT_FUNC (2)
		sym.st_other = 0;
		sym.st_shndx = 1; // .text section
		sym.st_value = BASE_ADDR + func_offset; // Actual function address
		sym.st_size = func_size;
		symtab.push_back(sym);
	}

	size_t symtab_size = symtab.size() * sizeof(Elf64_Sym);

	// Calculate file layout
	size_t offset = 0;

	// ELF header
	size_t ehdr_offset = offset;
	offset += ehdr_size;

	// Program headers
	size_t phdr_offset = offset;
	offset += num_phdrs * phdr_size;

	// Align to page
	offset = (offset + 0xFFF) & ~0xFFF;

	// .text section (code + constant pool)
	size_t text_offset = offset;
	offset += text_size;

	// .data section (global variables) - only if we have globals
	size_t data_offset = 0;
	if (has_globals) {
		// Align to page boundary for separate PT_LOAD segment
		offset = (offset + 0xFFF) & ~0xFFF;
		data_offset = offset;
		offset += data_size;
	}

	// Align
	offset = (offset + 7) & ~7;

	// .symtab section
	size_t symtab_offset = offset;
	offset += symtab_size;

	// .strtab section
	size_t strtab_offset = offset;
	offset += strtab.size();

	// .shstrtab section
	size_t shstrtab_offset = offset;
	offset += shstrtab.size();

	// Align
	offset = (offset + 7) & ~7;

	// Section headers
	size_t shdr_offset = offset;

	// Now write everything

	// 1. ELF Header
	Elf64_Ehdr ehdr;
	memset(&ehdr, 0, sizeof(ehdr));

	ehdr.e_ident[0] = 0x7f;
	ehdr.e_ident[1] = 'E';
	ehdr.e_ident[2] = 'L';
	ehdr.e_ident[3] = 'F';
	ehdr.e_ident[4] = 2;      // 64-bit
	ehdr.e_ident[5] = 1;      // Little endian
	ehdr.e_ident[6] = 1;      // ELF version
	ehdr.e_ident[7] = 0;      // System V ABI

	ehdr.e_type = ET_EXEC;
	ehdr.e_machine = EM_RISCV;
	ehdr.e_version = EV_CURRENT;
	ehdr.e_entry = BASE_ADDR;
	ehdr.e_phoff = phdr_offset;
	ehdr.e_shoff = shdr_offset;
	ehdr.e_flags = 0x5; // RV64I + RV64M (multiply/divide)
	ehdr.e_ehsize = sizeof(Elf64_Ehdr);
	ehdr.e_phentsize = sizeof(Elf64_Phdr);
	ehdr.e_phnum = static_cast<uint16_t>(num_phdrs);
	ehdr.e_shentsize = sizeof(Elf64_Shdr);
	ehdr.e_shnum = static_cast<uint16_t>(num_sections);
	ehdr.e_shstrndx = has_globals ? 5 : 4; // .shstrtab is the last section

	write_value(elf_data, ehdr);

	// 2. Program Headers
	// First PT_LOAD: .text (R+X) - code and constants
	Elf64_Phdr phdr_text;
	memset(&phdr_text, 0, sizeof(phdr_text));

	phdr_text.p_type = 1; // PT_LOAD
	phdr_text.p_flags = 5; // PF_R | PF_X (readable + executable)
	phdr_text.p_offset = static_cast<uint64_t>(text_offset);
	phdr_text.p_vaddr = BASE_ADDR;
	phdr_text.p_paddr = BASE_ADDR;
	phdr_text.p_filesz = static_cast<uint64_t>(text_size);
	phdr_text.p_memsz = static_cast<uint64_t>(text_size);
	phdr_text.p_align = 0x1000;

	write_value(elf_data, phdr_text);

	// Second PT_LOAD: .data (R+W) - global variables (if any)
	if (has_globals) {
		// Calculate data virtual address (after text segment, aligned to page)
		uint64_t data_vaddr = BASE_ADDR + text_size;
		data_vaddr = (data_vaddr + 0xFFF) & ~0xFFFULL; // Align to 4KB page

		Elf64_Phdr phdr_data;
		memset(&phdr_data, 0, sizeof(phdr_data));

		phdr_data.p_type = 1; // PT_LOAD
		phdr_data.p_flags = 6; // PF_R | PF_W (readable + writable)
		phdr_data.p_offset = static_cast<uint64_t>(data_offset);
		phdr_data.p_vaddr = data_vaddr;
		phdr_data.p_paddr = data_vaddr;
		phdr_data.p_filesz = static_cast<uint64_t>(data_size);
		phdr_data.p_memsz = static_cast<uint64_t>(data_size);
		phdr_data.p_align = 0x1000;

		write_value(elf_data, phdr_data);
	}

	// Pad to text section
	while (elf_data.size() < text_offset) {
		elf_data.push_back(0);
	}

	// 3. .text section (code + constant pool, excluding global data)
	elf_data.insert(elf_data.end(), code.begin(), code.begin() + text_size);

	// 4. .data section (global variables) - only if we have globals
	if (has_globals) {
		// Pad to data section
		while (elf_data.size() < data_offset) {
			elf_data.push_back(0);
		}
		// Write global data
		elf_data.insert(elf_data.end(), code.begin() + text_size, code.end());
	}

	// Pad to symtab
	while (elf_data.size() < symtab_offset) {
		elf_data.push_back(0);
	}

	// 4. .symtab section
	for (const auto& sym : symtab) {
		write_value(elf_data, sym);
	}

	// 5. .strtab section
	elf_data.insert(elf_data.end(), strtab.begin(), strtab.end());

	// 6. .shstrtab section
	elf_data.insert(elf_data.end(), shstrtab.begin(), shstrtab.end());

	// Pad to section headers
	while (elf_data.size() < shdr_offset) {
		elf_data.push_back(0);
	}

	// 7. Section Headers

	// Section 0: NULL
	Elf64_Shdr shdr_null = {};
	write_value(elf_data, shdr_null);

	// Section 1: .text
	Elf64_Shdr shdr_text = {};
	shdr_text.sh_name = static_cast<uint32_t>(section_name_offsets[1]);
	shdr_text.sh_type = 1; // SHT_PROGBITS
	shdr_text.sh_flags = 6; // SHF_ALLOC | SHF_EXECINSTR
	shdr_text.sh_addr = BASE_ADDR;
	shdr_text.sh_offset = static_cast<uint64_t>(text_offset);
	shdr_text.sh_size = static_cast<uint64_t>(text_size);
	shdr_text.sh_addralign = 4;
	write_value(elf_data, shdr_text);

	// Section 2: .data (only if we have globals)
	if (has_globals) {
		// Calculate data virtual address (same as in PT_LOAD)
		uint64_t data_vaddr = BASE_ADDR + text_size;
		data_vaddr = (data_vaddr + 0xFFF) & ~0xFFFULL; // Align to 4KB page

		Elf64_Shdr shdr_data = {};
		shdr_data.sh_name = static_cast<uint32_t>(section_name_offsets[2]);
		shdr_data.sh_type = 1; // SHT_PROGBITS
		shdr_data.sh_flags = 3; // SHF_WRITE | SHF_ALLOC
		shdr_data.sh_addr = data_vaddr;
		shdr_data.sh_offset = static_cast<uint64_t>(data_offset);
		shdr_data.sh_size = static_cast<uint64_t>(data_size);
		shdr_data.sh_addralign = 8;
		write_value(elf_data, shdr_data);
	}

	// Section: .symtab (index 2 or 3 depending on has_globals)
	size_t symtab_idx = has_globals ? 3 : 2;
	size_t strtab_idx = has_globals ? 4 : 3;
	Elf64_Shdr shdr_symtab = {};
	shdr_symtab.sh_name = static_cast<uint32_t>(section_name_offsets[symtab_idx]);
	shdr_symtab.sh_type = 2; // SHT_SYMTAB
	shdr_symtab.sh_offset = static_cast<uint64_t>(symtab_offset);
	shdr_symtab.sh_size = static_cast<uint64_t>(symtab_size);
	shdr_symtab.sh_link = static_cast<uint32_t>(strtab_idx); // Link to .strtab
	shdr_symtab.sh_info = 1; // Index of first non-local symbol
	shdr_symtab.sh_addralign = 8;
	shdr_symtab.sh_entsize = sizeof(Elf64_Sym);
	write_value(elf_data, shdr_symtab);

	// Section: .strtab
	Elf64_Shdr shdr_strtab = {};
	shdr_strtab.sh_name = static_cast<uint32_t>(section_name_offsets[strtab_idx]);
	shdr_strtab.sh_type = 3; // SHT_STRTAB
	shdr_strtab.sh_offset = static_cast<uint64_t>(strtab_offset);
	shdr_strtab.sh_size = static_cast<uint64_t>(strtab.size());
	shdr_strtab.sh_addralign = 1;
	write_value(elf_data, shdr_strtab);

	// Section: .shstrtab
	size_t shstrtab_idx = has_globals ? 5 : 4;
	Elf64_Shdr shdr_shstrtab = {};
	shdr_shstrtab.sh_name = static_cast<uint32_t>(section_name_offsets[shstrtab_idx]);
	shdr_shstrtab.sh_type = 3; // SHT_STRTAB
	shdr_shstrtab.sh_offset = static_cast<uint64_t>(shstrtab_offset);
	shdr_shstrtab.sh_size = static_cast<uint64_t>(shstrtab.size());
	shdr_shstrtab.sh_addralign = 1;
	write_value(elf_data, shdr_shstrtab);

	return elf_data;
}

void ElfBuilder::write_elf_header(std::vector<uint8_t>& data, uint64_t entry_point) {
	// Deprecated - now handled in build()
}

std::vector<uint8_t> ElfBuilder::generate_minimal_code(const IRProgram& program) {
	// Deprecated - now using RISCVCodeGen
	return {};
}

} // namespace gdscript
