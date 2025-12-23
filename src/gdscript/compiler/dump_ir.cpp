#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "ir_optimizer.h"
#include <iostream>
#include <variant>
#include <iomanip>
#include <limits>

using namespace gdscript;

// Helper to convert TypeHint to string
const char* type_hint_name(IRInstruction::TypeHint hint) {
	switch (hint) {
		case IRInstruction::TypeHint::NONE: return "NONE";
		case IRInstruction::TypeHint::RAW_INT: return "RAW_INT";
		case IRInstruction::TypeHint::RAW_BOOL: return "RAW_BOOL";
		case IRInstruction::TypeHint::VARIANT_INT: return "VARIANT_INT";
		case IRInstruction::TypeHint::VARIANT_FLOAT: return "VARIANT_FLOAT";
		case IRInstruction::TypeHint::VARIANT_BOOL: return "VARIANT_BOOL";
		case IRInstruction::TypeHint::VARIANT_VECTOR2: return "VARIANT_VECTOR2";
		case IRInstruction::TypeHint::VARIANT_VECTOR3: return "VARIANT_VECTOR3";
		case IRInstruction::TypeHint::VARIANT_VECTOR4: return "VARIANT_VECTOR4";
		case IRInstruction::TypeHint::VARIANT_VECTOR2I: return "VARIANT_VECTOR2I";
		case IRInstruction::TypeHint::VARIANT_VECTOR3I: return "VARIANT_VECTOR3I";
		case IRInstruction::TypeHint::VARIANT_VECTOR4I: return "VARIANT_VECTOR4I";
		case IRInstruction::TypeHint::VARIANT_COLOR: return "VARIANT_COLOR";
		case IRInstruction::TypeHint::VARIANT_RECT2: return "VARIANT_RECT2";
		case IRInstruction::TypeHint::VARIANT_RECT2I: return "VARIANT_RECT2I";
		case IRInstruction::TypeHint::VARIANT_PLANE: return "VARIANT_PLANE";
		default: return "UNKNOWN";
	}
}

// Helper to format operand with detailed type info
std::string format_operand_detailed(const IRValue& op) {
	std::ostringstream oss;

	switch (op.type) {
		case IRValue::Type::REGISTER:
			oss << "r" << std::get<int>(op.value);
			break;
		case IRValue::Type::IMMEDIATE: {
			int64_t val = std::get<int64_t>(op.value);
			oss << val << " (0x" << std::hex << val << std::dec << ")";
			break;
		}
		case IRValue::Type::FLOAT: {
			double val = std::get<double>(op.value);
			oss << std::setprecision(std::numeric_limits<double>::max_digits10) << val;
			break;
		}
		case IRValue::Type::LABEL:
			oss << "@" << std::get<std::string>(op.value);
			break;
		case IRValue::Type::VARIABLE:
			oss << "$" << std::get<std::string>(op.value);
			break;
		case IRValue::Type::STRING:
			oss << "\"" << std::get<std::string>(op.value) << "\"";
			break;
	}

	return oss.str();
}

int main(int argc, char** argv)
{
	std::string source;
	bool verbose = false;
	bool no_optimize = false;

	// Parse arguments
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-v" || arg == "--verbose") {
			verbose = true;
		} else if (arg == "--no-opt" || arg == "--no-optimize") {
			no_optimize = true;
		} else if (source.empty()) {
			source = arg;
		}
	}

	if (source.empty()) {
		// Read from stdin
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
		Program program = parser.parse();

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
		IRProgram ir = codegen.generate(program);

		// Apply optimizations unless disabled
		if (!no_optimize) {
			IROptimizer optimizer;
			optimizer.optimize(ir);
		}

		// Print string constants
		if (!ir.string_constants.empty()) {
			std::cout << "=== STRING CONSTANTS ===" << std::endl;
			for (size_t i = 0; i < ir.string_constants.size(); i++) {
				std::cout << "  [" << i << "] \"" << ir.string_constants[i] << "\"" << std::endl;
			}
			std::cout << std::endl;
		}

		// Print functions
		for (const auto& func : ir.functions) {
			std::cout << "=== Function: " << func.name << "(";
			for (size_t i = 0; i < func.parameters.size(); i++) {
				if (i > 0) std::cout << ", ";
				std::cout << func.parameters[i];
			}
			std::cout << ") ===" << std::endl;
			std::cout << "max_registers: " << func.max_registers << std::endl;
			std::cout << std::endl;

			for (size_t i = 0; i < func.instructions.size(); i++) {
				const auto& instr = func.instructions[i];

				// Format instruction index
				std::cout << std::setw(4) << i << ": ";

				// Format opcode
				std::cout << std::setw(20) << std::left << ir_opcode_name(instr.opcode);

				// Format operands
				for (size_t j = 0; j < instr.operands.size(); j++) {
					if (j > 0) std::cout << ", ";
					if (verbose) {
						std::cout << format_operand_detailed(instr.operands[j]);
					} else {
						std::cout << instr.operands[j].to_string();
					}
				}

				// Show type hint if present
				if (instr.type_hint != IRInstruction::TypeHint::NONE) {
					std::cout << "  [type: " << type_hint_name(instr.type_hint) << "]";
				}

				// Add semantic comments for certain instructions
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
