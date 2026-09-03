#include "compiler.h"
#include "chain.h"
#include "traits.h"
#include "compiler_exception.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "ir_optimizer.h"
#include "elf_builder.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

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
	m_signals.clear();
	m_rpc_configs.clear();
	m_tests.clear();
	m_class_signatures.clear();
	m_script_uses.clear();
	m_constants.clear();
	m_properties.clear();
	m_debug_variables.clear();
	m_line_table.entries.clear();
	m_installed_breakpoints.clear();
	m_class_name.clear();
	m_base_class.clear();
	m_base_is_path = false;
	m_native_base_class.clear();
	m_native_base_is_path = false;
	try {
		const bool has_executable_base = std::any_of(options.base_sources.begin(),
			options.base_sources.end(), [](const CompilerOptions::BaseSource &base) {
				return !base.trait_only;
			});
		if (has_executable_base && options.restricted) {
			throw CompilerException(ErrorType::SEMANTIC_ERROR,
				"A restricted Sandbox refuses 'extends', so no base script is compiled in",
				0, 0, "", "", "",
				"The same gate that refuses 'extends' outright refuses a base body");
		}

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

		if (!options.base_sources.empty()) {
			std::vector<ChainLink> links;
			links.reserve(options.base_sources.size() + 1);
			struct ImportedProviderTraits {
				std::unordered_set<std::string> local_names;
				std::unordered_set<std::string> qualified_names;
			};
			std::unordered_map<std::string, ImportedProviderTraits> imported_provider_traits;
			for (size_t i = options.base_sources.size(); i-- > 0;) {
				const CompilerOptions::BaseSource& base = options.base_sources[i];
				Lexer base_lexer(base.source);
				Parser base_parser(base_lexer.tokenize());
				base_parser.set_doc_comments(base_lexer.doc_comments());
				ChainLink link;
				link.name = base.name;
				link.path = base.path;
				try {
					link.program = base_parser.parse();
				} catch (CompilerException& e) {
					if (e.line() > 0 && e.source_line().empty()) {
						e.set_source_line(source_line_at(base.source, e.line()));
					}
					e.set_file(base.path);
					throw;
				}
				if (base.trait_only) {
					const size_t dot = base.name.rfind('.');
					std::string provider = base.path;
					if (provider.empty()) {
						provider = dot == std::string::npos ? base.name : base.name.substr(0, dot);
					}
					ImportedProviderTraits& imported = imported_provider_traits[provider];
					for (TraitDecl &decl : link.program.traits) {
						if (dot != std::string::npos && decl.name == base.name.substr(dot + 1)) {
							if (!imported.qualified_names.insert(base.name).second) continue;
							decl.name = base.name;
						} else if (!imported.local_names.insert(decl.name).second) {
							continue;
						}
						program.traits.push_back(std::move(decl));
					}
				} else {
					links.push_back(std::move(link));
				}
			}
			if (!links.empty()) {
				ChainLink leaf;
				leaf.path = options.source_path;
				leaf.program = std::move(program);
				links.push_back(std::move(leaf));
				std::vector<const TraitDecl*> available_traits;
				for (const ChainLink& link : links)
					for (const TraitDecl& trait : link.program.traits)
						available_traits.push_back(&trait);
				for (ChainLink& link : links) apply_traits(link.program, available_traits);
				program = merge_chain(std::move(links));
			}
		}

		if (!program.chain.merged()) apply_traits(program);

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

		// A shipping build carries no tests: they are dropped before codegen, so
		// no test body reaches the ELF. Tests call helpers, never the reverse.
		std::vector<std::string> dropped_tests;
		if (!options.emit_tests) {
			for (const FunctionDecl& decl : program.functions) {
				if (decl.is_test) {
					dropped_tests.push_back(decl.name);
				}
			}
			program.functions.erase(std::remove_if(program.functions.begin(),
				program.functions.end(), [](const FunctionDecl& decl) {
					return decl.is_test;
				}), program.functions.end());
		}

		CodeGenerator codegen;
		codegen.set_dropped_tests(dropped_tests);
		codegen.set_restricted(options.restricted);
		codegen.set_struct_checks(options.restricted ||
			options.struct_checks != CompilerOptions::StructChecks::OFF,
			options.struct_checks == CompilerOptions::StructChecks::DEEP);
		codegen.set_trait_structural_fallback(options.trait_structural_fallback);
		codegen.set_source_path(options.source_path);
		codegen.set_autoloads(options.autoloads);
		codegen.set_global_script_classes(options.global_script_classes);
		codegen.set_engine_ancestry(options.engine_ancestry);
		IRProgram ir_program = codegen.generate(program);
		m_signatures = ir_program.signatures;
		m_signals = ir_program.signals;
		m_rpc_configs = ir_program.rpc_configs;
		m_tests = ir_program.tests;
		m_class_signatures = ir_program.class_signatures;
		m_class_signatures.insert(m_class_signatures.end(),
			ir_program.trait_signatures.begin(), ir_program.trait_signatures.end());
		m_script_uses = ir_program.script_uses;
		m_constants = ir_program.constants;
		for (const IRGlobalVar &global : ir_program.globals) {
			if (global.is_const) {
				continue;
			}
			PropertySignature property;
			property.name = global.name;
			const TypeSet declared{global.declared_set};
			property.type = declared.is_nullable_single() &&
					declared.non_null().only() == Variant::OBJECT
					? int32_t(Variant::OBJECT)
					: global.value_type == IRInstruction::TypeHint_NONE
					? -1 : int32_t(global.value_type);
			property.class_name = global.class_name;
			property.hint = uint32_t(global.export_hint.hint);
			property.hint_string = global.export_hint.hint_string;
			if (global.value_type == Variant::DICTIONARY && !global.class_name.empty() &&
				property.hint_string.empty()) {
				property.hint_string = global.class_name;
			}
			property.declaration_line = global.declaration_line;
			property.is_member = global.is_member();
			property.is_static = global.is_static;
			// Godot property usage bits: STORAGE=2, EDITOR=4, SCRIPT_VARIABLE=4096.
			property.usage = global.export_hint.usage != 0
					? uint32_t(global.export_hint.usage)
					: (global.is_property ? 4102u : 4096u);
			using InitType = IRGlobalVar::InitType;
			switch (global.init_type) {
				case InitType::NONE:
				case InitType::RUNTIME:
					property.default_kind = PropertyDefaultKind::NONE;
					break;
				case InitType::NULL_VAL:
					property.default_kind = PropertyDefaultKind::NIL;
					break;
				case InitType::INT:
					property.default_kind = PropertyDefaultKind::INT;
					property.default_value = std::get<int64_t>(global.init_value);
					break;
				case InitType::FLOAT:
					property.default_kind = PropertyDefaultKind::FLOAT;
					property.default_value = std::get<double>(global.init_value);
					break;
				case InitType::BOOL:
					property.default_kind = PropertyDefaultKind::BOOL;
					property.default_value = std::get<bool>(global.init_value);
					break;
				case InitType::STRING:
					property.default_kind = PropertyDefaultKind::STRING;
					property.default_value = std::get<std::string>(global.init_value);
					break;
				case InitType::EMPTY_ARRAY:
					property.default_kind = PropertyDefaultKind::EMPTY_ARRAY;
					break;
				case InitType::EMPTY_DICT:
					property.default_kind = PropertyDefaultKind::EMPTY_DICTIONARY;
					break;
			}
			m_properties.push_back(std::move(property));
		}

		if (options.dump_ir) {
			std::cout << "=== IR (unoptimized) ===" << std::endl;
			for (const auto& func : ir_program.functions) {
				std::cout << "Function: " << func.name << std::endl;
				std::cout << "  Max registers: " << func.max_registers << std::endl;
				std::cout << "  Instructions:" << std::endl;
				for (const auto& instr : func.instructions) {
					std::cout << "    " << instr.to_string(&ir_program.strings) << std::endl;
				}
				std::cout << std::endl;
			}

			std::cout << "String constants:" << std::endl;
			for (size_t i = 0; i < ir_program.string_constants.size(); i++) {
				std::cout << "  [" << i << "] \"" << ir_program.string_constants[i] << "\"" << std::endl;
			}
			std::cout << std::endl;
		}

		// Stamp before optimizing: a debug local's live range is recorded as
		// positions in the unoptimized body, and the stamps are what survive the
		// passes that insert and delete around them.
		for (IRFunction &function : ir_program.functions) {
			for (size_t i = 0; i < function.instructions.size(); i++) {
				function.instructions[i].debug_order = uint32_t(i + 1);
			}
		}
		if (options.optimize) {
			IROptimizer optimizer;
			optimizer.optimize(ir_program);
		}
		for (IRFunction &function : ir_program.functions) {
			function.debug_locals.erase(std::remove_if(function.debug_locals.begin(),
					function.debug_locals.end(), [&](const IRFunction::DebugLocal &local) {
						return local.register_num < 0 || local.register_num >= function.max_registers;
					}), function.debug_locals.end());
			// Running maximum, so an instruction a pass synthesised (stamp 0) belongs
			// to the region it sits in rather than to the top of the function.
			std::vector<uint32_t> order(function.instructions.size());
			uint32_t running = 0;
			for (size_t i = 0; i < function.instructions.size(); i++) {
				running = std::max(running, function.instructions[i].debug_order);
				order[i] = running;
			}
			const auto position = [&order](size_t recorded) {
				const uint32_t stamp = recorded < UINT32_MAX ? uint32_t(recorded) + 1u : UINT32_MAX;
				return size_t(std::lower_bound(order.begin(), order.end(), stamp) - order.begin());
			};
			for (IRFunction::DebugLocal &local : function.debug_locals) {
				local.begin_instruction = position(local.begin_instruction);
				local.end_instruction = position(local.end_instruction);
			}
		}

		if (options.dump_ir) {
			std::cout << "=== IR (optimized) ===" << std::endl;
			for (const auto& func : ir_program.functions) {
				std::cout << "Function: " << func.name << std::endl;
				std::cout << "  Max registers: " << func.max_registers << std::endl;
				std::cout << "  Instructions:" << std::endl;
				for (const auto& instr : func.instructions) {
					std::cout << "    " << instr.to_string(&ir_program.strings) << std::endl;
				}
				std::cout << std::endl;
			}
		}

		m_is_tool = ir_program.is_tool;
		m_class_name = ir_program.class_name;
		m_base_class = ir_program.base_class;
		m_base_is_path = ir_program.base_is_path;
		m_native_base_class = ir_program.native_base_class;
		m_native_base_is_path = ir_program.native_base_is_path;

		std::vector<uint8_t> elf_data;

		if (options.output_elf) {
			ElfBuilder elf_builder;
			// Host breakpoints imply debug_info; the `breakpoint` statement does not.
			const bool debug_info = options.debug_info || !options.breakpoint_lines.empty();
			elf_data = elf_builder.build(ir_program, VariantLayout(options.double_precision),
				options.profiling, options.profiling_clock, debug_info, options.breakpoint_lines,
				options.debug_step_points);
			m_line_table = elf_builder.get_line_table();
			m_installed_breakpoints = elf_builder.get_installed_breakpoints();
			m_debug_variables = elf_builder.get_debug_variables();
		}

		m_error.clear();
		m_error_info = {};
		return elf_data;

	} catch (const CompilerException& e) {
		// Only compile() has the source text, so attach the snippet here.
		CompilerException located = e;
		if (located.line() > 0 && located.source_line().empty() && located.file().empty()) {
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
