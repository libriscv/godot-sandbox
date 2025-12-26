#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <variant>
#include <memory>

namespace gdscript {

// Intermediate Representation for RISC-V code generation
// This represents a simplified, linear instruction stream that can be easily converted to RISC-V

enum class IROpcode {
	// Stack and register operations
	LOAD_IMM,        // Load immediate integer value into register
	LOAD_FLOAT_IMM,  // Load immediate float value into register
	LOAD_BOOL,       // Load immediate boolean value into register
	LOAD_STRING,     // Load immediate string value into register
	LOAD_VAR,        // Load variable into register
	STORE_VAR,       // Store register into variable
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
	enum class TypeHint {
		NONE,       // No type information

		// Variant types (tracked for optimization)
		VARIANT_INT,
		VARIANT_FLOAT,
		VARIANT_BOOL,
		VARIANT_VECTOR2,
		VARIANT_VECTOR3,
		VARIANT_VECTOR4,
		VARIANT_VECTOR2I,
		VARIANT_VECTOR3I,
		VARIANT_VECTOR4I,
		VARIANT_COLOR,
		VARIANT_RECT2,
		VARIANT_RECT2I,
		VARIANT_PLANE,
		VARIANT_ARRAY,
		VARIANT_DICTIONARY,
	};

	IROpcode opcode {};
	std::vector<IRValue> operands;
	TypeHint type_hint = TypeHint::NONE; // Type hint for result (operand 0)

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

struct IRProgram {
	std::vector<IRFunction> functions;
	std::vector<std::string> string_constants;
};

const char* ir_opcode_name(IROpcode op);

// TypeHint helper functions - avoids hardcoded enum values
namespace TypeHintUtils {
	// Check if TypeHint is a Variant type
	inline bool is_variant(IRInstruction::TypeHint hint) {
		return hint >= IRInstruction::TypeHint::VARIANT_INT &&
		       hint <= IRInstruction::TypeHint::VARIANT_DICTIONARY;
	}

	// Check if TypeHint is a vector type
	inline bool is_vector(IRInstruction::TypeHint hint) {
		return hint == IRInstruction::TypeHint::VARIANT_VECTOR2 ||
		       hint == IRInstruction::TypeHint::VARIANT_VECTOR3 ||
		       hint == IRInstruction::TypeHint::VARIANT_VECTOR4 ||
		       hint == IRInstruction::TypeHint::VARIANT_VECTOR2I ||
		       hint == IRInstruction::TypeHint::VARIANT_VECTOR3I ||
		       hint == IRInstruction::TypeHint::VARIANT_VECTOR4I;
	}

	// Check if TypeHint is an integer vector type
	inline bool is_int_vector(IRInstruction::TypeHint hint) {
		return hint == IRInstruction::TypeHint::VARIANT_VECTOR2I ||
		       hint == IRInstruction::TypeHint::VARIANT_VECTOR3I ||
		       hint == IRInstruction::TypeHint::VARIANT_VECTOR4I;
	}

	// Check if TypeHint is a float vector type
	inline bool is_float_vector(IRInstruction::TypeHint hint) {
		return hint == IRInstruction::TypeHint::VARIANT_VECTOR2 ||
		       hint == IRInstruction::TypeHint::VARIANT_VECTOR3 ||
		       hint == IRInstruction::TypeHint::VARIANT_VECTOR4;
	}
} // namespace TypeHintUtils

} // namespace gdscript
