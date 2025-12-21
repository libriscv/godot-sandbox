#pragma once
#include "ir.h"
#include "register_allocator.h"
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace gdscript {

// RISC-V instruction encoder
class RISCVCodeGen {
public:
	RISCVCodeGen();

	// Generate RISC-V machine code from IR
	std::vector<uint8_t> generate(const IRProgram& program);

	// Get function offsets (name -> offset in code)
	const std::unordered_map<std::string, size_t>& get_function_offsets() const { return m_functions; }
	
	// Get register allocator (for testing)
	const RegisterAllocator& get_allocator() const { return m_allocator; }

private:
	struct Function {
		std::string name;
		size_t offset; // Offset in code section
	};

	// Generate code for a single function
	void gen_function(const IRFunction& func);

	// Instruction emission
	void emit_word(uint32_t word);  // Emit raw 32-bit word
	void emit_r_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t funct7);
	void emit_i_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm);
	void emit_s_type(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm);
	void emit_b_type(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm);
	void emit_u_type(uint8_t opcode, uint8_t rd, uint32_t imm);
	void emit_j_type(uint8_t opcode, uint8_t rd, int32_t imm);

	// Higher-level RISC-V instructions
	void emit_li(uint8_t rd, int64_t imm);      // Load immediate
	void emit_mv(uint8_t rd, uint8_t rs);       // Move
	void emit_add(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_sub(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_mul(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_div(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_rem(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_and(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_or(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_xor(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_slt(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_beq(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_bne(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_jal(uint8_t rd, int32_t offset);
	void emit_jalr(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_ecall();
	void emit_ret();

	// Pseudo-instructions
	void emit_call(const std::string& func_name);
	void emit_jump(const std::string& label);

	// Label management
	void define_label(const std::string& label);
	void mark_label_use(const std::string& label, size_t code_offset);
	void resolve_labels();

	// Variant management
	void emit_variant_create_int(int stack_offset, int64_t value);
	void emit_variant_create_bool(int stack_offset, bool value);
	void emit_variant_create_string(int stack_offset, int string_idx);
	void emit_variant_copy(int dst_offset, int src_offset);
	void emit_variant_eval(int result_offset, int lhs_offset, int rhs_offset, int op);
	
	// Get Variant pointer in register (or compute from stack offset)
	// Returns the register containing the pointer, or -1 if needs to be computed
	int get_variant_pointer_reg(int vreg, uint8_t target_reg);
	
	// Get stack offset for a virtual register (in bytes)
	int get_variant_stack_offset(int virtual_reg);

	// Output buffer
	std::vector<uint8_t> m_code;
	std::unordered_map<std::string, size_t> m_labels;
	std::vector<std::pair<std::string, size_t>> m_label_uses;
	std::unordered_map<std::string, size_t> m_functions;

	// Register allocator
	RegisterAllocator m_allocator;
	
	// Variant-based virtual register mapping (for spilled registers)
	std::unordered_map<int, int> m_variant_offsets; // virtual_reg -> stack offset
	size_t m_num_params = 0; // Number of parameters in current function
	int m_stack_frame_size = 0; // Total stack frame size in bytes
	int m_next_variant_slot = 0; // Next Variant slot to allocate
	int m_current_instr_idx = 0; // Current instruction index for register allocation
	static constexpr int VARIANT_SIZE = 24; // Size of Variant struct

	// String constants from IR
	const std::vector<std::string>* m_string_constants = nullptr;

	// RISC-V RV64I register definitions
	static constexpr uint8_t REG_ZERO = 0;  // x0 - always zero
	static constexpr uint8_t REG_RA = 1;    // x1 - return address
	static constexpr uint8_t REG_SP = 2;    // x2 - stack pointer
	static constexpr uint8_t REG_GP = 3;    // x3 - global pointer
	static constexpr uint8_t REG_TP = 4;    // x4 - thread pointer
	static constexpr uint8_t REG_T0 = 5;    // x5-x7 - temporaries
	static constexpr uint8_t REG_T1 = 6;
	static constexpr uint8_t REG_T2 = 7;
	static constexpr uint8_t REG_FP = 8;    // x8 - frame pointer
	static constexpr uint8_t REG_S1 = 9;    // x9 - saved register
	static constexpr uint8_t REG_A0 = 10;   // x10-x17 - arguments/return values
	static constexpr uint8_t REG_A1 = 11;
	static constexpr uint8_t REG_A2 = 12;
	static constexpr uint8_t REG_A3 = 13;
	static constexpr uint8_t REG_A4 = 14;
	static constexpr uint8_t REG_A5 = 15;
	static constexpr uint8_t REG_A6 = 16;
	static constexpr uint8_t REG_A7 = 17;
};

} // namespace gdscript
