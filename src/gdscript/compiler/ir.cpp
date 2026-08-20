#include "ir.h"
#include "globals.h"
#include <sstream>

namespace gdscript {

// The opcode metadata table, generated from ir_opcodes.def. Every accessor
// below is a lookup into this one array, so a pass can never disagree with
// another about what an opcode does.
static constexpr IROpcodeInfo IR_OPCODE_TABLE[] = {
#define DST       IROperandKind::DST
#define SRC       IROperandKind::SRC
#define IMM       IROperandKind::IMM
#define FIMM      IROperandKind::FIMM
#define STR       IROperandKind::STR
#define LBL       IROperandKind::LBL
#define CNT       IROperandKind::CNT
#define CNT2      IROperandKind::CNT2
#define SRC_LIST  IROperandKind::SRC_LIST
#define ARG_LIST  IROperandKind::ARG_LIST
#define LBL_LIST  IROperandKind::LBL_LIST
#define SIG(...)  IROperandSignature(__VA_ARGS__)
#define IR_OPCODE(name, mnemonic, sig, effects) \
	IROpcodeInfo { IROpcode::name, mnemonic, sig, static_cast<uint32_t>(effects) },
#include "ir_opcodes.def"
#undef IR_OPCODE
#undef SIG
#undef LBL_LIST
#undef ARG_LIST
#undef SRC_LIST
#undef CNT2
#undef CNT
#undef LBL
#undef STR
#undef FIMM
#undef IMM
#undef SRC
#undef DST
};

static_assert(sizeof(IR_OPCODE_TABLE) / sizeof(IR_OPCODE_TABLE[0]) == IR_OPCODE_COUNT,
	"The opcode metadata table and the opcode enum disagree");

const IROpcodeInfo& ir_opcode_info(IROpcode op) {
	const size_t index = static_cast<size_t>(op);
	// The table is generated from the same list as the enum, so an out-of-range
	// opcode is a corrupt IRInstruction rather than a missing table entry.
	if (index >= IR_OPCODE_COUNT) {
		static constexpr IROpcodeInfo unknown { IROpcode::LABEL, "UNKNOWN", IROperandSignature(), IR_SIDE_EFFECTS };
		return unknown;
	}
	return IR_OPCODE_TABLE[index];
}

const char* ir_opcode_name(IROpcode op) {
	return ir_opcode_info(op).mnemonic;
}

const char* ir_operand_kind_name(IROperandKind kind) {
	switch (kind) {
		case IROperandKind::NONE: return "nothing";
		case IROperandKind::DST: return "destination register";
		case IROperandKind::SRC: return "source register";
		case IROperandKind::IMM: return "immediate";
		case IROperandKind::FIMM: return "float immediate";
		case IROperandKind::STR: return "string";
		case IROperandKind::LBL: return "label";
		case IROperandKind::CNT: return "argument count";
		case IROperandKind::CNT2: return "pair count";
		case IROperandKind::SRC_LIST: return "source register";
		case IROperandKind::ARG_LIST: return "argument";
		case IROperandKind::LBL_LIST: return "label";
	}
	return "unknown";
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

bool ir_instruction_is_pure(const IRInstruction& instr) {
	if (!ir_is_pure(instr.opcode)) {
		return false;
	}
	if (instr.opcode != IROpcode::GLOBAL_CALL || instr.operands.size() < 2 ||
		!std::holds_alternative<int64_t>(instr.operands[1].value)) {
		return true;
	}
	// GLOBAL_CALL result, global_fn, ...
	return !global_function(static_cast<GlobalFn>(std::get<int64_t>(instr.operands[1].value))).impure;
}

int ir_destination_operand_index(IROpcode op) {
	const IROperandSignature& signature = ir_opcode_info(op).signature;
	for (size_t i = 0; i < signature.fixed_count(); i++) {
		if (signature.kinds[i] == IROperandKind::DST) {
			return static_cast<int>(i);
		}
	}
	return -1;
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
	// A register operand is read unless the signature says it is the one the
	// instruction defines.
	const IROperandKind kind = ir_opcode_info(instr.opcode).signature.kind_at(index);
	return kind != IROperandKind::DST;
}

void ir_collect_read_registers(const IRInstruction& instr, std::vector<int>& out) {
	// A bare RETURN returns whatever is in the return register.
	if (instr.opcode == IROpcode::RETURN && instr.operands.empty()) {
		out.push_back(IRFunction::RETURN_REGISTER);
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

	for (size_t i = 0; i < operands.size(); i++) {
		// GLOBAL_CALL's first immediate is a GlobalFn. A dump saying "11" says
		// nothing; a dump saying "absi" says which global was compiled.
		if (opcode == IROpcode::GLOBAL_CALL && i == 1 &&
			operands[i].type == IRValue::Type::IMMEDIATE) {
			oss << " " << global_function(static_cast<GlobalFn>(std::get<int64_t>(operands[i].value))).name;
			continue;
		}
		oss << " " << operands[i].to_string();
	}

	return oss.str();
}

} // namespace gdscript
