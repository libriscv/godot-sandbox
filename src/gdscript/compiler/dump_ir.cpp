#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "ir_optimizer.h"
#include "riscv_codegen.h"
#include <iostream>
#include <variant>
#include <iomanip>
#include <limits>
#include <sstream>
#include <set>

using namespace gdscript;

// RISC-V register names
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

// Helper to convert TypeHint to string
const char* type_hint_name(IRInstruction::TypeHint hint) {
	if (hint == IRInstruction::TypeHint_NONE) {
		return "NONE";
	}
	// Use Variant::Type enum values directly
	switch (hint) {
		case Variant::NIL: return "NIL";
		case Variant::BOOL: return "BOOL";
		case Variant::INT: return "INT";
		case Variant::FLOAT: return "FLOAT";
		case Variant::STRING: return "STRING";
		case Variant::VECTOR2: return "VECTOR2";
		case Variant::VECTOR2I: return "VECTOR2I";
		case Variant::VECTOR3: return "VECTOR3";
		case Variant::VECTOR3I: return "VECTOR3I";
		case Variant::VECTOR4: return "VECTOR4";
		case Variant::VECTOR4I: return "VECTOR4I";
		case Variant::COLOR: return "COLOR";
		case Variant::RECT2: return "RECT2";
		case Variant::RECT2I: return "RECT2I";
		case Variant::PLANE: return "PLANE";
		case Variant::ARRAY: return "ARRAY";
		case Variant::DICTIONARY: return "DICTIONARY";
		case Variant::PACKED_BYTE_ARRAY: return "PACKED_BYTE_ARRAY";
		case Variant::PACKED_INT32_ARRAY: return "PACKED_INT32_ARRAY";
		case Variant::PACKED_INT64_ARRAY: return "PACKED_INT64_ARRAY";
		case Variant::PACKED_FLOAT32_ARRAY: return "PACKED_FLOAT32_ARRAY";
		case Variant::PACKED_FLOAT64_ARRAY: return "PACKED_FLOAT64_ARRAY";
		case Variant::PACKED_STRING_ARRAY: return "PACKED_STRING_ARRAY";
		case Variant::PACKED_VECTOR2_ARRAY: return "PACKED_VECTOR2_ARRAY";
		case Variant::PACKED_VECTOR3_ARRAY: return "PACKED_VECTOR3_ARRAY";
		case Variant::PACKED_COLOR_ARRAY: return "PACKED_COLOR_ARRAY";
		case Variant::PACKED_VECTOR4_ARRAY: return "PACKED_VECTOR4_ARRAY";
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
	bool show_codegen = false;

	// Parse arguments
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-v" || arg == "--verbose") {
			verbose = true;
		} else if (arg == "--no-opt" || arg == "--no-optimize") {
			no_optimize = true;
		} else if (arg == "--codegen" || arg == "-c") {
			show_codegen = true;
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

			// Initialize register allocator to simulate codegen
			RegisterAllocator allocator;
			allocator.init(func);

			// Track value types like codegen does
			enum class ValueType {
				UNKNOWN,
				VARIANT
			};
			std::unordered_map<int, ValueType> vreg_types;
			std::unordered_map<int, int> variant_offsets;
			int next_variant_slot = func.parameters.size();

			// Helper to get value type name
			auto value_type_name = [](ValueType t) -> const char* {
				switch (t) {
					case ValueType::VARIANT: return "VARIANT";
					default: return "UNKNOWN";
				}
			};

			// Simulate type inference from instructions
			for (size_t i = 0; i < func.instructions.size(); i++) {
				const auto& instr = func.instructions[i];

				// Collect all vregs used in this instruction
				std::set<int> used_vregs;
				for (const auto& op : instr.operands) {
					if (op.type == IRValue::Type::REGISTER) {
						used_vregs.insert(std::get<int>(op.value));
					}
				}

				// Infer types from instruction
				ValueType result_type = ValueType::UNKNOWN;

				switch (instr.opcode) {
					case IROpcode::LOAD_IMM:
					case IROpcode::LOAD_FLOAT_IMM:
					case IROpcode::LOAD_BOOL:
					case IROpcode::LOAD_STRING:
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

				// Mark result register
				if (result_type != ValueType::UNKNOWN && !instr.operands.empty() &&
				    instr.operands[0].type == IRValue::Type::REGISTER) {
					int result_vreg = std::get<int>(instr.operands[0].value);
					vreg_types[result_vreg] = result_type;
					if (result_type == ValueType::VARIANT) {
						variant_offsets[result_vreg] = next_variant_slot++;
					}
				}

				// Format instruction index
				std::cout << std::setw(4) << i << ": ";

				// Format opcode
				std::cout << std::setw(20) << std::left << ir_opcode_name(instr.opcode);

				// Format operands with register allocation info
				for (size_t j = 0; j < instr.operands.size(); j++) {
					if (j > 0) std::cout << ", ";
					if (verbose) {
						std::cout << format_operand_detailed(instr.operands[j]);
					} else {
						std::cout << instr.operands[j].to_string();
					}

					// Show register allocation if codegen mode
					if (show_codegen && instr.operands[j].type == IRValue::Type::REGISTER) {
						int vreg = std::get<int>(instr.operands[j].value);
						int preg = allocator.allocate_register(vreg, i);

						std::cout << "(";
						if (preg >= 0) {
							std::cout << reg_name(preg);
						} else {
							std::cout << "spilled";
						}
						std::cout << ")";

						// Show value type if known
						if (vreg_types.count(vreg)) {
							std::cout << "[" << value_type_name(vreg_types[vreg]) << "]";
							if (vreg_types[vreg] == ValueType::VARIANT && variant_offsets.count(vreg)) {
								std::cout << "[slot" << variant_offsets[vreg] << "]";
							}
						}
					}
				}

				// Show type hint if present
				if (instr.type_hint != IRInstruction::TypeHint_NONE) {
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
