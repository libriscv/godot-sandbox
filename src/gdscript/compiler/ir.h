#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <variant>
#include <memory>
#include "function_signature.h"
#include "variant_types.h"

namespace gdscript {

// Generated from ir_opcodes.def.
enum class IROpcode {
#define IR_OPCODE(name, mnemonic, sig, effects) name,
#include "ir_opcodes.def"
#undef IR_OPCODE
};

// Table sizing and completeness checks.
enum : size_t {
	IR_OPCODE_COUNT =
#define IR_OPCODE(name, mnemonic, sig, effects) + 1
#include "ir_opcodes.def"
#undef IR_OPCODE
};

// Operand role. All read/write queries go through these.
enum class IROperandKind : uint8_t {
	NONE,
	DST,
	// Read-modify-write: copy propagation and DCE must preserve the register.
	INOUT,
	SRC,
	IMM,
	FIMM,
	STR,
	LBL,
	CNT,       // trailing-list length
	CNT2,      // trailing-list pair count
	SRC_LIST,
	ARG_LIST,  // registers and/or immediates
	LBL_LIST,
};

enum IREffect : uint32_t {
	IR_PURE          = 0,
	IR_SIDE_EFFECTS  = 1u << 0,
	IR_BRANCH        = 1u << 1,
	IR_FUSED_BRANCH  = 1u << 2,
	IR_TERMINATOR    = 1u << 3,
	IR_LABEL         = 1u << 4,
	IR_CALL          = 1u << 5,
	IR_READS_GLOBAL  = 1u << 6,
	IR_WRITES_GLOBAL = 1u << 7,
	IR_ARITHMETIC    = 1u << 8,   // foldable when operands are known
	IR_COMPARISON    = 1u << 9,   // result is boolean 0/1
	IR_SIMPLE_LOAD   = 1u << 10,  // loads and MOVE; no computation
};

struct IROperandSignature {
	static constexpr size_t MAX_OPERANDS = 8;

	IROperandKind kinds[MAX_OPERANDS] {};
	uint8_t count = 0;

	constexpr IROperandSignature() = default;

	template <typename... Kinds>
	constexpr IROperandSignature(Kinds... kinds_in)
		: kinds { kinds_in... }, count(sizeof...(Kinds))
	{
		static_assert(sizeof...(Kinds) <= MAX_OPERANDS, "IR opcode has too many operands");
	}

	// Ends in a variadic list.
	constexpr bool is_variadic() const {
		return count > 0 &&
			(kinds[count - 1] == IROperandKind::SRC_LIST ||
			 kinds[count - 1] == IROperandKind::ARG_LIST ||
			 kinds[count - 1] == IROperandKind::LBL_LIST);
	}

	// Operands before the repeating tail.
	constexpr size_t fixed_count() const {
		return is_variadic() ? static_cast<size_t>(count) - 1 : static_cast<size_t>(count);
	}

	// Role of operand `index`; NONE past the end.
	constexpr IROperandKind kind_at(size_t index) const {
		if (index < fixed_count()) {
			return kinds[index];
		}
		if (is_variadic()) {
			return kinds[count - 1];
		}
		return IROperandKind::NONE;
	}
};

struct IROpcodeInfo {
	IROpcode opcode;
	const char* mnemonic;
	IROperandSignature signature;
	uint32_t effects;
};

// Lookup; every opcode has an entry, checked at build time.
const IROpcodeInfo& ir_opcode_info(IROpcode op);

inline bool ir_has_effect(IROpcode op, IREffect effect) {
	return (ir_opcode_info(op).effects & effect) != 0;
}

inline bool ir_is_pure(IROpcode op) {
	return (ir_opcode_info(op).effects & IR_SIDE_EFFECTS) == 0;
}

inline bool ir_is_control_flow(IROpcode op) {
	return (ir_opcode_info(op).effects & (IR_LABEL | IR_BRANCH | IR_TERMINATOR)) != 0;
}

struct IRValue {
	enum class Type {
		REGISTER,
		IMMEDIATE,
		FLOAT,
		LABEL,
		VARIABLE,
		STRING
	};

	Type type = Type::IMMEDIATE;
	std::variant<int, int64_t, double, std::string> value;

	static IRValue reg(int r) {
		IRValue v {};
		v.type = Type::REGISTER;
		v.value = r;
		return v;
	}

	static IRValue imm(int64_t i) {
		IRValue v {};
		v.type = Type::IMMEDIATE;
		v.value = i;
		return v;
	}

	static IRValue fimm(double d) {
		IRValue v {};
		v.type = Type::FLOAT;
		v.value = d;
		return v;
	}

	static IRValue label(const std::string& l) {
		IRValue v {};
		v.type = Type::LABEL;
		v.value = l;
		return v;
	}

