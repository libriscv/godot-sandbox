#include "compiler.h"
#include "elf_builder.h"
#include <fstream>

namespace gdscript {

std::vector<uint8_t> Compiler::compile(const std::string& source, const CompilerOptions& options) {
	auto ir = compile_to_ir(source, options);
	if (!ir) return {};
	try {
		std::vector<uint8_t> elf_data;

		if (options.output_elf) {
			ElfBuilder elf_builder;
			// Host breakpoints imply debug_info; the `breakpoint` statement does not.
			const bool debug_info = options.debug_info || !options.breakpoint_lines.empty();
			elf_data = elf_builder.build(*ir, VariantLayout(options.double_precision),
				options.profiling, options.profiling_clock, debug_info, options.breakpoint_lines,
				options.debug_step_points);
			m_line_table = elf_builder.get_line_table();
			m_installed_breakpoints = elf_builder.get_installed_breakpoints();
			m_debug_variables = elf_builder.get_debug_variables();
		}

		return elf_data;
	} catch (const CompilerException& e) {
		set_error(source, e);
		return {};
	} catch (const std::exception& e) {
		set_error(e);
		return {};
	}
}

bool Compiler::compile_to_file(const std::string& source, const std::string& output_path, const CompilerOptions& options) {
	auto elf_data = compile(source, options);

	if (elf_data.empty()) {
		return false;
	}

	std::ofstream out(output_path, std::ios::binary);
	if (!out) {
		m_error = "Failed to open output file: " + output_path;
		m_error_info = CompilerError{};
		m_error_info.has_error = true;
		m_error_info.type = ErrorType::ELF_ERROR;
		m_error_info.message = m_error;
		return false;
	}

	out.write(reinterpret_cast<const char*>(elf_data.data()), elf_data.size());
	out.close();

	return out.good();
}

} // namespace gdscript
