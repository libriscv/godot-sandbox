#include "ir.h"
#include <sstream>

namespace gdscript {

const char* ir_opcode_name(IROpcode op) {
	switch (op) {
		case IROpcode::LOAD_IMM: return "LOAD_IMM";
		case IROpcode::LOAD_FLOAT_IMM: return "LOAD_FLOAT_IMM";
		case IROpcode::LOAD_BOOL: return "LOAD_BOOL";
		case IROpcode::LOAD_STRING: return "LOAD_STRING";
		case IROpcode::LOAD_GLOBAL: return "LOAD_GLOBAL";
		case IROpcode::STORE_GLOBAL: return "STORE_GLOBAL";
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
		case IROpcode::BRANCH_EQ: return "BRANCH_EQ";
		case IROpcode::BRANCH_NEQ: return "BRANCH_NEQ";
		case IROpcode::BRANCH_LT: return "BRANCH_LT";
		case IROpcode::BRANCH_LTE: return "BRANCH_LTE";
		case IROpcode::BRANCH_GT: return "BRANCH_GT";
		case IROpcode::BRANCH_GTE: return "BRANCH_GTE";
		case IROpcode::CALL: return "CALL";
		case IROpcode::CALL_SYSCALL: return "CALL_SYSCALL";
		case IROpcode::RETURN: return "RETURN";
		case IROpcode::VCALL: return "VCALL";
		case IROpcode::VGET: return "VGET";
		case IROpcode::VSET: return "VSET";
		case IROpcode::MAKE_VECTOR2: return "MAKE_VECTOR2";
		case IROpcode::MAKE_VECTOR3: return "MAKE_VECTOR3";
		case IROpcode::MAKE_VECTOR4: return "MAKE_VECTOR4";
		case IROpcode::MAKE_VECTOR2I: return "MAKE_VECTOR2I";
		case IROpcode::MAKE_VECTOR3I: return "MAKE_VECTOR3I";
		case IROpcode::MAKE_VECTOR4I: return "MAKE_VECTOR4I";
		case IROpcode::MAKE_COLOR: return "MAKE_COLOR";
		case IROpcode::MAKE_RECT2: return "MAKE_RECT2";
		case IROpcode::MAKE_RECT2I: return "MAKE_RECT2I";
		case IROpcode::MAKE_PLANE: return "MAKE_PLANE";
		case IROpcode::MAKE_ARRAY: return "MAKE_ARRAY";
		case IROpcode::MAKE_DICTIONARY: return "MAKE_DICTIONARY";
		case IROpcode::MAKE_PACKED_BYTE_ARRAY: return "MAKE_PACKED_BYTE_ARRAY";
		case IROpcode::MAKE_PACKED_INT32_ARRAY: return "MAKE_PACKED_INT32_ARRAY";
		case IROpcode::MAKE_PACKED_INT64_ARRAY: return "MAKE_PACKED_INT64_ARRAY";
		case IROpcode::MAKE_PACKED_FLOAT32_ARRAY: return "MAKE_PACKED_FLOAT32_ARRAY";
		case IROpcode::MAKE_PACKED_FLOAT64_ARRAY: return "MAKE_PACKED_FLOAT64_ARRAY";
		case IROpcode::MAKE_PACKED_STRING_ARRAY: return "MAKE_PACKED_STRING_ARRAY";
		case IROpcode::MAKE_PACKED_VECTOR2_ARRAY: return "MAKE_PACKED_VECTOR2_ARRAY";
		case IROpcode::MAKE_PACKED_VECTOR3_ARRAY: return "MAKE_PACKED_VECTOR3_ARRAY";
		case IROpcode::MAKE_PACKED_COLOR_ARRAY: return "MAKE_PACKED_COLOR_ARRAY";
		case IROpcode::MAKE_PACKED_VECTOR4_ARRAY: return "MAKE_PACKED_VECTOR4_ARRAY";
		case IROpcode::VGET_INLINE: return "VGET_INLINE";
		case IROpcode::VSET_INLINE: return "VSET_INLINE";
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
		case Type::FLOAT:
			oss << std::get<double>(value);
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
