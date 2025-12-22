#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "ir_optimizer.h"
#include <iostream>
#include <variant>

using namespace gdscript;

std::string opcode_to_string(IROpcode op) {
	switch (op) {
		case IROpcode::LOAD_IMM: return "LOAD_IMM";
		case IROpcode::LOAD_BOOL: return "LOAD_BOOL";
		case IROpcode::LOAD_STRING: return "LOAD_STRING";
		case IROpcode::LOAD_VAR: return "LOAD_VAR";
		case IROpcode::STORE_VAR: return "STORE_VAR";
		case IROpcode::MOVE: return "MOVE";
		case IROpcode::ADD: return "ADD";
		case IROpcode::SUB: return "SUB";
		case IROpcode::MUL: return "MUL";
		case IROpcode::DIV: return "DIV";
		case IROpcode::MOD: return "MOD";
		case IROpcode::NEG: return "NEG";
		case IROpcode::CMP_EQ: return "CMP_EQ";
		case IROpcode::CMP_NEQ: return "CMP_NEQ";
		case IROpcode::CMP_LT: return "CMP_LT";
		case IROpcode::CMP_LTE: return "CMP_LTE";
		case IROpcode::CMP_GT: return "CMP_GT";
		case IROpcode::CMP_GTE: return "CMP_GTE";
		case IROpcode::AND: return "AND";
		case IROpcode::OR: return "OR";
		case IROpcode::NOT: return "NOT";
		case IROpcode::LABEL: return "LABEL";
		case IROpcode::JUMP: return "JUMP";
		case IROpcode::BRANCH_ZERO: return "BRANCH_ZERO";
		case IROpcode::BRANCH_NOT_ZERO: return "BRANCH_NOT_ZERO";
		case IROpcode::CALL: return "CALL";
		case IROpcode::CALL_SYSCALL: return "CALL_SYSCALL";
		case IROpcode::RETURN: return "RETURN";
		case IROpcode::VCALL: return "VCALL";
		case IROpcode::VGET: return "VGET";
		case IROpcode::VSET: return "VSET";
		default: return "UNKNOWN";
	}
}

std::string value_to_string(const IRValue& val) {
	switch (val.type) {
		case IRValue::Type::REGISTER:
			return "r" + std::to_string(std::get<int>(val.value));
		case IRValue::Type::IMMEDIATE:
			return "#" + std::to_string(std::get<int64_t>(val.value));
		case IRValue::Type::LABEL:
			return std::get<std::string>(val.value);
		case IRValue::Type::VARIABLE:
			return std::get<std::string>(val.value);
		case IRValue::Type::STRING:
			return "\"" + std::get<std::string>(val.value) + "\"";
		default:
			return "???";
	}
}

int main(int argc, char** argv)
{
	std::string source;
	if (argc < 2) {
		// Read from stdin
		std::string line;
		while (std::getline(std::cin, line)) {
			source += line + "\n";
		}
	} else {
		source = argv[1];
	}

	try {
		Lexer lexer(source);
		Parser parser(lexer.tokenize());
		Program program = parser.parse();

		CodeGenerator codegen;
		IRProgram ir = codegen.generate(program);

		// Apply optimizations
		IROptimizer optimizer;
		optimizer.optimize(ir);

		for (const auto& func : ir.functions) {
			std::cout << "Function: " << func.name << "(";
			for (size_t i = 0; i < func.parameters.size(); i++) {
				if (i > 0) std::cout << ", ";
				std::cout << func.parameters[i];
			}
			std::cout << ")" << std::endl;
			std::cout << "  max_registers: " << func.max_registers << std::endl;
			std::cout << "  Instructions:" << std::endl;

			for (size_t i = 0; i < func.instructions.size(); i++) {
				const auto& instr = func.instructions[i];
				std::cout << "    " << i << ": " << opcode_to_string(instr.opcode);

				for (const auto& operand : instr.operands) {
					std::cout << " " << value_to_string(operand);
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