	static IRValue var(const std::string& name) {
		IRValue v {};
		v.type = Type::VARIABLE;
		v.value = name;
		return v;
	}

	static IRValue str(const std::string& s) {
		IRValue v {};
		v.type = Type::STRING;
		v.value = s;
		return v;
	}

	std::string to_string() const;
};

struct IRInstruction {
	// Variant::Type, or -1 for NONE.
	static constexpr int32_t TypeHint_NONE = -1;
	using TypeHint = int32_t;

	IROpcode opcode {};
	std::vector<IRValue> operands;
	TypeHint type_hint = TypeHint_NONE;

	IRInstruction(IROpcode op) : opcode(op) {}
	IRInstruction(IROpcode op, IRValue a) : opcode(op), operands{a} {}
	IRInstruction(IROpcode op, IRValue a, IRValue b) : opcode(op), operands{a, b} {}
	IRInstruction(IROpcode op, IRValue a, IRValue b, IRValue c) : opcode(op), operands{a, b, c} {}

	std::string to_string() const;
};

struct IRFunction {
	std::string name;
	std::vector<std::string> parameters;
	std::vector<IRInstruction> instructions;
	int max_registers = 0;

	// Parameters in r0..N-1, return value in r0.
	static constexpr int RETURN_REGISTER = 0;

	// Sandbox ABI: a0=return, a1-a7=args.
	static constexpr size_t MAX_PARAMETERS = 7;
};

struct IRGlobalVar {
	std::string name;
	bool is_const = false;
	bool is_property = false;
	IRInstruction::TypeHint type_hint = IRInstruction::TypeHint_NONE;

	enum class InitType {
		NONE,
		INT,
		FLOAT,
		STRING,
		BOOL,
		NULL_VAL,
		EMPTY_ARRAY,
		EMPTY_DICT,
		RUNTIME         // evaluated by global_init at startup
	};
	InitType init_type = InitType::NONE;
	std::variant<int64_t, double, std::string, bool> init_value;

	// Drives @export registration and VASSIGN decisions. TypeHint_NONE = any Variant.
	IRInstruction::TypeHint value_type = IRInstruction::TypeHint_NONE;
};

struct IRProgram {
	std::vector<IRGlobalVar> globals;
	std::vector<IRFunction> functions;
	std::vector<std::string> string_constants;

	// One entry per function, same order.
	std::vector<FunctionSignature> signatures;

	// Evaluates non-constant global initializers at startup, before @export registration.
	IRFunction global_init;
	bool has_global_init = false;
};

const char* ir_opcode_name(IROpcode op);

// For verifier diagnostics.
const char* ir_operand_kind_name(IROperandKind kind);

// For diagnostics and dumps.
const char* variant_type_name(IRInstruction::TypeHint hint);

// Operand role queries. All are ir_opcodes.def lookups.
// DST position varies by opcode (CALL uses operand 1, VSET has none);
// hardcoding "operand 0 = DST" miscompiles those opcodes.
int ir_destination_operand_index(IROpcode op);

int ir_destination_register(const IRInstruction& instr);

bool ir_reads_operand(const IRInstruction& instr, size_t index);

// True for DST or INOUT operands; substituting these redirects the write.
bool ir_writes_operand(const IRInstruction& instr, size_t index);

// Bare RETURN implicitly reads r0.
void ir_collect_read_registers(const IRInstruction& instr, std::vector<int>& out);

// Per-instruction purity. Unlike ir_is_pure(), checks GLOBAL_CALL's GlobalFn:
// impure globals (randi etc.) are not deletable even when their result is unused.
bool ir_instruction_is_pure(const IRInstruction& instr);

namespace TypeHintUtils {
	inline bool is_variant(IRInstruction::TypeHint hint) {
		return hint != IRInstruction::TypeHint_NONE;
	}

	inline bool is_vector(IRInstruction::TypeHint hint) {
		return hint == Variant::VECTOR2 ||
		       hint == Variant::VECTOR3 ||
		       hint == Variant::VECTOR4 ||
		       hint == Variant::VECTOR2I ||
		       hint == Variant::VECTOR3I ||
		       hint == Variant::VECTOR4I;
	}

	inline bool is_int_vector(IRInstruction::TypeHint hint) {
		return hint == Variant::VECTOR2I ||
		       hint == Variant::VECTOR3I ||
		       hint == Variant::VECTOR4I;
	}

	inline bool is_float_vector(IRInstruction::TypeHint hint) {
		return hint == Variant::VECTOR2 ||
		       hint == Variant::VECTOR3 ||
		       hint == Variant::VECTOR4;
	}
} // namespace TypeHintUtils

} // namespace gdscript
