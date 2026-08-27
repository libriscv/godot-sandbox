#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "ir_optimizer.h"
#include "riscv_codegen.h"
#include "chain.h"
#include <fstream>
#include <iostream>
#include <variant>
#include <iomanip>
#include <limits>
#include <sstream>
#include <set>

using namespace gdscript;

const char* reg_name(uint8_t reg) {
	static const char* names[] = {
		"x0", "ra", "sp", "gp", "tp",
		"t0", "t1", "t2", "fp", "s1",
		"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
		"s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11",
		"t3", "t4", "t5", "t6"
	};
	if (reg < 32) return names[reg];
	return "?";
}

static const char* type_hint_name(IRInstruction::TypeHint hint) { return variant_type_name(hint); }

std::string format_operand_detailed(const IRValue& op, const IRStringTable& strings) {
	std::ostringstream oss;

	switch (op.type) {
		case IRValue::Type::REGISTER:
			oss << "r" << op.reg_index();
			break;
		case IRValue::Type::IMMEDIATE: {
			int64_t val = op.immediate();
			oss << val << " (0x" << std::hex << val << std::dec << ")";
			break;
		}
		case IRValue::Type::FLOAT: {
			double val = op.float_number();
			oss << std::setprecision(std::numeric_limits<double>::max_digits10) << val;
			break;
		}
		case IRValue::Type::LABEL:
			oss << "@" << strings[op.string_id];
			break;
		case IRValue::Type::VARIABLE:
			oss << "$" << strings[op.string_id];
			break;
		case IRValue::Type::STRING:
			oss << "\"" << strings[op.string_id] << "\"";
			break;
	}

	return oss.str();
}

