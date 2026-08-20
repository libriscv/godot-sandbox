#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <variant>
#include <memory>
#include "variant_types.h"

namespace gdscript {

// Intermediate Representation for RISC-V code generation
// This represents a simplified, linear instruction stream that can be easily converted to RISC-V

// The opcode enum, generated from the metadata table so that the list of
// opcodes and the description of each one cannot drift apart. See
// ir_opcodes.def.
enum class IROpcode {
#define IR_OPCODE(name, mnemonic, sig, effects) name,
#include "ir_opcodes.def"
#undef IR_OPCODE
};

// Number of opcodes, for table sizing and completeness checks.
enum : size_t {
	IR_OPCODE_COUNT =
#define IR_OPCODE(name, mnemonic, sig, effects) + 1
#include "ir_opcodes.def"
#undef IR_OPCODE
};

// The role an operand plays. Everything that has to know whether an operand is
// read, written, or not a register at all goes through these.
enum class IROperandKind : uint8_t {
	NONE,      // past the end of the signature
	DST,       // destination register: the operand the instruction defines
	SRC,       // source register: an operand the instruction reads
	IMM,       // immediate integer
	FIMM,      // immediate double
	STR,       // inline string
	LBL,       // branch target label
	CNT,       // immediate holding the length of the trailing list
	CNT2,      // immediate holding half the length of the trailing list (pairs)
	SRC_LIST,  // trailing run of zero or more source registers
	ARG_LIST,  // trailing run of zero or more source registers and/or immediates
};

// What a pass is allowed to do with an instruction.
enum IREffect : uint32_t {
	// The empty mask: no side effects. A pure instruction can be deleted when
	// its destination is unused, and moved when its inputs allow.
	IR_PURE          = 0,
	// Must not be deleted or reordered against other side-effecting work.
	IR_SIDE_EFFECTS  = 1u << 0,
	IR_BRANCH        = 1u << 1,   // conditional transfer of control
	IR_FUSED_BRANCH  = 1u << 2,   // comparison fused into the branch: reads two registers
	IR_TERMINATOR    = 1u << 3,   // unconditional transfer of control
	IR_LABEL         = 1u << 4,   // a branch target, and therefore a barrier
	IR_CALL          = 1u << 5,   // transfers control to code this pass cannot see
	IR_READS_GLOBAL  = 1u << 6,
	IR_WRITES_GLOBAL = 1u << 7,
	// Arithmetic, bitwise and comparison opcodes: a destination computed from
	// register operands, foldable when the operands are known.
	IR_ARITHMETIC    = 1u << 8,
	IR_COMPARISON    = 1u << 9,   // destination is a boolean 0 or 1
	// Materialises a value into a register from a single operand, without
	// computing anything: the loads and MOVE.
	IR_SIMPLE_LOAD   = 1u << 10,
};

// The operand roles of one opcode, in order.
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

	// Whether the signature ends in a list that repeats.
	constexpr bool is_variadic() const {
		return count > 0 &&
			(kinds[count - 1] == IROperandKind::SRC_LIST ||
			 kinds[count - 1] == IROperandKind::ARG_LIST);
	}

	// Number of operands before the repeating tail.
	constexpr size_t fixed_count() const {
		return is_variadic() ? static_cast<size_t>(count) - 1 : static_cast<size_t>(count);
	}

	// The role of operand `index`, with the tail repeating forever. NONE means
	// the instruction has no operand there.
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

// Everything the compiler knows about an opcode, in one place.
struct IROpcodeInfo {
	IROpcode opcode;
	const char* mnemonic;
	IROperandSignature signature;
	uint32_t effects;
};

// Metadata for an opcode. Every opcode has an entry; the table is checked
// against the enum at build time.
const IROpcodeInfo& ir_opcode_info(IROpcode op);

inline bool ir_has_effect(IROpcode op, IREffect effect) {
	return (ir_opcode_info(op).effects & effect) != 0;
}

// Whether the instruction can be deleted when nothing reads its destination,
// and moved when its inputs allow.
inline bool ir_is_pure(IROpcode op) {
	return (ir_opcode_info(op).effects & IR_SIDE_EFFECTS) == 0;
}

// A label, a branch or a jump: anywhere a linear scan of the instruction stream
// stops being a scan of one execution path.
inline bool ir_is_control_flow(IROpcode op) {
	return (ir_opcode_info(op).effects & (IR_LABEL | IR_BRANCH | IR_TERMINATOR)) != 0;
}

struct IRValue {
	enum class Type {
		REGISTER,    // Virtual register (will be mapped to RISC-V registers)
		IMMEDIATE,   // Immediate integer value
		FLOAT,       // Immediate float value (64-bit double)
		LABEL,       // Branch target label
		VARIABLE,    // Local variable name
		STRING       // String constant
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
	// Type hint is simply Variant::Type, with -1 (or a special value) for NONE
	// We use int32_t to match the Variant::Type enum
	static constexpr int32_t TypeHint_NONE = -1;
	using TypeHint = int32_t;

