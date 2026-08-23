#include "elf_builder.h"
#include "riscv_codegen.h"
#include <cstring>
#include <stdexcept>

namespace gdscript {

ElfBuilder::ElfBuilder() {}

std::vector<uint8_t> ElfBuilder::build(const IRProgram& program, const VariantLayout& layout,
	bool profiling, ProfilingClock profiling_clock, bool debug_info,
	const std::vector<uint32_t>& breakpoint_lines) {
	RISCVCodeGen codegen(layout, profiling, profiling_clock, debug_info, breakpoint_lines);
	std::vector<uint8_t> code = codegen.generate(program);

	// Rebase addresses from .text-relative to virtual.
	m_line_table = codegen.get_line_table();
	m_installed_breakpoints = codegen.get_installed_breakpoints();
	for (LineTableEntry& entry : m_line_table.entries) {
		entry.address += uint32_t(BASE_ADDR);
	}
	auto func_offsets = codegen.get_function_offsets();
	auto const_pool = codegen.get_constant_pool();
	auto global_data_size = codegen.get_global_data_size();
	const uint64_t profiling_address = codegen.get_profiling_address();
	const uint64_t profiling_size = codegen.get_profiling_size();
	const uint64_t debug_address = codegen.get_debug_address();
	const uint64_t debug_size = codegen.get_debug_size();

	std::vector<uint8_t> elf_data;

	size_t ehdr_size = sizeof(Elf64_Ehdr);
	size_t phdr_size = sizeof(Elf64_Phdr);

	// Two PT_LOAD segments when globals exist: .text (R+X) and .data (R+W).
	bool has_globals = global_data_size > 0;
	size_t num_phdrs = has_globals ? 2 : 1;
	size_t num_sections = has_globals ? 6 : 5;

	size_t text_size = code.size() - global_data_size;
	size_t data_size = global_data_size;

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

	std::vector<uint8_t> strtab;
	strtab.push_back(0);

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

	size_t profiling_name_offset = 0;
	if (profiling_size > 0) {
		profiling_name_offset = strtab.size();
		const std::string name = PROFILING_SYMBOL;
		strtab.insert(strtab.end(), name.begin(), name.end());
		strtab.push_back(0);
	}

	size_t debug_name_offset = 0;
	if (debug_size > 0) {
		debug_name_offset = strtab.size();
		const std::string name = DEBUG_SYMBOL;
		strtab.insert(strtab.end(), name.begin(), name.end());
		strtab.push_back(0);
	}

	// Defined locally to avoid alignment/packing issues.
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

	Elf64_Sym null_sym = {};
	memset(&null_sym, 0, sizeof(null_sym));
	symtab.push_back(null_sym);
	for (size_t i = 0; i < program.functions.size(); i++) {
		const auto& func = program.functions[i];
		size_t func_offset = func_offsets.at(func.name);

		size_t func_size = text_size - func_offset;
		if (i + 1 < program.functions.size()) {
			const auto& next_func = program.functions[i + 1];
			size_t next_offset = func_offsets.at(next_func.name);
			func_size = next_offset - func_offset;
		}

		Elf64_Sym sym = {};
		memset(&sym, 0, sizeof(sym));
		sym.st_name = static_cast<uint32_t>(symbol_name_offsets[i]);
		sym.st_info = (1 << 4) | 2; // STB_GLOBAL (1) << 4 | STT_FUNC (2)
		sym.st_other = 0;
		sym.st_shndx = 1; // .text section
		sym.st_value = BASE_ADDR + func_offset; // Actual function address
		sym.st_size = func_size;
		symtab.push_back(sym);
	}

	if (profiling_size > 0) {
		Elf64_Sym sym = {};
		memset(&sym, 0, sizeof(sym));
		sym.st_name = static_cast<uint32_t>(profiling_name_offset);
		sym.st_info = (1 << 4) | 1; // STB_GLOBAL | STT_OBJECT
		sym.st_other = 0;
		sym.st_shndx = 2; // .data
		sym.st_value = profiling_address;
		sym.st_size = profiling_size;
		symtab.push_back(sym);
	}

	if (debug_size > 0) {
		Elf64_Sym sym = {};
		memset(&sym, 0, sizeof(sym));
		sym.st_name = static_cast<uint32_t>(debug_name_offset);
		sym.st_info = (1 << 4) | 1; // STB_GLOBAL | STT_OBJECT
		sym.st_other = 0;
		sym.st_shndx = 2; // .data
		sym.st_value = debug_address;
		sym.st_size = debug_size;
		symtab.push_back(sym);
	}

	size_t symtab_size = symtab.size() * sizeof(Elf64_Sym);

	size_t offset = 0;
	offset += ehdr_size;
	size_t phdr_offset = offset;
	offset += num_phdrs * phdr_size;

	offset = (offset + 0xFFF) & ~0xFFF;
	size_t text_offset = offset;
	offset += text_size;

	size_t data_offset = 0;
	if (has_globals) {
		offset = (offset + 0xFFF) & ~0xFFF;
		data_offset = offset;
		offset += data_size;
	}

	offset = (offset + 7) & ~7;
	size_t symtab_offset = offset;
	offset += symtab_size;

	size_t strtab_offset = offset;
	offset += strtab.size();

	size_t shstrtab_offset = offset;
	offset += shstrtab.size();

	offset = (offset + 7) & ~7;
	size_t shdr_offset = offset;
	Elf64_Ehdr ehdr;
	memset(&ehdr, 0, sizeof(ehdr));

	ehdr.e_ident[0] = 0x7f;
	ehdr.e_ident[1] = 'E';
	ehdr.e_ident[2] = 'L';
	ehdr.e_ident[3] = 'F';
	ehdr.e_ident[4] = 2;
	ehdr.e_ident[5] = 1;
	ehdr.e_ident[6] = 1;
	ehdr.e_ident[7] = 0;

	ehdr.e_type = ET_EXEC;
	ehdr.e_machine = EM_RISCV;
	ehdr.e_version = EV_CURRENT;
	ehdr.e_entry = BASE_ADDR;
	ehdr.e_phoff = phdr_offset;
	ehdr.e_shoff = shdr_offset;
	ehdr.e_flags = 0x5;
	ehdr.e_ehsize = sizeof(Elf64_Ehdr);
	ehdr.e_phentsize = sizeof(Elf64_Phdr);
	ehdr.e_phnum = static_cast<uint16_t>(num_phdrs);
	ehdr.e_shentsize = sizeof(Elf64_Shdr);
	ehdr.e_shnum = static_cast<uint16_t>(num_sections);
	ehdr.e_shstrndx = has_globals ? 5 : 4;

	write_value(elf_data, ehdr);

	Elf64_Phdr phdr_text;
	memset(&phdr_text, 0, sizeof(phdr_text));

	phdr_text.p_type = 1;
	phdr_text.p_flags = 5;
	phdr_text.p_offset = static_cast<uint64_t>(text_offset);
	phdr_text.p_vaddr = BASE_ADDR;
	phdr_text.p_paddr = BASE_ADDR;
	phdr_text.p_filesz = static_cast<uint64_t>(text_size);
	phdr_text.p_memsz = static_cast<uint64_t>(text_size);
	phdr_text.p_align = 0x1000;

	write_value(elf_data, phdr_text);

	if (has_globals) {
		uint64_t data_vaddr = BASE_ADDR + text_size;
		data_vaddr = (data_vaddr + 0xFFF) & ~0xFFFULL;

		Elf64_Phdr phdr_data;
		memset(&phdr_data, 0, sizeof(phdr_data));

		phdr_data.p_type = 1;
		phdr_data.p_flags = 6;
		phdr_data.p_offset = static_cast<uint64_t>(data_offset);
		phdr_data.p_vaddr = data_vaddr;
		phdr_data.p_paddr = data_vaddr;
		phdr_data.p_filesz = static_cast<uint64_t>(data_size);
		phdr_data.p_memsz = static_cast<uint64_t>(data_size);
		phdr_data.p_align = 0x1000;

		write_value(elf_data, phdr_data);
	}

	while (elf_data.size() < text_offset) {
		elf_data.push_back(0);
	}

	elf_data.insert(elf_data.end(), code.begin(), code.begin() + text_size);

	if (has_globals) {
		while (elf_data.size() < data_offset) {
			elf_data.push_back(0);
		}
		elf_data.insert(elf_data.end(), code.begin() + text_size, code.end());
	}

	while (elf_data.size() < symtab_offset) {
		elf_data.push_back(0);
	}

	for (const auto& sym : symtab) {
		write_value(elf_data, sym);
	}

	elf_data.insert(elf_data.end(), strtab.begin(), strtab.end());
	elf_data.insert(elf_data.end(), shstrtab.begin(), shstrtab.end());

	while (elf_data.size() < shdr_offset) {
		elf_data.push_back(0);
	}

	Elf64_Shdr shdr_null = {};
	write_value(elf_data, shdr_null);

	Elf64_Shdr shdr_text = {};
	shdr_text.sh_name = static_cast<uint32_t>(section_name_offsets[1]);
	shdr_text.sh_type = 1;
	shdr_text.sh_flags = 6;
	shdr_text.sh_addr = BASE_ADDR;
	shdr_text.sh_offset = static_cast<uint64_t>(text_offset);
	shdr_text.sh_size = static_cast<uint64_t>(text_size);
	shdr_text.sh_addralign = 4;
	write_value(elf_data, shdr_text);

	if (has_globals) {
		uint64_t data_vaddr = BASE_ADDR + text_size;
		data_vaddr = (data_vaddr + 0xFFF) & ~0xFFFULL;

		Elf64_Shdr shdr_data = {};
		shdr_data.sh_name = static_cast<uint32_t>(section_name_offsets[2]);
		shdr_data.sh_type = 1;
		shdr_data.sh_flags = 3;
		shdr_data.sh_addr = data_vaddr;
		shdr_data.sh_offset = static_cast<uint64_t>(data_offset);
		shdr_data.sh_size = static_cast<uint64_t>(data_size);
		shdr_data.sh_addralign = 8;
		write_value(elf_data, shdr_data);
	}

	size_t symtab_idx = has_globals ? 3 : 2;
	size_t strtab_idx = has_globals ? 4 : 3;
	Elf64_Shdr shdr_symtab = {};
	shdr_symtab.sh_name = static_cast<uint32_t>(section_name_offsets[symtab_idx]);
	shdr_symtab.sh_type = 2;
	shdr_symtab.sh_offset = static_cast<uint64_t>(symtab_offset);
	shdr_symtab.sh_size = static_cast<uint64_t>(symtab_size);
	shdr_symtab.sh_link = static_cast<uint32_t>(strtab_idx);
	shdr_symtab.sh_info = 1;
	shdr_symtab.sh_addralign = 8;
	shdr_symtab.sh_entsize = sizeof(Elf64_Sym);
	write_value(elf_data, shdr_symtab);

	Elf64_Shdr shdr_strtab = {};
	shdr_strtab.sh_name = static_cast<uint32_t>(section_name_offsets[strtab_idx]);
	shdr_strtab.sh_type = 3;
	shdr_strtab.sh_offset = static_cast<uint64_t>(strtab_offset);
	shdr_strtab.sh_size = static_cast<uint64_t>(strtab.size());
	shdr_strtab.sh_addralign = 1;
	write_value(elf_data, shdr_strtab);

	size_t shstrtab_idx = has_globals ? 5 : 4;
	Elf64_Shdr shdr_shstrtab = {};
	shdr_shstrtab.sh_name = static_cast<uint32_t>(section_name_offsets[shstrtab_idx]);
	shdr_shstrtab.sh_type = 3;
	shdr_shstrtab.sh_offset = static_cast<uint64_t>(shstrtab_offset);
	shdr_shstrtab.sh_size = static_cast<uint64_t>(shstrtab.size());
	shdr_shstrtab.sh_addralign = 1;
	write_value(elf_data, shdr_shstrtab);

	return elf_data;
}

void ElfBuilder::write_elf_header(std::vector<uint8_t>& data, uint64_t entry_point) {
}

std::vector<uint8_t> ElfBuilder::generate_minimal_code(const IRProgram& program) {
	return {};
}

} // namespace gdscript