int main(int argc, char** argv)
{
	std::string source;
	bool verbose = false;
	bool no_optimize = false;
	bool show_codegen = false;
	bool double_precision = native_variant_layout().double_precision;
	std::vector<std::string> autoloads;
	std::vector<std::pair<std::string, std::string>> global_classes;
	std::vector<std::pair<std::string, std::string>> base_specs;

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-v" || arg == "--verbose") {
			verbose = true;
		} else if (arg == "--no-opt" || arg == "--no-optimize") {
			no_optimize = true;
		} else if (arg == "--codegen" || arg == "-c") {
			show_codegen = true;
		} else if (arg == "--autoload") {
			if (i + 1 < argc) {
				autoloads.push_back(argv[++i]);
			}
		} else if (arg == "--global-class") {
			if (i + 1 < argc) {
				const std::string pair = argv[++i];
				const size_t eq = pair.find('=');
				if (eq != std::string::npos) {
					global_classes.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
				}
			}
		} else if (arg == "--base") {
			if (i + 1 < argc) {
				const std::string pair = argv[++i];
				const size_t eq = pair.find('=');
				if (eq == std::string::npos) {
					std::cerr << "Error: --base wants Name=path" << std::endl;
					return 1;
				}
				base_specs.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
			}
		} else if (arg == "--double-precision") {
			double_precision = true;
		} else if (arg == "--single-precision") {
			double_precision = false;
		} else if (source.empty()) {
			source = arg;
		}
	}

	if (source.empty()) {
		std::string line;
		while (std::getline(std::cin, line)) {
			source += line + "\n";
		}
	}

	try {
		Lexer lexer(source);
		auto tokens = lexer.tokenize();

		if (verbose) {
			std::cout << "=== TOKENS ===" << std::endl;
			for (const auto& tok : tokens) {
				std::cout << "  " << tok.lexeme << " [type=" << token_type_name(tok.type) << "]" << std::endl;
			}
			std::cout << std::endl;
		}

		Parser parser(tokens);
		parser.set_doc_comments(lexer.doc_comments());
		Program program = parser.parse();

		if (!base_specs.empty()) {
			std::vector<ChainLink> links;
			links.reserve(base_specs.size() + 1);
			for (size_t i = base_specs.size(); i-- > 0;) {
				std::ifstream in(base_specs[i].second);
				if (!in) {
					std::cerr << "Error: cannot read base script " << base_specs[i].second << std::endl;
					return 1;
				}
				const std::string base_source((std::istreambuf_iterator<char>(in)),
					std::istreambuf_iterator<char>());
				Lexer base_lexer(base_source);
				Parser base_parser(base_lexer.tokenize());
				base_parser.set_doc_comments(base_lexer.doc_comments());
				ChainLink link;
				link.name = base_specs[i].first;
				link.path = base_specs[i].second;
				link.program = base_parser.parse();
				links.push_back(std::move(link));
			}
			ChainLink leaf;
			leaf.program = std::move(program);
			links.push_back(std::move(leaf));
			program = merge_chain(std::move(links));
		}

		if (verbose) {
			std::cout << "=== AST ===" << std::endl;
			std::cout << "  Functions: " << program.functions.size() << std::endl;
			for (const auto& func : program.functions) {
				std::cout << "    " << func.name << "(";
				for (size_t i = 0; i < func.parameters.size(); i++) {
					if (i > 0) std::cout << ", ";
					std::cout << func.parameters[i].name;
				}
				std::cout << ") - " << func.body.size() << " statement(s)" << std::endl;
			}
			std::cout << std::endl;
		}

		CodeGenerator codegen;
		codegen.set_autoloads(autoloads);
		codegen.set_global_script_classes(global_classes);
		IRProgram ir = codegen.generate(program);

		if (!no_optimize) {
			IROptimizer optimizer;
			optimizer.optimize(ir);
		}

		if (!ir.globals.empty()) {
			std::cout << "=== GLOBAL VARIABLES ===" << std::endl;
			for (size_t i = 0; i < ir.globals.size(); i++) {
				const auto& global = ir.globals[i];
				std::cout << "  [" << i << "] " << global.name;

				if (global.is_property) {
					std::cout << " (@export";
					if (!global.export_hint.is_default()) {
						std::cout << " hint=" << global.export_hint.hint;
						if (!global.export_hint.hint_string.empty()) {
							std::cout << " \"" << global.export_hint.hint_string << "\"";
						}
						if (global.export_hint.usage != 0) {
							std::cout << " usage=" << global.export_hint.usage;
						}
					}
					std::cout << ")";
				}

				if (global.is_const) {
					std::cout << " (const)";
				}

				if (global.type_hint != IRInstruction::TypeHint_NONE) {
					std::cout << ": " << type_hint_name(global.type_hint);
				}

				if (global.init_type != IRGlobalVar::InitType::NONE) {
					std::cout << " = ";
					switch (global.init_type) {
						case IRGlobalVar::InitType::INT:
							std::cout << std::get<int64_t>(global.init_value);
							break;
						case IRGlobalVar::InitType::FLOAT:
							std::cout << std::get<double>(global.init_value);
							break;
						case IRGlobalVar::InitType::STRING:
							std::cout << "\"" << std::get<std::string>(global.init_value) << "\"";
							break;
						case IRGlobalVar::InitType::BOOL:
							std::cout << (std::get<bool>(global.init_value) ? "true" : "false");
							break;
						case IRGlobalVar::InitType::EMPTY_ARRAY:
							std::cout << "[]";
							break;
						case IRGlobalVar::InitType::EMPTY_DICT:
							std::cout << "{}";
							break;
						case IRGlobalVar::InitType::NULL_VAL:
							std::cout << "null";
							break;
						case IRGlobalVar::InitType::RUNTIME:
							std::cout << "<evaluated by "
								<< (global.is_member() ? ir.member_init.name : ir.global_init.name)
								<< ">";
							break;
						default:
							std::cout << "?";
							break;
					}
				}

				std::cout << std::endl;
			}
			std::cout << std::endl;
		}

		if (!ir.string_constants.empty()) {
			std::cout << "=== STRING CONSTANTS ===" << std::endl;
			for (size_t i = 0; i < ir.string_constants.size(); i++) {
				std::cout << "  [" << i << "] \"" << ir.string_constants[i] << "\"" << std::endl;
			}
			std::cout << std::endl;
		}

		std::vector<const IRFunction*> all_functions;
		if (ir.has_member_init) {
			all_functions.push_back(&ir.member_init);
		}
		if (ir.has_global_init) {
			all_functions.push_back(&ir.global_init);
		}
		for (const auto& func : ir.functions) {
			all_functions.push_back(&func);
		}
		for (const IRFunction* func_ptr : all_functions) {
			const IRFunction& func = *func_ptr;
			std::cout << "=== Function: " << func.name << "(";
			for (size_t i = 0; i < func.parameters.size(); i++) {
				if (i > 0) std::cout << ", ";
				std::cout << func.parameters[i];
			}
			std::cout << ") ===" << std::endl;
			std::cout << "max_registers: " << func.max_registers << std::endl;
			{
				const VariantLayout layout(double_precision);
				std::cout << "real_t: " << (layout.double_precision ? "double" : "float")
						  << " (sizeof(Variant) = " << layout.variant_size() << ", slot stride = "
						  << layout.variant_size() << " bytes)" << std::endl;
			}
			std::cout << std::endl;

			RegisterAllocator allocator;
			allocator.init(func);

			enum class ValueType {
				UNKNOWN,
				VARIANT
			};
			std::unordered_map<int, ValueType> vreg_types;
			std::unordered_map<int, int> variant_offsets;
			int next_variant_slot = func.parameters.size();

			auto value_type_name = [](ValueType t) -> const char* {
				switch (t) {
					case ValueType::VARIANT: return "VARIANT";
					default: return "UNKNOWN";
				}
			};

			for (size_t i = 0; i < func.instructions.size(); i++) {
				const auto& instr = func.instructions[i];

				std::set<int> used_vregs;
				for (const auto& op : instr.operands) {
					if (op.type == IRValue::Type::REGISTER) {
						used_vregs.insert(op.reg_index());
					}
				}

				ValueType result_type = ValueType::UNKNOWN;

				switch (instr.opcode) {
					case IROpcode::LOAD_IMM:
					case IROpcode::LOAD_FLOAT_IMM:
					case IROpcode::LOAD_BOOL:
					case IROpcode::LOAD_STRING:
					case IROpcode::ARRAY_APPEND:
					case IROpcode::ARRAY_GET:
					case IROpcode::CALL_SYSCALL:
					case IROpcode::VCALL:
					case IROpcode::VGET:
					case IROpcode::VSET:
					case IROpcode::MAKE_ARRAY:
					case IROpcode::MAKE_PACKED_BYTE_ARRAY:
					case IROpcode::MAKE_PACKED_INT32_ARRAY:
					case IROpcode::MAKE_PACKED_INT64_ARRAY:
					case IROpcode::MAKE_PACKED_FLOAT32_ARRAY:
					case IROpcode::MAKE_PACKED_FLOAT64_ARRAY:
					case IROpcode::MAKE_PACKED_STRING_ARRAY:
					case IROpcode::MAKE_PACKED_VECTOR2_ARRAY:
					case IROpcode::MAKE_PACKED_VECTOR3_ARRAY:
					case IROpcode::MAKE_PACKED_COLOR_ARRAY:
					case IROpcode::MAKE_PACKED_VECTOR4_ARRAY:
					case IROpcode::MAKE_DICTIONARY:
					case IROpcode::ADD:
					case IROpcode::SUB:
					case IROpcode::MUL:
					case IROpcode::DIV:
					case IROpcode::MOD:
						result_type = ValueType::VARIANT;
						break;
					default:
						break;
				}

				if (result_type != ValueType::UNKNOWN && !instr.operands.empty() &&
				    instr.operands[0].type == IRValue::Type::REGISTER) {
					int result_vreg = instr.operands[0].reg_index();
					vreg_types[result_vreg] = result_type;
					if (result_type == ValueType::VARIANT) {
						variant_offsets[result_vreg] = next_variant_slot++;
					}
				}

				std::cout << std::setw(4) << i << ": ";
				std::cout << std::setw(20) << std::left << ir_opcode_name(instr.opcode);
				for (size_t j = 0; j < instr.operands.size(); j++) {
					if (j > 0) std::cout << ", ";
					if (instr.opcode == IROpcode::GLOBAL_CALL && j == 1 &&
						instr.operands[j].type == IRValue::Type::IMMEDIATE) {
						// The GlobalFn, by name rather than by number.
						std::cout << global_function(
							static_cast<GlobalFn>(instr.operands[j].immediate())).name;
					} else if (verbose) {
						std::cout << format_operand_detailed(instr.operands[j], ir.strings);
					} else {
						std::cout << instr.operands[j].to_string(&ir.strings);
					}

					if (show_codegen && instr.operands[j].type == IRValue::Type::REGISTER) {
						int vreg = instr.operands[j].reg_index();
						int preg = allocator.allocate_register(vreg, i);

						std::cout << "(";
						if (preg >= 0) {
							std::cout << reg_name(preg);
						} else {
							std::cout << "spilled";
						}
						std::cout << ")";

						if (vreg_types.count(vreg)) {
							std::cout << "[" << value_type_name(vreg_types[vreg]) << "]";
							if (vreg_types[vreg] == ValueType::VARIANT && variant_offsets.count(vreg)) {
								std::cout << "[slot" << variant_offsets[vreg] << "]";
							}
						}
					}
				}

				if (instr.type_hint != IRInstruction::TypeHint_NONE) {
					std::cout << "  [type: " << type_hint_name(instr.type_hint) << "]";
				}

				if (!verbose) {
					switch (instr.opcode) {
						case IROpcode::MAKE_VECTOR2:
						case IROpcode::MAKE_VECTOR3:
						case IROpcode::MAKE_VECTOR4:
						case IROpcode::MAKE_VECTOR2I:
						case IROpcode::MAKE_VECTOR3I:
						case IROpcode::MAKE_VECTOR4I:
						case IROpcode::MAKE_COLOR:
							std::cout << "  ; Inline construction";
							break;
						case IROpcode::VGET_INLINE:
							if (instr.operands.size() >= 3) {
								std::cout << "  ; Get inline member";
							}
							break;
						case IROpcode::VSET_INLINE:
							if (instr.operands.size() >= 3) {
								std::cout << "  ; Set inline member";
							}
							break;
						case IROpcode::VGET:
							if (instr.operands.size() >= 3) {
								std::cout << "  ; Property get (sugar for obj.get)";
							}
							break;
						case IROpcode::VSET:
							if (instr.operands.size() >= 3) {
								std::cout << "  ; Property set (sugar for obj.set)";
							}
							break;
						default:
							break;
					}
				}

				std::cout << std::endl;
			}
			std::cout << std::endl;
		}

		return 0;
	} catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}
}
