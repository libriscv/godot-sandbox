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
	LOAD_BOOL,       // Load immediate boolean value into register
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
};

struct IRValue {
	enum class Type {
		REGISTER,    // Virtual register (will be mapped to RISC-V registers)
		IMMEDIATE,   // Immediate integer value
		LABEL,       // Branch target label
		VARIABLE,    // Local variable name
		STRING       // String constant
	};

	Type type;
	std::variant<int, int64_t, std::string> value;

	static IRValue reg(int r) {
		IRValue v;
		v.type = Type::REGISTER;
		v.value = r;
		return v;
	}

	static IRValue imm(int64_t i) {
		IRValue v;
		v.type = Type::IMMEDIATE;
		v.value = i;
		return v;
	}

	static IRValue label(const std::string& l) {
		IRValue v;
		v.type = Type::LABEL;
		v.value = l;
		return v;
	}

	static IRValue var(const std::string& name) {
		IRValue v;
		v.type = Type::VARIABLE;
		v.value = name;
		return v;
	}

	static IRValue str(const std::string& s) {
		IRValue v;
		v.type = Type::STRING;
		v.value = s;
		return v;
	}

	std::string to_string() const;
};

struct IRInstruction {
	IROpcode opcode;
	std::vector<IRValue> operands;

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

} // namespace gdscript
