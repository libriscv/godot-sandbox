#include "ir.h"
#include <sstream>

namespace gdscript {

const char* ir_opcode_name(IROpcode op) {
	switch (op) {
		case IROpcode::LOAD_IMM: return "LOAD_IMM";
		case IROpcode::LOAD_BOOL: return "LOAD_BOOL";
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

std::string IRValue::to_string() const {
	std::ostringstream oss;
	switch (type) {
		case Type::REGISTER:
			oss << "r" << std::get<int>(value);
			break;
		case Type::IMMEDIATE:
			oss << std::get<int64_t>(value);
			break;
		case Type::LABEL:
			oss << "@" << std::get<std::string>(value);
			break;
		case Type::VARIABLE:
			oss << "$" << std::get<std::string>(value);
			break;
		case Type::STRING:
			oss << "\"" << std::get<std::string>(value) << "\"";
			break;
	}
	return oss.str();
}

std::string IRInstruction::to_string() const {
	std::ostringstream oss;
	oss << ir_opcode_name(opcode);

	for (const auto& op : operands) {
		oss << " " << op.to_string();
	}

	return oss.str();
}

} // namespace gdscript
