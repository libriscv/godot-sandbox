#include "compiler.h"
#include "compiler_exception.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "ir_optimizer.h"
#include "elf_builder.h"
#include <iostream>
#include <fstream>
#include <stdexcept>

namespace gdscript {

namespace {
std::string source_line_at(const std::string& source, int line) {
	if (line <= 0) {
		return {};
	}
	size_t begin = 0;
	for (int current = 1; current < line; current++) {
		begin = source.find('\n', begin);
		if (begin == std::string::npos) {
			return {};
		}
		begin += 1;
	}
	size_t end = source.find('\n', begin);
	if (end == std::string::npos) {
		end = source.size();
	}
	// Strip trailing \r (CRLF files).
	while (end > begin && source[end - 1] == '\r') {
		end--;
	}
	return source.substr(begin, end - begin);
}
} // namespace

Compiler::Compiler() {}

std::vector<uint8_t> Compiler::compile(const std::string& source, const CompilerOptions& options) {
	m_signatures.clear();
	try {
		Lexer lexer(source);
		auto tokens = lexer.tokenize();

		if (options.dump_tokens) {
			std::cout << "=== TOKENS ===" << std::endl;
			for (const auto& token : tokens) {
				std::cout << token.to_string() << std::endl;
			}
			std::cout << std::endl;
		}

		Parser parser(tokens);
		parser.set_doc_comments(lexer.doc_comments());
		Program program = parser.parse();

		if (options.dump_ast) {
			std::cout << "=== AST ===" << std::endl;
			std::cout << "Functions: " << program.functions.size() << std::endl;
			for (const auto& func : program.functions) {
				std::cout << "  func " << func.name << "(";
				for (size_t i = 0; i < func.parameters.size(); i++) {
					if (i > 0) std::cout << ", ";
					std::cout << func.parameters[i].name;
				}
				std::cout << "): " << func.body.size() << " statements" << std::endl;
			}
			std::cout << std::endl;
		}

		CodeGenerator codegen;
		IRProgram ir_program = codegen.generate(program);
		m_signatures = ir_program.signatures;

		if (options.dump_ir) {
			std::cout << "=== IR (unoptimized) ===" << std::endl;
			for (const auto& func : ir_program.functions) {
				std::cout << "Function: " << func.name << std::endl;
				std::cout << "  Max registers: " << func.max_registers << std::endl;
				std::cout << "  Instructions:" << std::endl;
				for (const auto& instr : func.instructions) {
					std::cout << "    " << instr.to_string() << std::endl;
				}
				std::cout << std::endl;
			}

			std::cout << "String constants:" << std::endl;
			for (size_t i = 0; i < ir_program.string_constants.size(); i++) {
				std::cout << "  [" << i << "] \"" << ir_program.string_constants[i] << "\"" << std::endl;
			}
			std::cout << std::endl;
		}

		if (options.optimize) {
			IROptimizer optimizer;
			optimizer.optimize(ir_program);
		}

		if (options.dump_ir) {
			std::cout << "=== IR (optimized) ===" << std::endl;
			for (const auto& func : ir_program.functions) {
				std::cout << "Function: " << func.name << std::endl;
				std::cout << "  Max registers: " << func.max_registers << std::endl;
				std::cout << "  Instructions:" << std::endl;
				for (const auto& instr : func.instructions) {
					std::cout << "    " << instr.to_string() << std::endl;
				}
				std::cout << std::endl;
			}
		}

		std::vector<uint8_t> elf_data;

		if (options.output_elf) {
			ElfBuilder elf_builder;
			elf_data = elf_builder.build(ir_program, VariantLayout(options.double_precision));
		}

		m_error.clear();
		m_error_info = {};
		return elf_data;

	} catch (const CompilerException& e) {
		// Only compile() has the source text, so attach the snippet here.
		CompilerException located = e;
		if (located.line() > 0 && located.source_line().empty()) {
			located.set_source_line(source_line_at(source, located.line()));
		}
		m_error = located.what();
		m_error_info = CompilerError{ true, located.error_type(), located.message(),
			located.line(), located.column(), located.function(), located.hint() };
		return {};
	} catch (const std::exception& e) {
		m_error = e.what();
		m_error_info = CompilerError{};
		m_error_info.has_error = true;
		m_error_info.message = e.what();
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
