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

enum class IROpcode {
	// Stack and register operations
	LOAD_IMM,        // Load immediate integer value into register
	LOAD_FLOAT_IMM,  // Load immediate float value into register
	LOAD_BOOL,       // Load immediate boolean value into register
	LOAD_STRING,     // Load immediate string value into register
	LOAD_GLOBAL,     // Load global variable into register
	STORE_GLOBAL,    // Store register into global variable
	MOVE,            // Move between registers

	// Arithmetic
	ADD,             // Add two registers
	SUB,             // Subtract
	MUL,             // Multiply
	DIV,             // Divide
	MOD,             // Modulo
	NEG,             // Negate

	// Comparison (sets register to 0 or 1)
	CMP_EQ,          // ==
	CMP_NEQ,         // !=
	CMP_LT,          // <
	CMP_LTE,         // <=
	CMP_GT,          // >
	CMP_GTE,         // >=

	// Logical
	AND,             // Logical AND
	OR,              // Logical OR
	NOT,             // Logical NOT

	// Control flow
	LABEL,           // Target for branches
	JUMP,            // Unconditional jump
	BRANCH_ZERO,     // Branch if register == 0
	BRANCH_NOT_ZERO, // Branch if register != 0
	BRANCH_EQ,       // Branch if reg1 == reg2 (fused comparison + branch)
	BRANCH_NEQ,      // Branch if reg1 != reg2 (fused comparison + branch)
	BRANCH_LT,       // Branch if reg1 < reg2 (fused comparison + branch)
	BRANCH_LTE,      // Branch if reg1 <= reg2 (fused comparison + branch)
	BRANCH_GT,       // Branch if reg1 > reg2 (fused comparison + branch)
	BRANCH_GTE,      // Branch if reg1 >= reg2 (fused comparison + branch)

	// Function calls
	CALL,            // Call local function
	CALL_SYSCALL,    // Call syscall (for Godot API)
	RETURN,          // Return from function

	// Variant operations (through syscalls)
	VCALL,           // Variant method call
	VGET,            // Get property from variant
	VSET,            // Set property on variant

	// Inline primitive construction (no syscalls)
	MAKE_VECTOR2,    // Construct Vector2 inline
	MAKE_VECTOR3,    // Construct Vector3 inline
	MAKE_VECTOR4,    // Construct Vector4 inline
	MAKE_VECTOR2I,   // Construct Vector2i inline
	MAKE_VECTOR3I,   // Construct Vector3i inline
	MAKE_VECTOR4I,   // Construct Vector4i inline
	MAKE_COLOR,      // Construct Color inline
	MAKE_RECT2,      // Construct Rect2 inline
	MAKE_RECT2I,     // Construct Rect2i inline
	MAKE_PLANE,      // Construct Plane inline

	// Array and Dictionary construction (via VCREATE syscall)
	MAKE_ARRAY,      // Construct Array (empty or with elements)
	MAKE_DICTIONARY, // Construct Dictionary (empty)

	// Packed array construction (via VCREATE syscall)
	MAKE_PACKED_BYTE_ARRAY,      // Construct PackedByteArray (empty or with elements)
	MAKE_PACKED_INT32_ARRAY,     // Construct PackedInt32Array (empty or with elements)
	MAKE_PACKED_INT64_ARRAY,     // Construct PackedInt64Array (empty or with elements)
	MAKE_PACKED_FLOAT32_ARRAY,   // Construct PackedFloat32Array (empty or with elements)
	MAKE_PACKED_FLOAT64_ARRAY,   // Construct PackedFloat64Array (empty or with elements)
	MAKE_PACKED_STRING_ARRAY,    // Construct PackedStringArray (empty or with elements)
	MAKE_PACKED_VECTOR2_ARRAY,   // Construct PackedVector2Array (empty or with elements)
	MAKE_PACKED_VECTOR3_ARRAY,   // Construct PackedVector3Array (empty or with elements)
	MAKE_PACKED_COLOR_ARRAY,     // Construct PackedColorArray (empty or with elements)
	MAKE_PACKED_VECTOR4_ARRAY,   // Construct PackedVector4Array (empty or with elements)

	// Inline member access (no syscalls)
	VGET_INLINE,     // Get inlined member from Variant (x, y, z, w, r, g, b, a)
	VSET_INLINE,     // Set inlined member on Variant
};

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
		EMPTY_DICT      // Empty dictionary {}
	};
	InitType init_type = InitType::NONE;
	std::variant<int64_t, double, std::string, bool> init_value;
};

struct IRProgram {
	std::vector<IRGlobalVar> globals;
	std::vector<IRFunction> functions;
	std::vector<std::string> string_constants;
};

const char* ir_opcode_name(IROpcode op);

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
