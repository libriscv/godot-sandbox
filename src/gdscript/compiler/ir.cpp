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
		case IROpcode::CONVERT: return "CONVERT";
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
		case IROpcode::BIT_AND: return "BIT_AND";
		case IROpcode::BIT_OR: return "BIT_OR";
		case IROpcode::BIT_XOR: return "BIT_XOR";
		case IROpcode::BIT_NOT: return "BIT_NOT";
		case IROpcode::SHL: return "SHL";
		case IROpcode::SHR: return "SHR";
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

const char* variant_type_name(IRInstruction::TypeHint hint) {
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
		case Variant::STRING_NAME: return "STRING_NAME";
		case Variant::NODE_PATH: return "NODE_PATH";
		case Variant::VECTOR2: return "VECTOR2";
		case Variant::VECTOR2I: return "VECTOR2I";
		case Variant::VECTOR3: return "VECTOR3";
		case Variant::VECTOR3I: return "VECTOR3I";
		case Variant::VECTOR4: return "VECTOR4";
		case Variant::VECTOR4I: return "VECTOR4I";
		case Variant::COLOR: return "COLOR";
		case Variant::RECT2: return "RECT2";
		case Variant::RECT2I: return "RECT2I";
		case Variant::TRANSFORM2D: return "TRANSFORM2D";
		case Variant::TRANSFORM3D: return "TRANSFORM3D";
		case Variant::BASIS: return "BASIS";
		case Variant::QUATERNION: return "QUATERNION";
		case Variant::PLANE: return "PLANE";
		case Variant::AABB: return "AABB";
		case Variant::PROJECTION: return "PROJECTION";
		case Variant::ARRAY: return "ARRAY";
		case Variant::DICTIONARY: return "DICTIONARY";
		case Variant::RID: return "RID";
		case Variant::CALLABLE: return "CALLABLE";
		case Variant::SIGNAL: return "SIGNAL";
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

int ir_destination_operand_index(IROpcode op) {
	switch (op) {
		// No destination register at all: these either read operand 0 or take
		// no register operands.
		case IROpcode::LABEL:
		case IROpcode::JUMP:
		case IROpcode::BRANCH_ZERO:
		case IROpcode::BRANCH_NOT_ZERO:
		case IROpcode::BRANCH_EQ:
		case IROpcode::BRANCH_NEQ:
		case IROpcode::BRANCH_LT:
		case IROpcode::BRANCH_LTE:
		case IROpcode::BRANCH_GT:
		case IROpcode::BRANCH_GTE:
		case IROpcode::RETURN:
		// STORE_GLOBAL writes memory: operand 0 is the global index, operand 1
		// is the value being read.
		case IROpcode::STORE_GLOBAL:
		// VSET/VSET_INLINE write into the object held by operand 0, which they
		// read rather than define.
		case IROpcode::VSET:
		case IROpcode::VSET_INLINE:
			return -1;

		// CALL is "CALL name, dst, argc, args..." - the callee name occupies
		// operand 0.
		case IROpcode::CALL:
			return 1;

		default:
			return 0;
	}
}

int ir_destination_register(const IRInstruction& instr) {
	const int index = ir_destination_operand_index(instr.opcode);
	if (index < 0 || static_cast<size_t>(index) >= instr.operands.size()) {
		return -1;
	}
	const IRValue& operand = instr.operands[index];
	if (operand.type != IRValue::Type::REGISTER) {
		return -1;
	}
	return std::get<int>(operand.value);
}

bool ir_reads_operand(const IRInstruction& instr, size_t index) {
	if (index >= instr.operands.size()) {
		return false;
	}
	if (instr.operands[index].type != IRValue::Type::REGISTER) {
		return false;
	}
	const int dst_index = ir_destination_operand_index(instr.opcode);
	if (dst_index >= 0 && static_cast<size_t>(dst_index) == index) {
		return false;
	}
	return true;
}

void ir_collect_read_registers(const IRInstruction& instr, std::vector<int>& out) {
	// A bare RETURN returns whatever is in r0.
	if (instr.opcode == IROpcode::RETURN && instr.operands.empty()) {
		out.push_back(0);
		return;
	}
	for (size_t i = 0; i < instr.operands.size(); i++) {
		if (ir_reads_operand(instr, i)) {
			out.push_back(std::get<int>(instr.operands[i].value));
		}
	}
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
