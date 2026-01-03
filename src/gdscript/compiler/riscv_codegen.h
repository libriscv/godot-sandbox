#pragma once
#include "ir.h"
#include "register_allocator.h"
#include <unordered_set>
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

	// Get constant pool (for ELF builder to create .rodata section)
	const std::vector<int64_t>& get_constant_pool() const { return m_constant_pool; }

	// Get global variables information
	const std::vector<IRGlobalVar>& get_globals() const { return m_globals; }
	size_t get_global_data_size() const { return m_global_data_size; }

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
	void emit_r4_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t rs3, uint8_t funct2);

	// Higher-level RISC-V instructions
	void emit_li(uint8_t rd, int64_t imm);      // Load immediate
	void emit_la(uint8_t rd, const std::string& label); // Load address (pseudo: auipc + addi)
	void emit_mv(uint8_t rd, uint8_t rs);       // Move
	void emit_addi(uint8_t rd, uint8_t rs1, int32_t imm); // Add immediate
	void emit_add(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_sub(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_mul(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_div(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_rem(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_and(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_or(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_xor(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_xori(uint8_t rd, uint8_t rs, int32_t imm);  // XOR immediate
	void emit_slt(uint8_t rd, uint8_t rs1, uint8_t rs2);
	void emit_seqz(uint8_t rd, uint8_t rs);   // Set if equal to zero (pseudo: sltiu rd, rs, 1)
	void emit_snez(uint8_t rd, uint8_t rs);   // Set if not equal to zero (pseudo: sltu rd, x0, rs)
	void emit_beq(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_bne(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_blt(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_bge(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_bltu(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_bgeu(uint8_t rs1, uint8_t rs2, int32_t offset);
	void emit_jal(uint8_t rd, int32_t offset);
	void emit_jalr(uint8_t rd, uint8_t rs1, int32_t offset);
	void emit_ecall();
	void emit_ret();

	// Load/Store instructions (with automatic large offset handling)
	void emit_ld(uint8_t rd, uint8_t rs1, int32_t offset);   // Load doubleword (64-bit)
	void emit_lw(uint8_t rd, uint8_t rs1, int32_t offset);   // Load word (32-bit)
	void emit_lwu(uint8_t rd, uint8_t rs1, int32_t offset);  // Load word unsigned (32-bit, zero-extended to 64-bit)
	void emit_lh(uint8_t rd, uint8_t rs1, int32_t offset);   // Load halfword (16-bit)
	void emit_lb(uint8_t rd, uint8_t rs1, int32_t offset);   // Load byte (8-bit signed)
	void emit_lbu(uint8_t rd, uint8_t rs1, int32_t offset);  // Load byte unsigned (8-bit)
	void emit_sd(uint8_t rs2, uint8_t rs1, int32_t offset);  // Store doubleword (64-bit)
	void emit_sw(uint8_t rs2, uint8_t rs1, int32_t offset);  // Store word (32-bit)
	void emit_sh(uint8_t rs2, uint8_t rs1, int32_t offset);  // Store halfword (16-bit)
	void emit_sb(uint8_t rs2, uint8_t rs1, int32_t offset);  // Store byte (8-bit)

	// Floating-point load/store (RV64D extension)
	void emit_fld(uint8_t rd, uint8_t rs1, int32_t offset);  // Load double (64-bit FP)
	void emit_fsd(uint8_t rs2, uint8_t rs1, int32_t offset); // Store double (64-bit FP)
	void emit_flw(uint8_t rd, uint8_t rs1, int32_t offset);  // Load float (32-bit FP)
	void emit_fsw(uint8_t rs2, uint8_t rs1, int32_t offset); // Store float (32-bit FP)
	void emit_fcvt_d_s(uint8_t rd, uint8_t rs1);             // Convert float to double
	void emit_fcvt_s_d(uint8_t rd, uint8_t rs1);             // Convert double to float
	void emit_fcvt_d_l(uint8_t rd, uint8_t rs1);             // Convert signed 64-bit int to double

	// FP arithmetic instructions (RV64D extension - double precision)
	void emit_fadd_d(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Double-precision FP add
	void emit_fsub_d(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Double-precision FP sub
	void emit_fmul_d(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Double-precision FP mul
	void emit_fdiv_d(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Double-precision FP div
	void emit_fmv_d(uint8_t rd, uint8_t rs);                // Double-precision FP move

	// FP arithmetic instructions (RV32F extension - single precision)
	void emit_fadd_s(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Single-precision FP add
	void emit_fsub_s(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Single-precision FP sub
	void emit_fmul_s(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Single-precision FP mul
	void emit_fdiv_s(uint8_t rd, uint8_t rs1, uint8_t rs2);  // Single-precision FP div

	// Additional integer instructions
	void emit_sext_w(uint8_t rd, uint8_t rs);  // Sign-extend word to doubleword (addiw rd, rs, 0)

	// Pseudo-instructions
	void emit_call(const std::string& func_name);
	void emit_jump(const std::string& label);

	// Label management
	void define_label(const std::string& label);
	void mark_label_use(const std::string& label, size_t code_offset);
	void resolve_labels();

	// Variant field access helpers
	// Load/store Variant fields with proper types and offsets
	void emit_load_variant_type(uint8_t rd, uint8_t base_reg, int32_t variant_offset);
	void emit_store_variant_type(uint8_t rs, uint8_t base_reg, int32_t variant_offset);
	void emit_load_variant_bool(uint8_t rd, uint8_t base_reg, int32_t variant_offset);
	void emit_store_variant_bool(uint8_t rs, uint8_t base_reg, int32_t variant_offset);
	void emit_load_variant_int(uint8_t rd, uint8_t base_reg, int32_t variant_offset);
	void emit_store_variant_int(uint8_t rs, uint8_t base_reg, int32_t variant_offset);

	// Variant management
	void emit_variant_create_int(int stack_offset, int64_t value);
	void emit_variant_create_bool(int stack_offset, bool value);
	void emit_variant_create_string(int stack_offset, int string_idx);
	void emit_variant_copy(int dst_offset, int src_offset);
	void emit_variant_eval(int result_offset, int lhs_offset, int rhs_offset, int op);

	// VCREATE syscall abstraction
	// Generic VCREATE emission that handles register clobbering and stack management
	// variant_type: Variant::Type enum value (e.g., Variant::STRING, Variant::ARRAY)
	// method: Constructor method number (type-specific)
	// data_ptr_reg: Register containing pointer to data (or REG_ZERO for nullptr)
	// result_offset: Stack offset where to create the Variant
	// Note: Caller is responsible for setting up data_ptr_reg before calling
	void emit_vcreate_syscall(int variant_type, int method, uint8_t data_ptr_reg, int result_offset);

	// High-level variant constructors using VCREATE
	// These handle all the setup including data preparation
	void emit_variant_create_empty_array(int stack_offset);
	void emit_variant_create_empty_dictionary(int stack_offset);

	// Typed operations (optimized paths when type hints are available)
	// These emit native RISC-V instructions instead of syscalls
	void emit_typed_int_binary_op(int result_offset, int lhs_offset, int rhs_offset, IROpcode op);
	void emit_typed_int_comparison(int result_offset, int lhs_offset, int rhs_offset, IROpcode cmp_op);
	void emit_typed_float_binary_op(int result_offset, int lhs_offset, int rhs_offset, IROpcode op);
	void emit_typed_vector_binary_op(int result_offset, int lhs_offset, int rhs_offset, IROpcode op, IRInstruction::TypeHint type_hint);

	// Get stack offset for a virtual register (in bytes)
	int get_variant_stack_offset(int virtual_reg);

	// Syscall result handling helpers
	// Store syscall result from register to Variant, with optional register allocation
	// result_vreg: virtual register for the result
	// result_reg: physical register containing the result (e.g., REG_A0 after syscall)
	// result_offset: stack offset for the Variant
	// variant_type: Variant type value (e.g., 2 for INT, 24 for GDOBJECT)
	void emit_syscall_result(int result_vreg, uint8_t result_reg, int result_offset, int variant_type);

	// Stack manipulation helpers
	// Adjust stack pointer by a constant amount (positive or negative)
	// Handles both small immediates (< 2048) and large values
	void emit_stack_adjust(int32_t amount);

	// Load stack pointer offset into a register
	// If offset < 2048, uses addi; otherwise loads into temp register and adds
	void emit_load_stack_offset(uint8_t rd, int32_t offset);

	// Check if a Variant type needs permanent storage (refcounted/complex types)
	static bool is_complex_variant_type(int variant_type);

	// Output buffer
	std::vector<uint8_t> m_code;
	std::unordered_map<std::string, size_t> m_labels;
	std::vector<std::pair<std::string, size_t>> m_label_uses;
	std::unordered_map<std::string, size_t> m_functions;

	// Register allocator
	RegisterAllocator m_allocator;

	// For VARIANT values: virtual_reg -> stack offset
	std::unordered_map<int, int> m_variant_offsets;

	size_t m_num_params = 0; // Number of parameters in current function
	int m_stack_frame_size = 0; // Total stack frame size in bytes
	int m_next_variant_slot = 0; // Next Variant slot to allocate
	int m_current_instr_idx = 0; // Current instruction index for register allocation

	// Variant structure layout constants
	// Variant layout: [uint32_t m_type (4)] [padding (4)] [union data (16)]
	static constexpr int VARIANT_SIZE = 24;           // Total size of Variant struct
	static constexpr int VARIANT_TYPE_OFFSET = 0;     // Offset to m_type field (4 bytes)
	static constexpr int VARIANT_DATA_OFFSET = 8;     // Offset to union data (bool/int64/etc)

	// String constants from IR
	const std::vector<std::string>* m_string_constants = nullptr;

	// Constant pool for large immediates (64-bit values that can't be encoded in instructions)
	std::vector<int64_t> m_constant_pool;
	std::unordered_map<int64_t, size_t> m_constant_pool_map; // value -> index

	// Add a constant to the pool and return its index
	size_t add_constant(int64_t value);

	// Generate a unique local label for internal use
	std::string gen_local_label(const std::string& prefix);

	// Convert a Variant component (at comp_offset) to float and store to dest_offset + store_offset
	// Handles both INT and FLOAT Variants
	// If normalize_by_255 is true, divides integers by 255.0 (for Color components)
	void emit_variant_component_to_float(int comp_offset, int result_offset, int store_offset, bool normalize_by_255 = false);

	// Label counter for generating unique local labels
	int m_label_counter = 0;

	// Global variables
	std::vector<IRGlobalVar> m_globals;
	size_t m_global_count = 0;
	size_t m_global_data_size = 0;

	// Property name strings for @export globals
	// These are stored as: vector of {string_data, label_name}
	std::vector<std::pair<std::string, std::string>> m_property_name_strings;

	// RISC-V RV64I register definitions
	static constexpr uint8_t REG_ZERO = 0;  // x0 - always zero
	static constexpr uint8_t REG_RA = 1;    // x1 - return address
	static constexpr uint8_t REG_SP = 2;    // x2 - stack pointer
	static constexpr uint8_t REG_GP = 3;    // x3 - global pointer
	static constexpr uint8_t REG_TP = 4;    // x4 - thread pointer
	static constexpr uint8_t REG_T0 = 5;    // x5-x7 - temporaries
	static constexpr uint8_t REG_T1 = 6;
	static constexpr uint8_t REG_T2 = 7;
	// x8-x9 are s0-s1 (saved registers)
	// x10-x17 are a0-a7 (argument registers)
	// x18-x27 are s2-s11 (saved registers)
	static constexpr uint8_t REG_T3 = 28;   // x28-x31 - temporaries
	static constexpr uint8_t REG_T4 = 29;
	static constexpr uint8_t REG_T5 = 30;
	static constexpr uint8_t REG_T6 = 31;
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

	static constexpr uint8_t REG_FA0 = 0;  // f0-f1 - floating-point return values
	static constexpr uint8_t REG_FA1 = 1;  // f0-f7 - floating-point arguments
	static constexpr uint8_t REG_FA2 = 2;
	static constexpr uint8_t REG_FA3 = 3;
	static constexpr uint8_t REG_FA4 = 4;
	static constexpr uint8_t REG_FA5 = 5;
	static constexpr uint8_t REG_FA6 = 6;
	static constexpr uint8_t REG_FA7 = 7;
};

} // namespace gdscript