	IROpcode opcode {};
	std::vector<IRValue> operands;
	TypeHint type_hint = TypeHint_NONE; // Type hint for result (operand 0)

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
	int max_registers = 0; // Number of virtual registers used
};

// Global variable declaration in IR
struct IRGlobalVar {
	std::string name;
	bool is_const = false;
	bool is_property = false; // Whether this is an exported property (@export)
	IRInstruction::TypeHint type_hint = IRInstruction::TypeHint_NONE;

	// Initialization value (if any)
	enum class InitType {
		NONE,           // No initialization (will be NIL)
		INT,            // Integer literal
		FLOAT,          // Float literal
		STRING,         // String literal
		BOOL,           // Bool literal
		NULL_VAL,       // Explicit null
		EMPTY_ARRAY,    // Empty array []
		EMPTY_DICT,     // Empty dictionary {}
		RUNTIME         // Evaluated by IRProgram::global_init at startup
	};
	InitType init_type = InitType::NONE;
	std::variant<int64_t, double, std::string, bool> init_value;

	// The Variant type the global holds, when it is known at compile time.
	// Taken from the type hint when there is one and derived from the initializer
	// otherwise. Drives both @export property registration and the decision of
	// whether a store into this global has to go through VASSIGN. TypeHint_NONE
	// means "any Variant".
	IRInstruction::TypeHint value_type = IRInstruction::TypeHint_NONE;
};

struct IRProgram {
	std::vector<IRGlobalVar> globals;
	std::vector<IRFunction> functions;
	std::vector<std::string> string_constants;

	// Synthetic function evaluating every global initializer that is not a
	// compile-time constant: array and dictionary literals, constructor calls,
	// references to other globals. Runs once from the entry point, before any
	// @export property is registered. Empty when all globals fold to constants.
	IRFunction global_init;
	bool has_global_init = false;
};

const char* ir_opcode_name(IROpcode op);

// Human-readable name of an operand role, for verifier diagnostics.
const char* ir_operand_kind_name(IROperandKind kind);

// Human-readable name of a Variant type / type hint, for diagnostics and dumps.
const char* variant_type_name(IRInstruction::TypeHint hint);

// ---------------------------------------------------------------------------
// Operand roles
//
// The IR is not SSA and the operand layout is not uniform: most opcodes put
// their destination register in operand 0, but CALL puts the callee name there
// and its destination in operand 1, while VSET, STORE_GLOBAL, RETURN and the
// branches have no destination at all and read operand 0.
//
// Every pass that needs to know whether an operand is read or written has to go
// through these helpers. Passes that instead hardcode "operand 0 is the
// destination, the rest are sources" silently miscompile CALL and VSET, which is
// exactly the class of bug these exist to prevent.
//
// All of them are lookups into the signature declared in ir_opcodes.def; none
// of them re-derives anything.
// ---------------------------------------------------------------------------

// Index of the operand holding the destination register, or -1 when the opcode
// does not write a register.
int ir_destination_operand_index(IROpcode op);

// The destination register of an instruction, or -1 when it has none.
int ir_destination_register(const IRInstruction& instr);

// Whether operand `index` is a register that the instruction reads. Returns
// false for non-register operands and for the destination operand.
bool ir_reads_operand(const IRInstruction& instr, size_t index);

// Collect every register the instruction reads. RETURN without operands
// implicitly reads r0.
void ir_collect_read_registers(const IRInstruction& instr, std::vector<int>& out);

// TypeHint helper functions - now using Variant::Type directly
namespace TypeHintUtils {
	// Check if TypeHint is a Variant type (not NONE)
	inline bool is_variant(IRInstruction::TypeHint hint) {
		return hint != IRInstruction::TypeHint_NONE;
	}

	// Check if TypeHint is a vector type
	inline bool is_vector(IRInstruction::TypeHint hint) {
		return hint == Variant::VECTOR2 ||
		       hint == Variant::VECTOR3 ||
		       hint == Variant::VECTOR4 ||
		       hint == Variant::VECTOR2I ||
		       hint == Variant::VECTOR3I ||
		       hint == Variant::VECTOR4I;
	}

	// Check if TypeHint is an integer vector type
	inline bool is_int_vector(IRInstruction::TypeHint hint) {
		return hint == Variant::VECTOR2I ||
		       hint == Variant::VECTOR3I ||
		       hint == Variant::VECTOR4I;
	}

	// Check if TypeHint is a float vector type
	inline bool is_float_vector(IRInstruction::TypeHint hint) {
		return hint == Variant::VECTOR2 ||
		       hint == Variant::VECTOR3 ||
		       hint == Variant::VECTOR4;
	}
} // namespace TypeHintUtils

} // namespace gdscript
