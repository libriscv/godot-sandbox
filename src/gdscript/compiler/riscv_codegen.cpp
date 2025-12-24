#include "riscv_codegen.h"
#include <stdexcept>
#include <cstring>

namespace gdscript {

RISCVCodeGen::RISCVCodeGen() {}

size_t RISCVCodeGen::add_constant(int64_t value) {
	// Check if constant already exists in pool
	auto it = m_constant_pool_map.find(value);
	if (it != m_constant_pool_map.end()) {
		return it->second;
	}

	// Add new constant
	size_t index = m_constant_pool.size();
	m_constant_pool.push_back(value);
	m_constant_pool_map[value] = index;
	return index;
}

std::vector<uint8_t> RISCVCodeGen::generate(const IRProgram& program) {
	m_code.clear();
	m_labels.clear();
	m_label_uses.clear();
	m_functions.clear();
	m_variant_offsets.clear();
	m_constant_pool.clear();
	m_constant_pool_map.clear();
	m_vreg_to_fpreg.clear();
	m_fpreg_to_vreg.clear();
	m_vreg_types.clear();
	m_string_constants = &program.string_constants;

	// Initialize free FP register pool (ft0-ft11 = f8-f19, excluding fa0-fa7)
	// FP registers: f0-f7 are args/return, f8-f19 are temps, f20-f31 are saved
	m_free_fp_registers = {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};

	// Emit a STOP instruction at entry point (offset 0)
	// This prevents accidental execution when ELF is loaded
	// STOP is encoded as: SYSTEM instruction (I-type) with imm[11:0] = 0x7ff
	// SYSTEM opcode = 0x73, funct3 = 0, rs1 = 0, rd = 0, imm = 0x7ff
	emit_i_type(0x73, 0, 0, 0, 0x7ff);

	// Generate code for each function
	for (const auto& func : program.functions) {
		m_functions[func.name] = m_code.size();
		// Also register function name as a label for CALL instructions
		m_labels[func.name] = m_code.size();
		gen_function(func);
	}

	// Define constant pool labels at the end of code
	// Constants are appended after the code section
	size_t const_pool_base = m_code.size();
	for (size_t i = 0; i < m_constant_pool.size(); i++) {
		std::string label = ".LC" + std::to_string(i);
		m_labels[label] = const_pool_base + (i * 8);
	}

	// Resolve all label references
	resolve_labels();

	return m_code;
}

void RISCVCodeGen::gen_function(const IRFunction& func) {
	// Godot Sandbox calling convention with Variants:
	// a0 = pointer to return Variant (pre-allocated by caller)
	// a1-a7 = pointers to argument Variants
	m_variant_offsets.clear();
	m_vreg_to_fpreg.clear();
	m_fpreg_to_vreg.clear();
	m_vreg_types.clear();
	m_num_params = func.parameters.size();
	m_next_variant_slot = 0;
	m_stack_frame_size = 0;
	m_current_instr_idx = 0;

	// Re-initialize free FP register pool for this function
	m_free_fp_registers = {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};

	// Initialize register allocator
	m_allocator.init(func);

	// Calculate stack frame size
	// Need space for: saved registers (24 bytes) + space for Variants
	int saved_reg_space = 24; // Save ra, fp, and a0 (return pointer)

	// Pre-allocate stack offsets for ALL virtual registers in order
	// This is CRITICAL: we must assign offsets deterministically based on
	// virtual register number, not based on the order instructions are visited.
	// Otherwise, optimizations that reorder or eliminate instructions will
	// cause different virtual registers to get different offsets, breaking the code.
	int max_variants = func.max_registers;
	int variant_space = max_variants * VARIANT_SIZE;

	// Pre-assign all virtual register offsets
	for (int vreg = 0; vreg < max_variants; vreg++) {
		int offset = saved_reg_space + (vreg * VARIANT_SIZE);
		m_variant_offsets[vreg] = offset;
	}
	m_next_variant_slot = max_variants;

	m_stack_frame_size = saved_reg_space + variant_space;

	// Align to 16 bytes (RISC-V ABI requirement)
	m_stack_frame_size = (m_stack_frame_size + 15) & ~15;

	// Function prologue - allocate stack frame
	// addi sp, sp, -frame_size
	if (m_stack_frame_size > 0) {
		if (m_stack_frame_size < 2048) {
			emit_i_type(0x13, REG_SP, 0, REG_SP, -m_stack_frame_size);
		} else {
			emit_li(REG_T0, -m_stack_frame_size);
			emit_add(REG_SP, REG_SP, REG_T0);
		}
	}

	// Save return address: sd ra, 0(sp)
	emit_sd(REG_RA, REG_SP, 0); // SD

	// Save frame pointer: sd fp, 8(sp)
	emit_sd(REG_FP, REG_SP, 8); // SD

	// Save a0 (return Variant pointer): sd a0, 16(sp)
	emit_sd(REG_A0, REG_SP, 16); // SD

	// Set frame pointer: addi fp, sp, frame_size
	if (m_stack_frame_size < 2048) {
		emit_i_type(0x13, REG_FP, 0, REG_SP, m_stack_frame_size);
	} else {
		emit_li(REG_T0, m_stack_frame_size);
		emit_add(REG_FP, REG_SP, REG_T0);
	}

	// Copy parameter Variants from argument registers to stack
	// Parameters come in a1-a7 as POINTERS to Variants
	for (size_t i = 0; i < m_num_params && i < 7; i++) {
		int param_vreg = static_cast<int>(i); // Parameters map to virtual registers 0-6
		int dst_offset = get_variant_stack_offset(param_vreg);
		uint8_t arg_reg = REG_A1 + static_cast<uint8_t>(i);

		// Copy 24 bytes from pointer in arg_reg to stack
		for (int j = 0; j < 3; j++) {
			emit_ld(REG_T0, arg_reg, j * 8); // LD t0, j*8(arg_reg)
			emit_sd(REG_T0, REG_SP, dst_offset + j * 8); // SD t0, offset(sp)
		}
	}

	// Process each IR instruction
	for (const auto& instr : func.instructions) {
		m_current_instr_idx++;
		
		switch (instr.opcode) {
			case IROpcode::LABEL:
				define_label(std::get<std::string>(instr.operands[0].value));
				break;

			case IROpcode::LOAD_IMM: {
				int vreg = std::get<int>(instr.operands[0].value);
				int64_t value = std::get<int64_t>(instr.operands[1].value);

				if (instr.type_hint == IRInstruction::TypeHint::RAW_INT) {
					// Optimization: keep as raw integer in physical register
					int preg = m_allocator.allocate_register(vreg, m_current_instr_idx);
					if (preg >= 0) {
						emit_li(preg, value);
						m_vreg_types[vreg] = ValueType::RAW_INT;

						// Also materialize to Variant on stack so it's accessible
						int stack_offset = get_variant_stack_offset(vreg);
						emit_li(REG_T0, 2); // Variant::INT type
						emit_store_variant_type(REG_T0, REG_SP, stack_offset);
						emit_store_variant_int(preg, REG_SP, stack_offset);
					} else {
						// No physical register available, fallback to Variant on stack
						int stack_offset = get_variant_stack_offset(vreg);
						emit_variant_create_int(stack_offset, value);
						m_vreg_types[vreg] = ValueType::VARIANT;
					}
				} else {
					// Standard: create integer Variant on stack
					int stack_offset = get_variant_stack_offset(vreg);
					emit_variant_create_int(stack_offset, value);
					m_vreg_types[vreg] = ValueType::VARIANT;
				}
				break;
			}

			case IROpcode::LOAD_FLOAT_IMM: {
				int vreg = std::get<int>(instr.operands[0].value);
				double value = std::get<double>(instr.operands[1].value);

				// Convert double to bit pattern for storage
				int64_t bits;
				memcpy(&bits, &value, sizeof(double));

				int stack_offset = get_variant_stack_offset(vreg);

				// Try RAW_FLOAT optimization when type hint indicates a float value
				// This includes both RAW_FLOAT and VARIANT_FLOAT (backend-driven optimization)
				bool is_float_type = (instr.type_hint == IRInstruction::TypeHint::RAW_FLOAT ||
				                     instr.type_hint == IRInstruction::TypeHint::VARIANT_FLOAT);

				if (is_float_type) {
					// Optimization: try to keep float value in FP register
					int fpreg = allocate_fp_register(vreg);

					if (fpreg >= 0) {
						// Load the 64-bit double into FP register
						// Use constant pool and FLD to load
						size_t const_idx = add_constant(bits);
						std::string label = ".LC" + std::to_string(const_idx);

						// Load address of constant into integer register, then FLD
						emit_la(REG_T0, label);
						emit_fld(fpreg, REG_T0, 0);

						m_vreg_types[vreg] = ValueType::RAW_FLOAT;

						// Also materialize to Variant on stack so it's accessible
						emit_li(REG_T0, 3); // Variant::FLOAT type
						emit_sw(REG_T0, REG_SP, stack_offset);
						emit_fsd(fpreg, REG_SP, stack_offset + 8);
					} else {
						// No FP register available, fallback to Variant on stack
						emit_li(REG_T0, 3); // Variant::FLOAT type
						emit_sw(REG_T0, REG_SP, stack_offset);
						emit_li(REG_T0, bits);
						emit_sd(REG_T0, REG_SP, stack_offset + 8);
						m_vreg_types[vreg] = ValueType::VARIANT;
					}
				} else {
					// Standard: create float Variant on stack
					emit_li(REG_T0, 3); // Variant::FLOAT type
					emit_sw(REG_T0, REG_SP, stack_offset);
					emit_li(REG_T0, bits);
					emit_sd(REG_T0, REG_SP, stack_offset + 8);
					m_vreg_types[vreg] = ValueType::VARIANT;
				}
				break;
			}

			case IROpcode::LOAD_BOOL: {
				int vreg = std::get<int>(instr.operands[0].value);
				int64_t value = std::get<int64_t>(instr.operands[1].value);

				// Allocate register for Variant pointer (stack offset)
				int preg = m_allocator.allocate_register(vreg, m_current_instr_idx);
				int stack_offset = get_variant_stack_offset(vreg);

				// Create boolean Variant at stack offset
				emit_variant_create_bool(stack_offset, value != 0);

				// If in register, load stack offset (pointer) into register
				if (preg >= 0) {
					// Load stack offset into register: addi preg, sp, stack_offset
					if (stack_offset < 2048) {
						emit_i_type(0x13, preg, 0, REG_SP, stack_offset);
					} else {
						emit_li(REG_T0, stack_offset);
						emit_add(preg, REG_SP, REG_T0);
					}
				}
				break;
			}

			case IROpcode::LOAD_STRING: {
				int vreg = std::get<int>(instr.operands[0].value);
				int64_t string_idx = std::get<int64_t>(instr.operands[1].value);

				// Allocate register for Variant pointer (stack offset)
				int preg = m_allocator.allocate_register(vreg, m_current_instr_idx);
				int stack_offset = get_variant_stack_offset(vreg);

				// Create string Variant at stack offset
				emit_variant_create_string(stack_offset, static_cast<int>(string_idx));

				// If in register, load stack offset (pointer) into register
				if (preg >= 0) {
					// Load stack offset into register: addi preg, sp, stack_offset
					if (stack_offset < 2048) {
						emit_i_type(0x13, preg, 0, REG_SP, stack_offset);
					} else {
						emit_li(REG_T0, stack_offset);
						emit_add(preg, REG_SP, REG_T0);
					}
				}
				break;
			}

			case IROpcode::MOVE: {
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int src_vreg = std::get<int>(instr.operands[1].value);

				// Skip no-op moves
				if (dst_vreg == src_vreg) {
					break;
				}

				// Check if source is RAW_INT
				auto src_type_it = m_vreg_types.find(src_vreg);
				ValueType src_type = (src_type_it != m_vreg_types.end()) ? src_type_it->second : ValueType::VARIANT;

				if (instr.type_hint == IRInstruction::TypeHint::RAW_INT && src_type == ValueType::RAW_INT) {
					// Both source and dest are raw integers - use register move
					int dst_preg = m_allocator.allocate_register(dst_vreg, m_current_instr_idx);
					int src_preg = m_allocator.get_physical_register(src_vreg);

					if (dst_preg >= 0 && src_preg >= 0) {
						emit_mv(dst_preg, src_preg);
						m_vreg_types[dst_vreg] = ValueType::RAW_INT;

						// Also materialize to Variant on stack so LOAD_VAR can access it
						int dst_offset = get_variant_stack_offset(dst_vreg);
						emit_li(REG_T0, 2); // Variant::INT type
						emit_store_variant_type(REG_T0, REG_SP, dst_offset);
						emit_store_variant_int(dst_preg, REG_SP, dst_offset);
					} else {
						// Fallback to Variant copy if registers unavailable
						int dst_offset = get_variant_stack_offset(dst_vreg);
						int src_offset = get_variant_stack_offset(src_vreg);
						if (dst_offset != src_offset) {
							emit_variant_copy(dst_offset, src_offset);
						}
						m_vreg_types[dst_vreg] = ValueType::VARIANT;
					}
				} else {
					// Standard Variant copy
					int dst_offset = get_variant_stack_offset(dst_vreg);
					int src_offset = get_variant_stack_offset(src_vreg);

					// Skip if source and destination are at same stack location
					if (dst_offset == src_offset) {
						break;
					}

					// Copy Variant: use sys_vclone or memcpy (24 bytes)
					emit_variant_copy(dst_offset, src_offset);
					m_vreg_types[dst_vreg] = ValueType::VARIANT;
				}
				break;
			}

			case IROpcode::ADD:
			case IROpcode::SUB:
			case IROpcode::MUL:
			case IROpcode::DIV:
			case IROpcode::MOD: {
				// Check that all operands are valid before processing
				if (instr.operands.size() < 3 ||
				    instr.operands[0].type != IRValue::Type::REGISTER) {
					throw std::runtime_error("Arithmetic operations require at least 3 operands with first being REGISTER");
				}

				int dst_vreg = std::get<int>(instr.operands[0].value);
				int lhs_vreg = 0;
				int rhs_vreg = 0;
				std::unordered_map<int, ValueType>::iterator lhs_type_it, rhs_type_it;
				ValueType lhs_type, rhs_type;

				// Check if operands are RAW_INT or IMMEDIATE
				bool lhs_is_reg = (instr.operands[1].type == IRValue::Type::REGISTER);
				bool rhs_is_reg = (instr.operands[2].type == IRValue::Type::REGISTER);

				// For now, if any operand is not a register, fall back to variant arithmetic
				// TODO: Optimize for register + immediate cases
				if (!lhs_is_reg || !rhs_is_reg) {
					goto variant_arithmetic;
				}

				lhs_vreg = std::get<int>(instr.operands[1].value);
				rhs_vreg = std::get<int>(instr.operands[2].value);

				lhs_type_it = m_vreg_types.find(lhs_vreg);
				rhs_type_it = m_vreg_types.find(rhs_vreg);
				lhs_type = (lhs_type_it != m_vreg_types.end()) ? lhs_type_it->second : ValueType::VARIANT;
				rhs_type = (rhs_type_it != m_vreg_types.end()) ? rhs_type_it->second : ValueType::VARIANT;

				if (instr.type_hint == IRInstruction::TypeHint::RAW_INT &&
				    lhs_type == ValueType::RAW_INT && rhs_type == ValueType::RAW_INT) {
					// Optimization: use native RISC-V integer arithmetic
					int dst_preg = m_allocator.allocate_register(dst_vreg, m_current_instr_idx);
					int lhs_preg = m_allocator.get_physical_register(lhs_vreg);
					int rhs_preg = m_allocator.get_physical_register(rhs_vreg);

					if (dst_preg >= 0 && lhs_preg >= 0 && rhs_preg >= 0) {
						// Emit native RISC-V arithmetic instruction
						if (instr.opcode == IROpcode::ADD) {
							emit_add(dst_preg, lhs_preg, rhs_preg);
						} else if (instr.opcode == IROpcode::SUB) {
							emit_sub(dst_preg, lhs_preg, rhs_preg);
						} else {
							// MUL/DIV/MOD need special handling or fallback
							// For now, fallback to VEVAL for complex ops
							goto variant_arithmetic;
						}
						m_vreg_types[dst_vreg] = ValueType::RAW_INT;

						// Also materialize to Variant on stack so it's accessible
						int dst_offset = get_variant_stack_offset(dst_vreg);
						emit_li(REG_T0, 2); // Variant::INT type
						emit_store_variant_type(REG_T0, REG_SP, dst_offset);
						emit_store_variant_int(dst_preg, REG_SP, dst_offset);
					} else {
						goto variant_arithmetic;
					}
				} else if (lhs_type == ValueType::RAW_FLOAT && rhs_type == ValueType::RAW_FLOAT) {
					// Optimization: use native RISC-V FP arithmetic for doubles
					int dst_fpreg = allocate_fp_register(dst_vreg);
					int lhs_fpreg = get_fp_register(lhs_vreg);
					int rhs_fpreg = get_fp_register(rhs_vreg);

					if (dst_fpreg >= 0 && lhs_fpreg >= 0 && rhs_fpreg >= 0) {
						// Emit native RISC-V FP arithmetic instruction
						if (instr.opcode == IROpcode::ADD) {
							emit_fadd_d(dst_fpreg, lhs_fpreg, rhs_fpreg);
						} else if (instr.opcode == IROpcode::SUB) {
							emit_fsub_d(dst_fpreg, lhs_fpreg, rhs_fpreg);
						} else if (instr.opcode == IROpcode::MUL) {
							emit_fmul_d(dst_fpreg, lhs_fpreg, rhs_fpreg);
						} else if (instr.opcode == IROpcode::DIV) {
							emit_fdiv_d(dst_fpreg, lhs_fpreg, rhs_fpreg);
						} else {
							// MOD not supported for FP, fallback to VEVAL
							goto variant_arithmetic;
						}
						m_vreg_types[dst_vreg] = ValueType::RAW_FLOAT;

						// Also materialize to Variant on stack so it's accessible
						int dst_offset = get_variant_stack_offset(dst_vreg);
						emit_li(REG_T0, 3); // Variant::FLOAT type
						emit_sw(REG_T0, REG_SP, dst_offset);
						emit_fsd(dst_fpreg, REG_SP, dst_offset + 8);
					} else {
						goto variant_arithmetic;
					}
				} else {
					variant_arithmetic:
					// Standard Variant arithmetic through VEVAL
					int dst_offset = get_variant_stack_offset(dst_vreg);

					// Map IR opcode to Variant::Operator
					int variant_op;
					switch (instr.opcode) {
						case IROpcode::ADD: variant_op = 6; break;  // OP_ADD
						case IROpcode::SUB: variant_op = 7; break;  // OP_SUBTRACT
						case IROpcode::MUL: variant_op = 8; break;  // OP_MULTIPLY
						case IROpcode::DIV: variant_op = 9; break;  // OP_DIVIDE
						case IROpcode::MOD: variant_op = 12; break; // OP_MODULE
						default: variant_op = 6; break;
					}

					// Handle different operand types (register vs immediate)
					if (lhs_is_reg && rhs_is_reg) {
						int lhs_vreg_local = std::get<int>(instr.operands[1].value);
						int rhs_vreg_local = std::get<int>(instr.operands[2].value);
						int lhs_offset = get_variant_stack_offset(lhs_vreg_local);
						int rhs_offset = get_variant_stack_offset(rhs_vreg_local);
						emit_variant_eval(dst_offset, lhs_offset, rhs_offset, variant_op);
					} else if (lhs_is_reg && !rhs_is_reg && instr.operands[2].type == IRValue::Type::IMMEDIATE) {
						// Left operand is register, right is immediate
						int lhs_vreg_local = std::get<int>(instr.operands[1].value);
						int64_t imm_val = std::get<int64_t>(instr.operands[2].value);
						int lhs_offset = get_variant_stack_offset(lhs_vreg_local);

						// Create a temporary Variant for the immediate value
						int imm_vreg = m_next_variant_slot++;
						int imm_offset = get_variant_stack_offset(imm_vreg);
						emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
						emit_variant_eval(dst_offset, lhs_offset, imm_offset, variant_op);
					} else if (!lhs_is_reg && rhs_is_reg && instr.operands[1].type == IRValue::Type::IMMEDIATE) {
						// Left operand is immediate, right is register
						int64_t imm_val = std::get<int64_t>(instr.operands[1].value);
						int rhs_vreg_local = std::get<int>(instr.operands[2].value);
						int rhs_offset = get_variant_stack_offset(rhs_vreg_local);

						// Create a temporary Variant for the immediate value
						int imm_vreg = m_next_variant_slot++;
						int imm_offset = get_variant_stack_offset(imm_vreg);
						emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
						emit_variant_eval(dst_offset, imm_offset, rhs_offset, variant_op);
					} else {
						throw std::runtime_error("Unsupported operand types for arithmetic operation");
					}

					m_vreg_types[dst_vreg] = ValueType::VARIANT;
				}
				break;
			}

			case IROpcode::NEG: {
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int src_vreg = std::get<int>(instr.operands[1].value);

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int src_offset = get_variant_stack_offset(src_vreg);

				// Use OP_NEGATE (unary operation - use veval with same operand twice)
				// Actually for unary, we need a different approach - create a zero Variant
				// For now, use subtract: 0 - src
				// TODO: Add proper unary operation support
				int zero_vreg = m_next_variant_slot++;
				int zero_offset = get_variant_stack_offset(zero_vreg);
				emit_variant_create_int(zero_offset, 0);
				emit_variant_eval(dst_offset, zero_offset, src_offset, 7); // OP_SUBTRACT
				break;
			}

			case IROpcode::CMP_EQ:
			case IROpcode::CMP_NEQ:
			case IROpcode::CMP_LT:
			case IROpcode::CMP_LTE:
			case IROpcode::CMP_GT:
			case IROpcode::CMP_GTE: {
				// Check that all operands are valid
				if (instr.operands.size() < 3 ||
				    instr.operands[0].type != IRValue::Type::REGISTER) {
					throw std::runtime_error("Comparison operations require at least 3 operands with first being REGISTER");
				}

				int dst_vreg = std::get<int>(instr.operands[0].value);
				int lhs_vreg = 0;
				int rhs_vreg = 0;
				std::unordered_map<int, ValueType>::iterator lhs_type_it, rhs_type_it;
				ValueType lhs_type, rhs_type;

				// Check if operands are RAW_INT or IMMEDIATE
				bool lhs_is_reg = (instr.operands[1].type == IRValue::Type::REGISTER);
				bool rhs_is_reg = (instr.operands[2].type == IRValue::Type::REGISTER);

				// For now, if any operand is not a register, fall back to variant comparison
				if (!lhs_is_reg || !rhs_is_reg) {
					goto variant_comparison;
				}

				lhs_vreg = std::get<int>(instr.operands[1].value);
				rhs_vreg = std::get<int>(instr.operands[2].value);

				// Check if operands are RAW_INT
				lhs_type_it = m_vreg_types.find(lhs_vreg);
				rhs_type_it = m_vreg_types.find(rhs_vreg);
				lhs_type = (lhs_type_it != m_vreg_types.end()) ? lhs_type_it->second : ValueType::VARIANT;
				rhs_type = (rhs_type_it != m_vreg_types.end()) ? rhs_type_it->second : ValueType::VARIANT;

				if (instr.type_hint == IRInstruction::TypeHint::RAW_INT &&
				    lhs_type == ValueType::RAW_INT && rhs_type == ValueType::RAW_INT) {
					// Optimization: use native RISC-V integer comparison
					// Result is stored as RAW_INT (0 or 1) in a register
					int dst_preg = m_allocator.allocate_register(dst_vreg, m_current_instr_idx);
					int lhs_preg = m_allocator.get_physical_register(lhs_vreg);
					int rhs_preg = m_allocator.get_physical_register(rhs_vreg);

					if (dst_preg >= 0 && lhs_preg >= 0 && rhs_preg >= 0) {
						// Use native RISC-V comparison instructions
						// Set dst_preg = 1 if condition is true, 0 otherwise
						switch (instr.opcode) {
							case IROpcode::CMP_LT:
								// slt rd, rs1, rs2: rd = (rs1 < rs2) ? 1 : 0
								emit_slt(dst_preg, lhs_preg, rhs_preg);
								break;
							case IROpcode::CMP_GT:
								// slt rd, rs2, rs1: rd = (rs2 < rs1) ? 1 : 0 = (rs1 > rs2)
								emit_slt(dst_preg, rhs_preg, lhs_preg);
								break;
							case IROpcode::CMP_LTE:
								// rs1 <= rs2 is !(rs2 < rs1)
								emit_slt(dst_preg, rhs_preg, lhs_preg);
								emit_xori(dst_preg, dst_preg, 1); // Flip the result
								break;
							case IROpcode::CMP_GTE:
								// rs1 >= rs2 is !(rs1 < rs2)
								emit_slt(dst_preg, lhs_preg, rhs_preg);
								emit_xori(dst_preg, dst_preg, 1); // Flip the result
								break;
							case IROpcode::CMP_EQ:
								// rs1 == rs2: xor rd, rs1, rs2; seqz rd, rd
								emit_xor(dst_preg, lhs_preg, rhs_preg);
								emit_seqz(dst_preg, dst_preg);
								break;
							case IROpcode::CMP_NEQ:
								// rs1 != rs2: xor rd, rs1, rs2; snez rd, rd
								emit_xor(dst_preg, lhs_preg, rhs_preg);
								emit_snez(dst_preg, dst_preg);
								break;
							default:
								goto variant_comparison;
						}
						m_vreg_types[dst_vreg] = ValueType::RAW_INT;
					} else {
						goto variant_comparison;
					}
				} else {
					variant_comparison:
					// Standard Variant comparison through VEVAL
					int dst_offset = get_variant_stack_offset(dst_vreg);

					// Map IR opcode to Variant::Operator
					int variant_op;
					switch (instr.opcode) {
						case IROpcode::CMP_EQ:  variant_op = 0; break; // OP_EQUAL
						case IROpcode::CMP_NEQ: variant_op = 1; break; // OP_NOT_EQUAL
						case IROpcode::CMP_LT:  variant_op = 2; break; // OP_LESS
						case IROpcode::CMP_LTE: variant_op = 3; break; // OP_LESS_EQUAL
						case IROpcode::CMP_GT:  variant_op = 4; break; // OP_GREATER
						case IROpcode::CMP_GTE: variant_op = 5; break; // OP_GREATER_EQUAL
						default: variant_op = 0; break;
					}

					// Handle different operand types (register vs immediate)
					if (lhs_is_reg && rhs_is_reg) {
						int lhs_vreg = std::get<int>(instr.operands[1].value);
						int rhs_vreg = std::get<int>(instr.operands[2].value);
						int lhs_offset = get_variant_stack_offset(lhs_vreg);
						int rhs_offset = get_variant_stack_offset(rhs_vreg);
						emit_variant_eval(dst_offset, lhs_offset, rhs_offset, variant_op);
					} else if (lhs_is_reg && !rhs_is_reg && instr.operands[2].type == IRValue::Type::IMMEDIATE) {
						// Left operand is register, right is immediate
						int lhs_vreg = std::get<int>(instr.operands[1].value);
						int64_t imm_val = std::get<int64_t>(instr.operands[2].value);
						int lhs_offset = get_variant_stack_offset(lhs_vreg);

						// Create a temporary Variant for the immediate value
						int imm_vreg = m_next_variant_slot++;
						int imm_offset = get_variant_stack_offset(imm_vreg);
						emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
						emit_variant_eval(dst_offset, lhs_offset, imm_offset, variant_op);
					} else if (!lhs_is_reg && rhs_is_reg && instr.operands[1].type == IRValue::Type::IMMEDIATE) {
						// Left operand is immediate, right is register
						int64_t imm_val = std::get<int64_t>(instr.operands[1].value);
						int rhs_vreg = std::get<int>(instr.operands[2].value);
						int rhs_offset = get_variant_stack_offset(rhs_vreg);

						// Create a temporary Variant for the immediate value
						int imm_vreg = m_next_variant_slot++;
						int imm_offset = get_variant_stack_offset(imm_vreg);
						emit_variant_create_int(imm_offset, static_cast<int>(imm_val));
						emit_variant_eval(dst_offset, imm_offset, rhs_offset, variant_op);
					} else {
						throw std::runtime_error("Unsupported operand types for comparison operation");
					}

					m_vreg_types[dst_vreg] = ValueType::VARIANT;
				}
				break;
			}

			case IROpcode::AND: {
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int lhs_vreg = std::get<int>(instr.operands[1].value);
				int rhs_vreg = std::get<int>(instr.operands[2].value);

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int lhs_offset = get_variant_stack_offset(lhs_vreg);
				int rhs_offset = get_variant_stack_offset(rhs_vreg);

				emit_variant_eval(dst_offset, lhs_offset, rhs_offset, 18); // OP_AND
				break;
			}

			case IROpcode::OR: {
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int lhs_vreg = std::get<int>(instr.operands[1].value);
				int rhs_vreg = std::get<int>(instr.operands[2].value);

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int lhs_offset = get_variant_stack_offset(lhs_vreg);
				int rhs_offset = get_variant_stack_offset(rhs_vreg);

				emit_variant_eval(dst_offset, lhs_offset, rhs_offset, 19); // OP_OR
				break;
			}

			case IROpcode::NOT: {
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int src_vreg = std::get<int>(instr.operands[1].value);

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int src_offset = get_variant_stack_offset(src_vreg);

				// OP_NOT - use veval (unary operations need special handling)
				// For now, use the same operand for both sides
				emit_variant_eval(dst_offset, src_offset, src_offset, 21); // OP_NOT
				break;
			}

			case IROpcode::BRANCH_ZERO: {
				int vreg = std::get<int>(instr.operands[0].value);

				// Check if value is RAW_INT in a register
				auto vreg_type_it = m_vreg_types.find(vreg);
				ValueType vreg_type = (vreg_type_it != m_vreg_types.end()) ? vreg_type_it->second : ValueType::VARIANT;

				if (vreg_type == ValueType::RAW_INT) {
					// Optimization: value is in a physical register
					int preg = m_allocator.get_physical_register(vreg);
					if (preg >= 0) {
						// Branch if register is zero
						mark_label_use(std::get<std::string>(instr.operands[1].value), m_code.size());
						emit_beq(preg, REG_ZERO, 0);
					} else {
						// Fallback to Variant load if not in register
						int offset = get_variant_stack_offset(vreg);
						emit_load_variant_bool(REG_T0, REG_SP, offset);
						mark_label_use(std::get<std::string>(instr.operands[1].value), m_code.size());
						emit_beq(REG_T0, REG_ZERO, 0);
					}
				} else {
					// Standard: load from Variant
					int offset = get_variant_stack_offset(vreg);
					emit_load_variant_bool(REG_T0, REG_SP, offset);
					mark_label_use(std::get<std::string>(instr.operands[1].value), m_code.size());
					emit_beq(REG_T0, REG_ZERO, 0);
				}
				break;
			}

			case IROpcode::BRANCH_NOT_ZERO: {
				int vreg = std::get<int>(instr.operands[0].value);

				// Check if value is RAW_INT in a register
				auto vreg_type_it = m_vreg_types.find(vreg);
				ValueType vreg_type = (vreg_type_it != m_vreg_types.end()) ? vreg_type_it->second : ValueType::VARIANT;

				if (vreg_type == ValueType::RAW_INT) {
					// Optimization: value is in a physical register
					int preg = m_allocator.get_physical_register(vreg);
					if (preg >= 0) {
						// Branch if register is not zero
						mark_label_use(std::get<std::string>(instr.operands[1].value), m_code.size());
						emit_bne(preg, REG_ZERO, 0);
					} else {
						// Fallback to Variant load if not in register
						int offset = get_variant_stack_offset(vreg);
						emit_load_variant_bool(REG_T0, REG_SP, offset);
						mark_label_use(std::get<std::string>(instr.operands[1].value), m_code.size());
						emit_bne(REG_T0, REG_ZERO, 0);
					}
				} else {
					// Standard: load from Variant
					int offset = get_variant_stack_offset(vreg);
					emit_load_variant_bool(REG_T0, REG_SP, offset);
					mark_label_use(std::get<std::string>(instr.operands[1].value), m_code.size());
					emit_bne(REG_T0, REG_ZERO, 0);
				}
				break;
			}

			case IROpcode::JUMP:
				mark_label_use(std::get<std::string>(instr.operands[0].value), m_code.size());
				emit_jal(REG_ZERO, 0);
				break;

			case IROpcode::RETURN: {
				// Godot Sandbox calling convention with Variants:
				// a0 points to pre-allocated Variant for return value
				// Copy the return Variant (virtual register 0) to *a0

				// First, restore a0 from stack (it may have been clobbered by syscalls)
				emit_ld(REG_A0, REG_SP, 16); // LD a0, 16(sp)

				if (m_variant_offsets.find(0) != m_variant_offsets.end()) {
					// We have a return value in virtual register 0
					int src_offset = get_variant_stack_offset(0);

					// Copy 24 bytes from stack to *a0
					// Load address of source Variant: addi t0, sp, src_offset
					if (src_offset < 2048) {
						emit_i_type(0x13, REG_T0, 0, REG_SP, src_offset);
					} else {
						emit_li(REG_T0, src_offset);
						emit_add(REG_T0, REG_SP, REG_T0);
					}

					// Copy 24 bytes (3 8-byte loads/stores)
					for (int i = 0; i < 3; i++) {
						emit_ld(REG_T1, REG_T0, i * 8); // LD t1, i*8(t0)
						emit_sd(REG_T1, REG_A0, i * 8); // SD t1, i*8(a0)
					}
				}

				// Function epilogue - restore registers and deallocate stack
				// Restore return address: ld ra, 0(sp)
				emit_ld(REG_RA, REG_SP, 0); // LD

				// Restore frame pointer: ld fp, 8(sp)
				emit_ld(REG_FP, REG_SP, 8); // LD

				// Deallocate stack: addi sp, sp, frame_size
				if (m_stack_frame_size > 0) {
					if (m_stack_frame_size < 2048) {
						emit_i_type(0x13, REG_SP, 0, REG_SP, m_stack_frame_size);
					} else {
						emit_li(REG_T0, m_stack_frame_size);
						emit_add(REG_SP, REG_SP, REG_T0);
					}
				}

				emit_ret();
				break;
			}

			case IROpcode::VCALL: {
				// VCALL format: result_reg, obj_reg, method_name, arg_count, arg1_reg, arg2_reg, ...
				if (instr.operands.size() < 4) {
					throw std::runtime_error("VCALL requires at least 4 operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int obj_vreg = std::get<int>(instr.operands[1].value);
				std::string method_name = std::get<std::string>(instr.operands[2].value);
				int arg_count = static_cast<int>(std::get<int64_t>(instr.operands[3].value));

				if (instr.operands.size() != static_cast<size_t>(4 + arg_count)) {
					throw std::runtime_error("VCALL argument count mismatch");
				}

				int result_offset = get_variant_stack_offset(result_vreg);
				int obj_offset = get_variant_stack_offset(obj_vreg);

				// VCALL clobbers a0-a7, handle register clobbering
				std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7};
				auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_current_instr_idx);

				for (const auto& move : moves) {
					emit_mv(move.second, move.first);
				}

				// If we have arguments, allocate stack space for argument array
				int args_stack_offset = 0;
				if (arg_count > 0) {
					args_stack_offset = m_stack_frame_size - (arg_count * VARIANT_SIZE);
					// Expand stack if needed
					int additional_space = arg_count * VARIANT_SIZE;
					additional_space = (additional_space + 15) & ~15; // Align to 16 bytes

					// Adjust stack pointer: addi sp, sp, -additional_space
					if (additional_space < 2048) {
						emit_i_type(0x13, REG_SP, 0, REG_SP, -additional_space);
					} else {
						emit_li(REG_T0, -additional_space);
						emit_add(REG_SP, REG_SP, REG_T0);
					}

					// Copy argument Variants to the new stack space
					for (int i = 0; i < arg_count; i++) {
						int arg_vreg = std::get<int>(instr.operands[4 + i].value);
						int arg_src_offset = get_variant_stack_offset(arg_vreg) + additional_space; // Adjust for moved stack
						int arg_dst_offset = i * VARIANT_SIZE;

						// Copy 24 bytes from src to dst (3 8-byte loads/stores)
						for (int j = 0; j < 3; j++) {
							// Load from source
							emit_ld(REG_T0, REG_SP, arg_src_offset + j * 8); // LD

							// Store to destination
							emit_sd(REG_T0, REG_SP, arg_dst_offset + j * 8); // SD
						}
					}

					// a3 = pointer to arguments array (sp + 0)
					emit_mv(REG_A3, REG_SP);
				} else {
					// No arguments, a3 = 0
					emit_mv(REG_A3, REG_ZERO);
				}

				// a0 = pointer to object Variant (sp + obj_offset + additional_space if we allocated stack)
				int adjusted_obj_offset = obj_offset;
				if (arg_count > 0) {
					int additional_space = arg_count * VARIANT_SIZE;
					additional_space = (additional_space + 15) & ~15;
					adjusted_obj_offset += additional_space;
				}

				if (adjusted_obj_offset < 2048) {
					emit_i_type(0x13, REG_A0, 0, REG_SP, adjusted_obj_offset);
				} else {
					emit_li(REG_A0, adjusted_obj_offset);
					emit_add(REG_A0, REG_SP, REG_A0);
				}

				// a1 = pointer to method name string (need to store in .rodata section)
				// For now, we'll use a temporary approach: store the string on stack
				// TODO: Better approach would be to use .rodata section
				int method_len = method_name.length();
				int str_space = ((method_len + 1) + 7) & ~7; // Align to 8 bytes, +1 for null terminator

				// Allocate more stack space for the string
				if (str_space < 2048) {
					emit_i_type(0x13, REG_SP, 0, REG_SP, -str_space);
				} else {
					emit_li(REG_T0, -str_space);
					emit_add(REG_SP, REG_SP, REG_T0);
				}

				// Store method name on stack
				for (size_t i = 0; i < method_name.length(); i++) {
					emit_li(REG_T0, static_cast<unsigned char>(method_name[i]));
					emit_sb(REG_T0, REG_SP, i); // SB (store byte)
				}
				// Store null terminator
				emit_sb(REG_ZERO, REG_SP, method_len); // SB

				// a1 = pointer to method name (sp)
				emit_mv(REG_A1, REG_SP);

				// a2 = method length
				emit_li(REG_A2, method_len);

				// a4 = argument count
				emit_li(REG_A4, arg_count);

				// a5 = pointer to result Variant
				int adjusted_result_offset = result_offset;
				if (arg_count > 0) {
					int additional_space = arg_count * VARIANT_SIZE;
					additional_space = (additional_space + 15) & ~15;
					adjusted_result_offset += additional_space;
				}
				adjusted_result_offset += str_space;

				if (adjusted_result_offset < 2048) {
					emit_i_type(0x13, REG_A5, 0, REG_SP, adjusted_result_offset);
				} else {
					emit_li(REG_A5, adjusted_result_offset);
					emit_add(REG_A5, REG_SP, REG_A5);
				}

				// a7 = ECALL_VCALL (501)
				emit_li(REG_A7, 501);
				emit_ecall();

				// Restore stack pointer
				int total_stack_adjust = str_space;
				if (arg_count > 0) {
					int additional_space = arg_count * VARIANT_SIZE;
					additional_space = (additional_space + 15) & ~15;
					total_stack_adjust += additional_space;
				}

				if (total_stack_adjust < 2048) {
					emit_i_type(0x13, REG_SP, 0, REG_SP, total_stack_adjust);
				} else {
					emit_li(REG_T0, total_stack_adjust);
					emit_add(REG_SP, REG_SP, REG_T0);
				}

				break;
			}

			case IROpcode::CALL: {
				// CALL format: function_name, result_reg, arg_count, arg1_reg, arg2_reg, ...
				if (instr.operands.size() < 3) {
					throw std::runtime_error("CALL requires at least 3 operands");
				}

				std::string func_name = std::get<std::string>(instr.operands[0].value);
				int result_vreg = std::get<int>(instr.operands[1].value);
				int arg_count = static_cast<int>(std::get<int64_t>(instr.operands[2].value));

				if (instr.operands.size() != static_cast<size_t>(3 + arg_count)) {
					throw std::runtime_error("CALL argument count mismatch");
				}

				// Handle register clobbering (calls clobber a0-a7, ra, and temporaries)
				std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7, REG_RA};
				auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_current_instr_idx);

				for (const auto& move : moves) {
					emit_mv(move.second, move.first);
				}

				// Allocate stack space for return value Variant
				int return_var_offset = get_variant_stack_offset(result_vreg);

				// Set up arguments: a1-a7 point to Variant arguments on stack
				// a0 points to return Variant
				for (int i = 0; i < arg_count && i < 7; i++) {
					int arg_vreg = std::get<int>(instr.operands[3 + i].value);
					int arg_offset = get_variant_stack_offset(arg_vreg);
					uint8_t arg_reg = REG_A1 + static_cast<uint8_t>(i);

					// Load address of argument Variant: addi arg_reg, sp, arg_offset
					if (arg_offset < 2048) {
						emit_i_type(0x13, arg_reg, 0, REG_SP, arg_offset);
					} else {
						emit_li(REG_T0, arg_offset);
						emit_add(arg_reg, REG_SP, REG_T0);
					}
				}

				// a0 = pointer to return Variant
				if (return_var_offset < 2048) {
					emit_i_type(0x13, REG_A0, 0, REG_SP, return_var_offset);
				} else {
					emit_li(REG_A0, return_var_offset);
					emit_add(REG_A0, REG_SP, REG_A0);
				}

				// Call the function using JAL with label
				// We'll use the function name as a label that will be resolved later
				mark_label_use(func_name, m_code.size());
				emit_jal(REG_RA, 0); // JAL ra, func_name

				// Return value is already in the Variant at result_vreg
				break;
			}

			// Inline primitive construction (no syscalls!)
			case IROpcode::MAKE_VECTOR2:
			case IROpcode::MAKE_VECTOR3:
			case IROpcode::MAKE_VECTOR4: {
				// Format: MAKE_VECTORn result_reg, x_reg, y_reg, [z_reg], [w_reg]
				int num_components = (instr.opcode == IROpcode::MAKE_VECTOR2) ? 2 :
				                     (instr.opcode == IROpcode::MAKE_VECTOR3) ? 3 : 4;

				if (instr.operands.size() != static_cast<size_t>(1 + num_components)) {
					throw std::runtime_error("MAKE_VECTOR requires correct number of operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int result_offset = get_variant_stack_offset(result_vreg);

				// Set m_type field (offset 0)
				int variant_type = (instr.opcode == IROpcode::MAKE_VECTOR2) ? 5 :  // VECTOR2
				                   (instr.opcode == IROpcode::MAKE_VECTOR3) ? 9 : 12; // VECTOR3 : VECTOR4
				emit_li(REG_T0, variant_type);
				emit_sw(REG_T0, REG_SP, result_offset); // Store type at offset 0

				// Store each component at offset 8, 12, 16, 20 (4 bytes per component as real_t=float)
				// The components are 64-bit doubles, need to convert to 32-bit float for storage
				for (int i = 0; i < num_components; i++) {
					int comp_vreg = std::get<int>(instr.operands[1 + i].value);

					// Check if component is RAW_FLOAT (in FP register)
					auto comp_type_it = m_vreg_types.find(comp_vreg);
					ValueType comp_type = (comp_type_it != m_vreg_types.end()) ? comp_type_it->second : ValueType::VARIANT;

					if (comp_type == ValueType::RAW_FLOAT) {
						// Use FP register directly
						int fpreg = get_fp_register(comp_vreg);
						if (fpreg >= 0) {
							// Convert double (64-bit) to float (32-bit)
							uint8_t temp_freg = REG_FA0; // Use a temporary
							emit_fcvt_s_d(temp_freg, fpreg);
							// Store 4-byte float to result at offset 8 + i*4
							emit_fsw(temp_freg, REG_SP, result_offset + 8 + i * 4);
						} else {
							// Fallback to stack load
							int comp_offset = get_variant_stack_offset(comp_vreg);
							emit_fld(REG_FA0, REG_SP, comp_offset + 8);
							emit_fcvt_s_d(REG_FA0, REG_FA0);
							emit_fsw(REG_FA0, REG_SP, result_offset + 8 + i * 4);
						}
					} else {
						// Load from stack Variant
						int comp_offset = get_variant_stack_offset(comp_vreg);
						emit_fld(REG_FA0, REG_SP, comp_offset + 8);
						emit_fcvt_s_d(REG_FA0, REG_FA0);
						emit_fsw(REG_FA0, REG_SP, result_offset + 8 + i * 4);
					}
				}

				m_vreg_types[result_vreg] = ValueType::VARIANT;
				break;
			}

			case IROpcode::MAKE_VECTOR2I:
			case IROpcode::MAKE_VECTOR3I:
			case IROpcode::MAKE_VECTOR4I: {
				// Format: MAKE_VECTORnI result_reg, x_reg, y_reg, [z_reg], [w_reg]
				int num_components = (instr.opcode == IROpcode::MAKE_VECTOR2I) ? 2 :
				                     (instr.opcode == IROpcode::MAKE_VECTOR3I) ? 3 : 4;

				if (instr.operands.size() != static_cast<size_t>(1 + num_components)) {
					throw std::runtime_error("MAKE_VECTORnI requires correct number of operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int result_offset = get_variant_stack_offset(result_vreg);

				// Set m_type field (offset 0)
				int variant_type = (instr.opcode == IROpcode::MAKE_VECTOR2I) ? 6 :  // VECTOR2I
				                   (instr.opcode == IROpcode::MAKE_VECTOR3I) ? 10 : 13; // VECTOR3I : VECTOR4I
				emit_li(REG_T0, variant_type);
				emit_sw(REG_T0, REG_SP, result_offset); // Store type at offset 0

				// Store each component in v.v4i[] at offset 8, 12, 16, 20
				for (int i = 0; i < num_components; i++) {
					int comp_vreg = std::get<int>(instr.operands[1 + i].value);
					int comp_offset = get_variant_stack_offset(comp_vreg);

					// Load the component value (it's a Variant containing int)
					// Load v.i from offset 8 (lower 32 bits)
					emit_lw(REG_T0, REG_SP, comp_offset + 8);

					// Store to result v.v4i[i] at offset 8 + i*4
					emit_sw(REG_T0, REG_SP, result_offset + 8 + i * 4);
				}

				break;
			}

			case IROpcode::MAKE_COLOR: {
				// Format: MAKE_COLOR result_reg, r_reg, g_reg, b_reg, a_reg
				if (instr.operands.size() != 5) {
					throw std::runtime_error("MAKE_COLOR requires 5 operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int result_offset = get_variant_stack_offset(result_vreg);

				// Set m_type field to COLOR (20)
				emit_li(REG_T0, 20);
				emit_sw(REG_T0, REG_SP, result_offset);

				// Store each component (r, g, b, a) as 32-bit floats (real_t)
				// The components are FLOAT Variants with 64-bit double values at offset 8
				// We need to convert double to float and store 4 bytes
				for (int i = 0; i < 4; i++) {
					int comp_vreg = std::get<int>(instr.operands[1 + i].value);
					int comp_offset = get_variant_stack_offset(comp_vreg);

					// Load the 64-bit double value from the source FLOAT Variant (offset 8)
					emit_fld(REG_FA0, REG_SP, comp_offset + 8);

					// Convert double (64-bit) to float (32-bit)
					emit_fcvt_s_d(REG_FA0, REG_FA0);

					// Store 4-byte float to result at offset 8 + i*4
					emit_fsw(REG_FA0, REG_SP, result_offset + 8 + i * 4);
				}

				m_vreg_types[result_vreg] = ValueType::VARIANT;
				break;
			}

			case IROpcode::MAKE_ARRAY: {
				// Format: MAKE_ARRAY result_reg, element_count, [element_reg1, element_reg2, ...]
				// For empty arrays: element_count = 0, no element regs
				if (instr.operands.size() < 2) {
					throw std::runtime_error("MAKE_ARRAY requires at least 2 operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int element_count = static_cast<int>(std::get<int64_t>(instr.operands[1].value));
				int result_offset = get_variant_stack_offset(result_vreg);

				// Handle register clobbering (VCREATE uses a0-a3)
				// Note: t0-t1 are used here for copying, they're not clobbered by the syscall itself
				std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3};
				auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_current_instr_idx);
				for (const auto& move : moves) {
					emit_mv(move.second, move.first);
				}

				if (element_count == 0) {
					// Empty Array: sys_vcreate(&v, ARRAY, 0, nullptr)
					// a0 = pointer to destination Variant
					if (result_offset < 2048) {
						emit_i_type(0x13, REG_A0, 0, REG_SP, result_offset);
					} else {
						emit_li(REG_A0, result_offset);
						emit_add(REG_A0, REG_SP, REG_A0);
					}

					// a1 = Variant::ARRAY (28)
					emit_li(REG_A1, 28);

					// a2 = method (0 for empty)
					emit_li(REG_A2, 0);

					// a3 = nullptr (0)
					emit_li(REG_A3, 0);

					// a7 = ECALL_VCREATE (517)
					emit_li(REG_A7, 517);
					emit_ecall();
				} else {
					// Array with elements: sys_vcreate(&v, ARRAY, size, data_pointer)
					// GuestVariant is 24 bytes (4-byte type + 4-byte padding + 16-byte data)
					// We need to copy the full Variant structures to stack
					constexpr int VARIANT_SIZE = 24;
					int args_space = element_count * VARIANT_SIZE;
					args_space = (args_space + 15) & ~15; // Align to 16 bytes

					// Adjust stack pointer
					if (args_space < 2048) {
						emit_i_type(0x13, REG_SP, 0, REG_SP, -args_space);
					} else {
						emit_li(REG_T0, -args_space);
						emit_add(REG_SP, REG_SP, REG_T0);
					}

					// Copy full GuestVariant structures (24 bytes each) to stack
					for (int i = 0; i < element_count; i++) {
						int elem_vreg = std::get<int>(instr.operands[2 + i].value);
						int elem_offset = get_variant_stack_offset(elem_vreg);

						// Destination address for this element
						int dst_offset = i * VARIANT_SIZE;

						// The element Variant is at elem_offset from the ORIGINAL stack frame
						// After SP -= args_space, it's now at elem_offset + args_space from NEW SP
						// So we load from (elem_offset + args_space) and store to dst_offset

						// Copy 24 bytes: 8 + 8 + 8 = three 64-bit loads/stores
						// First 8 bytes (type + padding)
						emit_ld(REG_T0, REG_SP, elem_offset + args_space);
						emit_sd(REG_T0, REG_SP, dst_offset);

						// Second 8 bytes (data part 1)
						emit_ld(REG_T0, REG_SP, elem_offset + args_space + 8);
						emit_sd(REG_T0, REG_SP, dst_offset + 8);

						// Third 8 bytes (data part 2)
						emit_ld(REG_T0, REG_SP, elem_offset + args_space + 16);
						emit_sd(REG_T0, REG_SP, dst_offset + 16);
					}

					// a0 = pointer to destination Variant
					// The result Variant is at result_offset from the ORIGINAL stack frame
					// After SP -= args_space, it's now at result_offset + args_space from NEW SP
					int adjusted_dst_offset = result_offset + args_space;
					if (adjusted_dst_offset < 2048) {
						emit_i_type(0x13, REG_A0, 0, REG_SP, adjusted_dst_offset);
					} else {
						emit_li(REG_A0, adjusted_dst_offset);
						emit_add(REG_A0, REG_SP, REG_A0);
					}

					// a1 = Variant::ARRAY (28)
					emit_li(REG_A1, 28);

					// a2 = size (element_count)
					emit_li(REG_A2, element_count);

					// a3 = pointer to element array (sp + 0)
					emit_mv(REG_A3, REG_SP);

					// a7 = ECALL_VCREATE (517)
					emit_li(REG_A7, 517);
					emit_ecall();

					// Restore stack pointer
					if (args_space < 2048) {
						emit_i_type(0x13, REG_SP, 0, REG_SP, args_space);
					} else {
						emit_li(REG_T0, args_space);
						emit_add(REG_SP, REG_SP, REG_T0);
					}
				}

				m_vreg_types[result_vreg] = ValueType::VARIANT;
				break;
			}

			case IROpcode::MAKE_DICTIONARY: {
				// Format: MAKE_DICTIONARY result_reg
				// Empty Dictionary: sys_vcreate(&v, DICTIONARY, 0, nullptr)
				if (instr.operands.size() != 1) {
					throw std::runtime_error("MAKE_DICTIONARY requires 1 operand");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int result_offset = get_variant_stack_offset(result_vreg);

				// Handle register clobbering (VCREATE uses a0-a3)
				std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3};
				auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_current_instr_idx);
				for (const auto& move : moves) {
					emit_mv(move.second, move.first);
				}

				// a0 = pointer to destination Variant
				if (result_offset < 2048) {
					emit_i_type(0x13, REG_A0, 0, REG_SP, result_offset);
				} else {
					emit_li(REG_A0, result_offset);
					emit_add(REG_A0, REG_SP, REG_A0);
				}

				// a1 = Variant::DICTIONARY (27)
				emit_li(REG_A1, 27);

				// a2 = method (0 for empty)
				emit_li(REG_A2, 0);

				// a3 = nullptr (0)
				emit_li(REG_A3, 0);

				// a7 = ECALL_VCREATE (517)
				emit_li(REG_A7, 517);
				emit_ecall();

				m_vreg_types[result_vreg] = ValueType::VARIANT;
				break;
			}

			case IROpcode::VGET_INLINE: {
				// Format: VGET_INLINE result_reg, obj_reg, member_name, obj_type_hint
				if (instr.operands.size() != 4) {
					throw std::runtime_error("VGET_INLINE requires 4 operands");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int obj_vreg = std::get<int>(instr.operands[1].value);
				std::string member = std::get<std::string>(instr.operands[2].value);
				int obj_type_hint = static_cast<int>(std::get<int64_t>(instr.operands[3].value));

				int result_offset = get_variant_stack_offset(result_vreg);
				int obj_offset = get_variant_stack_offset(obj_vreg);

				// Determine the offset within the object Variant
				// Vector types with float components (real_t=float=4 bytes): offsets at 8, 12, 16, 20
				// Vector types with int components (int32_t=4 bytes): offsets at 8, 12, 16, 20
				int member_offset = 8; // Base offset
				bool is_int_type = false;

				// Map member name to array index
				int component_idx = 0;
				if (member == "x" || member == "r") component_idx = 0;
				else if (member == "y" || member == "g") component_idx = 1;
				else if (member == "z" || member == "b") component_idx = 2;
				else if (member == "w" || member == "a") component_idx = 3;

				// Check if it's an integer vector type using helper function
				IRInstruction::TypeHint hint = static_cast<IRInstruction::TypeHint>(obj_type_hint);
				is_int_type = TypeHintUtils::is_int_vector(hint);

				// All vector components are stored as 4-byte values
				member_offset += component_idx * 4;

				if (is_int_type) {
					// Load 32-bit integer from object
					emit_lw(REG_T0, REG_SP, obj_offset + member_offset);

					// Create result Variant with INT type (2)
					emit_li(REG_T1, 2);
					emit_sw(REG_T1, REG_SP, result_offset); // m_type = INT

					// Sign-extend to 64-bit and store in v.i
					emit_sext_w(REG_T0, REG_T0);
					emit_sd(REG_T0, REG_SP, result_offset + 8);
				} else {
					// For float components from Vector types:
					// Use FP instructions to convert 4-byte float to 8-byte double
					// FLW loads 32-bit float, FCVT.D.S converts to double

					// Try to allocate an FP register for RAW_FLOAT optimization
					int fpreg = allocate_fp_register(result_vreg);

					// Load 4-byte float component to FP register
					uint8_t load_freg = (fpreg >= 0) ? static_cast<uint8_t>(fpreg) : REG_FA0;
					emit_flw(load_freg, REG_SP, obj_offset + member_offset);

					// Convert float (32-bit) to double (64-bit)
					emit_fcvt_d_s(load_freg, load_freg);

					// Set result Variant type to FLOAT (3)
					emit_li(REG_T0, 3);
					emit_sw(REG_T0, REG_SP, result_offset); // m_type = FLOAT

					// Store 8-byte double to v.f field
					emit_fsd(load_freg, REG_SP, result_offset + 8);

					// Mark as RAW_FLOAT if we allocated an FP register
					if (fpreg >= 0) {
						m_vreg_types[result_vreg] = ValueType::RAW_FLOAT;
					} else {
						m_vreg_types[result_vreg] = ValueType::VARIANT;
					}
				}

				break;
			}

			// Not implementing these for now
			case IROpcode::MAKE_RECT2:
			case IROpcode::MAKE_RECT2I:
			case IROpcode::MAKE_PLANE:
			case IROpcode::VSET_INLINE:
			case IROpcode::VGET:
			case IROpcode::VSET:
			case IROpcode::LOAD_VAR:
			case IROpcode::STORE_VAR:
				throw std::runtime_error("Opcode not yet implemented in RISC-V codegen");

			case IROpcode::CALL_SYSCALL: {
				// CALL_SYSCALL format: result_reg, syscall_number, arg1, arg2, ...
				// Different syscalls have different calling conventions:
				// ECALL_GET_OBJ (504): result_reg, 504, string_index, string_length
				// ECALL_ARRAY_SIZE (523): result_reg, 523, array_vreg
				// ECALL_ARRAY_AT (522): result_reg, 522, array_vreg, index_vreg

				if (instr.operands.size() < 2) {
					throw std::runtime_error("CALL_SYSCALL requires at least 2 operands (result_reg, syscall_num)");
				}

				int result_vreg = std::get<int>(instr.operands[0].value);
				int syscall_num = static_cast<int>(std::get<int64_t>(instr.operands[1].value));

				// Handle different syscalls based on their calling conventions
				if (syscall_num == 504) {
					// ECALL_GET_OBJ: result_reg, 504, string_index, string_length
					if (instr.operands.size() != 4) {
						throw std::runtime_error("ECALL_GET_OBJ requires 4 operands");
					}

					int string_idx = static_cast<int>(std::get<int64_t>(instr.operands[2].value));
					int string_len = static_cast<int>(std::get<int64_t>(instr.operands[3].value));

					// Get the string constant
					if (string_idx < 0 || static_cast<size_t>(string_idx) >= m_string_constants->size()) {
						throw std::runtime_error("String constant index out of range");
					}
					const std::string& str = (*m_string_constants)[string_idx];

					int result_offset = get_variant_stack_offset(result_vreg);

					// ECALL_GET_OBJ clobbers a0 and a1
					std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1};
					auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_current_instr_idx);

					for (const auto& move : moves) {
						emit_mv(move.second, move.first);
					}

					// Allocate stack space for the string
					int str_space = ((string_len + 1) + 7) & ~7; // Align to 8 bytes, +1 for null terminator

					if (str_space < 2048) {
						emit_i_type(0x13, REG_SP, 0, REG_SP, -str_space);
					} else {
						emit_li(REG_T0, -str_space);
						emit_add(REG_SP, REG_SP, REG_T0);
					}

					// Store string on stack
					for (size_t i = 0; i < str.length(); i++) {
						emit_li(REG_T0, static_cast<unsigned char>(str[i]));
						emit_sb(REG_T0, REG_SP, i); // SB (store byte)
					}
					// Store null terminator
					emit_sb(REG_ZERO, REG_SP, string_len); // SB

					// a0 = pointer to class name string (sp) - FIRST ARGUMENT
					emit_mv(REG_A0, REG_SP);

					// a1 = string length - SECOND ARGUMENT
					emit_li(REG_A1, string_len);

					// a7 = syscall number (504 for ECALL_GET_OBJ)
					emit_li(REG_A7, syscall_num);
					emit_ecall();

					// After ecall, a0 contains the object data (uint64_t)
					// We need to store this into the result Variant

					// First, restore stack pointer (before storing result)
					if (str_space < 2048) {
						emit_i_type(0x13, REG_SP, 0, REG_SP, str_space);
					} else {
						emit_li(REG_T0, str_space);
						emit_add(REG_SP, REG_SP, REG_T0);
					}

					// Store the object type (GDOBJECT = 24) into Variant
					emit_li(REG_T0, 24); // GDOBJECT type
					emit_sw(REG_T0, REG_SP, result_offset); // Store 4-byte type at offset 0

					// Store the object data from a0 into the Variant data area (offset 8)
					emit_sd(REG_A0, REG_SP, result_offset + 8); // Store 8-byte object data

				} else if (syscall_num == 523) {
					// ECALL_ARRAY_SIZE: result_reg, 523, array_vreg
					// Takes: a0 = array variant index (unsigned)
					// Returns: a0 = array size (int)
					if (instr.operands.size() != 3) {
						throw std::runtime_error("ECALL_ARRAY_SIZE requires 3 operands");
					}

					int array_vreg = static_cast<int>(std::get<int>(instr.operands[2].value));
					int array_offset = get_variant_stack_offset(array_vreg);

					// ECALL_ARRAY_SIZE clobbers a0
					std::vector<uint8_t> clobbered_regs = {REG_A0};
					auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_current_instr_idx);

					for (const auto& move : moves) {
						emit_mv(move.second, move.first);
					}

					// Load the array variant index from offset 8 of the array Variant
					emit_ld(REG_A0, REG_SP, array_offset + 8); // Load 64-bit variant index

					// a7 = syscall number (523 for ECALL_ARRAY_SIZE)
					emit_li(REG_A7, syscall_num);
					emit_ecall();

					// After ecall, a0 contains the size (int)
					// Store it into the result Variant as an integer
					int result_offset = get_variant_stack_offset(result_vreg);
					emit_li(REG_T0, 2); // INT type = 2
					emit_sw(REG_T0, REG_SP, result_offset); // Store 4-byte type
					emit_sw(REG_A0, REG_SP, result_offset + 8); // Store 4-byte integer at offset 8

				} else if (syscall_num == 522) {
					// ECALL_ARRAY_AT: result_reg, 522, array_vreg, index_vreg
					// Takes: a0 = array variant index (unsigned), a1 = index (int), a2 = result GuestVariant pointer
					// Returns: result variant is filled with the element
					if (instr.operands.size() != 4) {
						throw std::runtime_error("ECALL_ARRAY_AT requires 4 operands");
					}

					int array_vreg = static_cast<int>(std::get<int>(instr.operands[2].value));
					int index_vreg = static_cast<int>(std::get<int>(instr.operands[3].value));

					int array_offset = get_variant_stack_offset(array_vreg);
					int result_offset = get_variant_stack_offset(result_vreg);

					// ECALL_ARRAY_AT clobbers a0, a1, a2
					std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2};
					auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_current_instr_idx);

					for (const auto& move : moves) {
						emit_mv(move.second, move.first);
					}

					// a0 = array variant index (load from offset 8 of array Variant)
					emit_ld(REG_A0, REG_SP, array_offset + 8); // Load 64-bit variant index

					// a1 = index (load from index_vreg)
					// The index is stored as a Variant, we need to extract the integer value
					int index_offset = get_variant_stack_offset(index_vreg);
					emit_lw(REG_A1, REG_SP, index_offset + 8); // Load integer from offset 8

					// a2 = pointer to result GuestVariant
					emit_i_type(0x13, REG_A2, 0, REG_SP, result_offset); // addi a2, sp, result_offset

					// a7 = syscall number (522 for ECALL_ARRAY_AT)
					emit_li(REG_A7, syscall_num);
					emit_ecall();

					// After ecall, the result Variant is already filled by the syscall

				} else {
					throw std::runtime_error("Unknown syscall number: " + std::to_string(syscall_num));
				}

				break;
			}

			default:
				throw std::runtime_error("Unknown IR opcode");
		}
	}
}

// Instruction encoding helpers
void RISCVCodeGen::emit_word(uint32_t word) {
	m_code.push_back(word & 0xFF);
	m_code.push_back((word >> 8) & 0xFF);
	m_code.push_back((word >> 16) & 0xFF);
	m_code.push_back((word >> 24) & 0xFF);
}

void RISCVCodeGen::emit_r_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t funct7) {
	uint32_t instr = opcode | (rd << 7) | (funct3 << 12) | (rs1 << 15) | (rs2 << 20) | (funct7 << 25);
	emit_word(instr);
}

void RISCVCodeGen::emit_i_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm) {
	uint32_t instr = opcode | (rd << 7) | (funct3 << 12) | (rs1 << 15) | ((imm & 0xFFF) << 20);
	emit_word(instr);
}

void RISCVCodeGen::emit_s_type(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm) {
	uint32_t imm_lo = imm & 0x1F;
	uint32_t imm_hi = (imm >> 5) & 0x7F;
	uint32_t instr = opcode | (imm_lo << 7) | (funct3 << 12) | (rs1 << 15) | (rs2 << 20) | (imm_hi << 25);
	m_code.push_back(instr & 0xFF);
	m_code.push_back((instr >> 8) & 0xFF);
	m_code.push_back((instr >> 16) & 0xFF);
	m_code.push_back((instr >> 24) & 0xFF);
}

void RISCVCodeGen::emit_b_type(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm) {
	// B-type immediate encoding (weird layout)
	uint32_t imm12 = (imm >> 12) & 1;
	uint32_t imm10_5 = (imm >> 5) & 0x3F;
	uint32_t imm4_1 = (imm >> 1) & 0xF;
	uint32_t imm11 = (imm >> 11) & 1;

	uint32_t instr = opcode | (imm11 << 7) | (imm4_1 << 8) | (funct3 << 12) | (rs1 << 15) | (rs2 << 20) | (imm10_5 << 25) | (imm12 << 31);
	m_code.push_back(instr & 0xFF);
	m_code.push_back((instr >> 8) & 0xFF);
	m_code.push_back((instr >> 16) & 0xFF);
	m_code.push_back((instr >> 24) & 0xFF);
}

void RISCVCodeGen::emit_u_type(uint8_t opcode, uint8_t rd, uint32_t imm) {
	uint32_t instr = opcode | (rd << 7) | (imm & 0xFFFFF000);
	m_code.push_back(instr & 0xFF);
	m_code.push_back((instr >> 8) & 0xFF);
	m_code.push_back((instr >> 16) & 0xFF);
	m_code.push_back((instr >> 24) & 0xFF);
}

void RISCVCodeGen::emit_j_type(uint8_t opcode, uint8_t rd, int32_t imm) {
	// J-type immediate encoding
	uint32_t imm20 = (imm >> 20) & 1;
	uint32_t imm10_1 = (imm >> 1) & 0x3FF;
	uint32_t imm11 = (imm >> 11) & 1;
	uint32_t imm19_12 = (imm >> 12) & 0xFF;

	uint32_t instr = opcode | (rd << 7) | (imm19_12 << 12) | (imm11 << 20) | (imm10_1 << 21) | (imm20 << 31);
	m_code.push_back(instr & 0xFF);
	m_code.push_back((instr >> 8) & 0xFF);
	m_code.push_back((instr >> 16) & 0xFF);
	m_code.push_back((instr >> 24) & 0xFF);
}

void RISCVCodeGen::emit_r4_type(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t rs3, uint8_t funct2)
{
	union {
		struct {
			uint32_t opcode : 7;
			uint32_t rd     : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t rs2    : 5;
			uint32_t funct2 : 2;
			uint32_t rs3    : 5;
		} R4type;
		uint32_t value;
	} r4;
	r4.R4type.opcode = opcode;
	r4.R4type.rd = rd;
	r4.R4type.funct3 = funct3;
	r4.R4type.rs1 = rs1;
	r4.R4type.rs2 = rs2;
	r4.R4type.funct2 = funct2;
	r4.R4type.rs3 = rs3;
	emit_word(r4.value);
}

// Higher-level instructions
void RISCVCodeGen::emit_li(uint8_t rd, int64_t imm) {
	// Load immediate - handles 64-bit values
	// For small values, use addi; for larger, build up the value
	if (imm >= -2048 && imm < 2048) {
		// Small immediate: addi rd, x0, imm
		emit_i_type(0x13, rd, 0, REG_ZERO, static_cast<int32_t>(imm));
	} else if (imm >= INT32_MIN && imm <= INT32_MAX) {
		// 32-bit immediate: lui + addi
		int32_t imm32 = static_cast<int32_t>(imm);
		int32_t upper = (imm32 + 0x800) >> 12; // Add 0x800 for rounding
		emit_u_type(0x37, rd, upper << 12);

		int32_t lower = imm32 & 0xFFF;
		if (lower != 0 || upper == 0) {
			emit_i_type(0x13, rd, 0, rd, lower);
		}
	} else {
		// Full 64-bit immediate: load from constant pool appended to .text
		// This avoids using temporary registers and generates cleaner code
		size_t const_index = add_constant(imm);

		// We'll use a label to refer to the constant pool entry
		// The constant will be located at: code_end + (const_index * 8)
		// We use AUIPC to get PC-relative address, then LD to load the value
		std::string label = ".LC" + std::to_string(const_index);

		// Mark this as a use of the constant label
		// Only mark AUIPC since we'll patch both AUIPC and LD together
		size_t auipc_offset = m_code.size();
		mark_label_use(label, auipc_offset);
		emit_u_type(0x17, rd, 0);  // auipc rd, 0 (will be patched)

		// Emit LD instruction (offset will be patched when AUIPC is resolved)
		emit_ld(rd, rd, 0);  // ld rd, 0(rd) (offset will be patched)
	}
}

void RISCVCodeGen::emit_mv(uint8_t rd, uint8_t rs) {
	// mv rd, rs = addi rd, rs, 0
	emit_i_type(0x13, rd, 0, rs, 0);
}

void RISCVCodeGen::emit_la(uint8_t rd, const std::string& label) {
	// Load address using AUIPC + ADDI
	// auipc rd, 0  # Load PC-relative upper bits
	// addi rd, rd, offset  # Add lower bits (will be patched)

	size_t auipc_offset = m_code.size();
	mark_label_use(label, auipc_offset);
	emit_u_type(0x17, rd, 0);  // auipc rd, 0 (will be patched)

	// Emit ADDI instruction (offset will be patched when AUIPC is resolved)
	emit_i_type(0x13, rd, 0, rd, 0);  // addi rd, rd, 0 (will be patched)
}

void RISCVCodeGen::emit_add(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 0, rs1, rs2, 0);
}

void RISCVCodeGen::emit_sub(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 0, rs1, rs2, 0x20);
}

void RISCVCodeGen::emit_mul(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 0, rs1, rs2, 1); // RV64M extension
}

void RISCVCodeGen::emit_div(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 4, rs1, rs2, 1); // RV64M extension - div
}

void RISCVCodeGen::emit_rem(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 6, rs1, rs2, 1); // RV64M extension - rem
}

void RISCVCodeGen::emit_and(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 7, rs1, rs2, 0);
}

void RISCVCodeGen::emit_or(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 6, rs1, rs2, 0);
}

void RISCVCodeGen::emit_xor(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 4, rs1, rs2, 0);
}

void RISCVCodeGen::emit_slt(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	emit_r_type(0x33, rd, 2, rs1, rs2, 0);
}

void RISCVCodeGen::emit_xori(uint8_t rd, uint8_t rs, int32_t imm) {
	emit_i_type(0x13, rd, 4, rs, imm); // XORI
}

void RISCVCodeGen::emit_seqz(uint8_t rd, uint8_t rs) {
	// seqz rd, rs is pseudo-instruction for sltiu rd, rs, 1
	emit_i_type(0x13, rd, 3, rs, 1); // SLTIU rd, rs, 1
}

void RISCVCodeGen::emit_snez(uint8_t rd, uint8_t rs) {
	// snez rd, rs is pseudo-instruction for sltu rd, x0, rs
	emit_r_type(0x33, rd, 3, REG_ZERO, rs, 0); // SLTU rd, x0, rs
}

void RISCVCodeGen::emit_beq(uint8_t rs1, uint8_t rs2, int32_t offset) {
	emit_b_type(0x63, 0, rs1, rs2, offset);
}

void RISCVCodeGen::emit_bne(uint8_t rs1, uint8_t rs2, int32_t offset) {
	emit_b_type(0x63, 1, rs1, rs2, offset);
}

void RISCVCodeGen::emit_jal(uint8_t rd, int32_t offset) {
	emit_j_type(0x6F, rd, offset);
}

void RISCVCodeGen::emit_jalr(uint8_t rd, uint8_t rs1, int32_t offset) {
	emit_i_type(0x67, rd, 0, rs1, offset);
}

void RISCVCodeGen::emit_ecall() {
	// ecall instruction: opcode = 0x73, funct3 = 0, rs1 = 0, rd = 0, imm = 0
	emit_i_type(0x73, 0, 0, 0, 0);
}

void RISCVCodeGen::emit_ret() {
	// ret = jalr x0, x1, 0
	emit_jalr(REG_ZERO, REG_RA, 0);
}

// Label management
void RISCVCodeGen::define_label(const std::string& label) {
	m_labels[label] = m_code.size();
}

void RISCVCodeGen::mark_label_use(const std::string& label, size_t code_offset) {
	m_label_uses.push_back({label, code_offset});
}

void RISCVCodeGen::resolve_labels() {
	for (const auto& use : m_label_uses) {
		const std::string& label = use.first;
		size_t use_offset = use.second;

		auto it = m_labels.find(label);
		if (it == m_labels.end()) {
			throw std::runtime_error("Undefined label: " + label);
		}

		size_t target_offset = it->second;
		int32_t offset = static_cast<int32_t>(target_offset - use_offset);

		// Patch the instruction at use_offset with the correct offset
		uint32_t instr;
		memcpy(&instr, &m_code[use_offset], 4);

		uint8_t opcode = instr & 0x7F;

		if (opcode == 0x63) { // B-type (branch)
			// Re-encode with correct offset
			uint8_t funct3 = (instr >> 12) & 0x7;
			uint8_t rs1 = (instr >> 15) & 0x1F;
			uint8_t rs2 = (instr >> 20) & 0x1F;

			uint32_t imm12 = (offset >> 12) & 1;
			uint32_t imm10_5 = (offset >> 5) & 0x3F;
			uint32_t imm4_1 = (offset >> 1) & 0xF;
			uint32_t imm11 = (offset >> 11) & 1;

			instr = opcode | (imm11 << 7) | (imm4_1 << 8) | (funct3 << 12) | (rs1 << 15) | (rs2 << 20) | (imm10_5 << 25) | (imm12 << 31);
		} else if (opcode == 0x6F) { // J-type (jal)
			uint8_t rd = (instr >> 7) & 0x1F;

			uint32_t imm20 = (offset >> 20) & 1;
			uint32_t imm10_1 = (offset >> 1) & 0x3FF;
			uint32_t imm11 = (offset >> 11) & 1;
			uint32_t imm19_12 = (offset >> 12) & 0xFF;

			instr = opcode | (rd << 7) | (imm19_12 << 12) | (imm11 << 20) | (imm10_1 << 21) | (imm20 << 31);
		} else if (opcode == 0x17) { // U-type (AUIPC) - for constant pool or address loading
			uint8_t rd = (instr >> 7) & 0x1F;

			// Check if the next instruction is LD (for constant pool) or ADDI (for address)
			uint32_t next_instr;
			memcpy(&next_instr, &m_code[use_offset + 4], 4);
			uint8_t next_opcode = next_instr & 0x7F;

			if (next_opcode == 0x03) { // LD instruction (for constant pool access)
				// Calculate PC-relative offset
				// Split into upper 20 bits (for AUIPC) and lower 12 bits (for LD)
				int32_t upper = (offset + 0x800) >> 12; // Add 0x800 for sign extension
				int32_t lower = offset & 0xFFF;

				// Patch AUIPC with upper 20 bits
				instr = opcode | (rd << 7) | ((upper & 0xFFFFF) << 12);
				memcpy(&m_code[use_offset], &instr, 4);

				// Patch the following LD instruction with lower 12 bits
				// LD is at use_offset + 4
				uint8_t ld_rd = (next_instr >> 7) & 0x1F;
				uint8_t ld_rs1 = (next_instr >> 15) & 0x1F;
				uint8_t ld_funct3 = (next_instr >> 12) & 0x7;
				next_instr = 0x03 | (ld_rd << 7) | (ld_funct3 << 12) | (ld_rs1 << 15) | ((lower & 0xFFF) << 20);
				memcpy(&m_code[use_offset + 4], &next_instr, 4);

				// Skip the next use since we processed both AUIPC and LD together
				continue;
			} else if (next_opcode == 0x13) { // ADDI instruction (for load address)
				// AUIPC + ADDI pattern for load address (emit_la)
				uint8_t addi_funct3 = (next_instr >> 12) & 0x7;
				uint8_t addi_rs1 = (next_instr >> 15) & 0x1F;

				// For AUIPC+ADDI, the offset is the full 32-bit value
				// Split into upper 20 bits (for AUIPC) and lower 12 bits (for ADDI)
				int32_t upper = (offset + 0x800) >> 12; // Add 0x800 for sign extension
				int32_t lower = offset & 0xFFF;

				// Verify ADDI is using the same register as rd (rs1 == rd)
				// This is required for AUIPC+ADDI pattern
				if (addi_rs1 == rd && addi_funct3 == 0) {
					// Patch AUIPC with upper 20 bits
					instr = opcode | (rd << 7) | ((upper & 0xFFFFF) << 12);
					memcpy(&m_code[use_offset], &instr, 4);

					// Patch the following ADDI instruction with lower 12 bits
					uint8_t addi_rd = (next_instr >> 7) & 0x1F;
					next_instr = 0x13 | (addi_rd << 7) | (addi_funct3 << 12) | (addi_rs1 << 15) | ((lower & 0xFFF) << 20);
					memcpy(&m_code[use_offset + 4], &next_instr, 4);

					// Skip the next use since we processed both AUIPC and ADDI together
					continue;
				}
			}
		}

		memcpy(&m_code[use_offset], &instr, 4);
	}
}

int RISCVCodeGen::get_variant_stack_offset(int virtual_reg) {
	auto it = m_variant_offsets.find(virtual_reg);
	if (it != m_variant_offsets.end()) {
		return it->second;
	}

	// Allocate new stack slot for this Variant
	// Stack layout: [saved ra (8)] [saved fp (8)] [saved a0 (8)] [Variants...]
	int offset = 24 + (m_next_variant_slot * VARIANT_SIZE);
	m_variant_offsets[virtual_reg] = offset;
	m_next_variant_slot++;

	return offset;
}

void RISCVCodeGen::emit_variant_create_int(int stack_offset, int64_t value) {
	// Create an integer Variant on the stack
	// Variant layout: [uint32_t m_type (4 bytes)] [padding (4 bytes)] [int64_t v.i (8 bytes)]

	// Store m_type = INT (2) at stack_offset
	emit_li(REG_T0, 2); // INT type
	emit_store_variant_type(REG_T0, REG_SP, stack_offset);

	// Store value
	emit_li(REG_T0, value);
	emit_store_variant_int(REG_T0, REG_SP, stack_offset);
}

void RISCVCodeGen::emit_variant_create_bool(int stack_offset, bool value) {
	// Similar to create_int but with BOOL type (1)
	emit_li(REG_T0, 1); // BOOL type
	emit_store_variant_type(REG_T0, REG_SP, stack_offset);

	emit_li(REG_T0, value ? 1 : 0);
	emit_store_variant_bool(REG_T0, REG_SP, stack_offset);
}

void RISCVCodeGen::emit_variant_create_string(int stack_offset, int string_idx) {
	// Create a string Variant using VCREATE syscall
	// VCREATE signature: void vcreate(Variant* dst, Variant::Type type, int method, void* data)
	// For strings with method==1: data = struct { const char* str; size_t length; }

	if (!m_string_constants || string_idx < 0 || string_idx >= static_cast<int>(m_string_constants->size())) {
		throw std::runtime_error("Invalid string constant index: " + std::to_string(string_idx));
	}

	const std::string& str = (*m_string_constants)[string_idx];
	int str_len = static_cast<int>(str.length());

	// Handle register clobbering (VCREATE uses a0-a3)
	std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3};
	auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_current_instr_idx);
	for (const auto& move : moves) {
		emit_mv(move.second, move.first);
	}

	// Allocate stack space for: string data + struct { char*, size_t }
	int str_space = ((str_len + 1) + 7) & ~7; // String + null terminator, aligned to 8 bytes
	int struct_space = 16; // Two 8-byte fields
	int total_space = str_space + struct_space;
	total_space = (total_space + 15) & ~15; // Align to 16 bytes

	// Adjust stack pointer
	if (total_space < 2048) {
		emit_i_type(0x13, REG_SP, 0, REG_SP, -total_space);
	} else {
		emit_li(REG_T0, -total_space);
		emit_add(REG_SP, REG_SP, REG_T0);
	}

	// Store string data on stack
	for (size_t i = 0; i < str.length(); i++) {
		emit_li(REG_T0, static_cast<unsigned char>(str[i]));
		emit_sb(REG_T0, REG_SP, i); // SB (store byte)
	}
	// Store null terminator
	emit_sb(REG_ZERO, REG_SP, str_len); // SB

	// Create struct at sp + str_space
	// struct.str = sp (pointer to string data)
	if (str_space < 2048) {
		emit_i_type(0x13, REG_T0, 0, REG_SP, 0); // T0 = SP + 0 (pointer to string)
	} else {
		emit_mv(REG_T0, REG_SP);
	}
	emit_sd(REG_T0, REG_SP, str_space); // SD (store pointer at sp + str_space)

	// struct.length = str_len
	emit_li(REG_T0, str_len);
	emit_sd(REG_T0, REG_SP, str_space + 8); // SD (store length)

	// Call VCREATE
	// a0 = pointer to destination Variant (stack_offset + total_space)
	int adjusted_dst_offset = stack_offset + total_space;
	if (adjusted_dst_offset < 2048) {
		emit_i_type(0x13, REG_A0, 0, REG_SP, adjusted_dst_offset);
	} else {
		emit_li(REG_A0, adjusted_dst_offset);
		emit_add(REG_A0, REG_SP, REG_A0);
	}

	// a1 = Variant::STRING (4)
	emit_li(REG_A1, 4);

	// a2 = method (1 for const char* + size_t)
	emit_li(REG_A2, 1);

	// a3 = pointer to struct (sp + str_space)
	if (str_space < 2048) {
		emit_i_type(0x13, REG_A3, 0, REG_SP, str_space);
	} else {
		emit_li(REG_A3, str_space);
		emit_add(REG_A3, REG_SP, REG_A3);
	}

	// a7 = ECALL_VCREATE (517)
	emit_li(REG_A7, 517);
	emit_ecall();

	// Restore stack pointer
	if (total_space < 2048) {
		emit_i_type(0x13, REG_SP, 0, REG_SP, total_space);
	} else {
		emit_li(REG_T0, total_space);
		emit_add(REG_SP, REG_SP, REG_T0);
	}
}

void RISCVCodeGen::emit_variant_copy(int dst_offset, int src_offset) {
	// Copy 24 bytes from src to dst
	for (int i = 0; i < 3; i++) {
		// Load from source
		emit_ld(REG_T0, REG_SP, src_offset + i * 8); // LD

		// Store to destination
		emit_sd(REG_T0, REG_SP, dst_offset + i * 8); // SD
	}
}

void RISCVCodeGen::emit_variant_eval(int result_offset, int lhs_offset, int rhs_offset, int op) {
	// Call sys_veval(op, &lhs, &rhs, &result)
	// Signature: bool sys_veval(int op, const Variant* a, const Variant* b, Variant* result)
	
	// VEVAL clobbers a0-a3, so handle register clobbering first
	std::vector<uint8_t> clobbered_regs = {REG_A0, REG_A1, REG_A2, REG_A3};
	auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_current_instr_idx);
	
	// Emit moves to save live values from clobbered registers
	for (const auto& move : moves) {
		emit_mv(move.second, move.first); // Move from clobbered reg to new reg
	}

	// Load operator into a0
	emit_li(REG_A0, op);

	// Load address of lhs Variant into a1: addi a1, sp, lhs_offset
	if (lhs_offset < 2048) {
		emit_i_type(0x13, REG_A1, 0, REG_SP, lhs_offset);
	} else {
		emit_li(REG_A1, lhs_offset);
		emit_add(REG_A1, REG_SP, REG_A1);
	}

	// Load address of rhs Variant into a2: addi a2, sp, rhs_offset
	if (rhs_offset < 2048) {
		emit_i_type(0x13, REG_A2, 0, REG_SP, rhs_offset);
	} else {
		emit_li(REG_A2, rhs_offset);
		emit_add(REG_A2, REG_SP, REG_A2);
	}

	// Load address of result Variant into a3: addi a3, sp, result_offset
	if (result_offset < 2048) {
		emit_i_type(0x13, REG_A3, 0, REG_SP, result_offset);
	} else {
		emit_li(REG_A3, result_offset);
		emit_add(REG_A3, REG_SP, REG_A3);
	}

	// Make the ecall to sys_veval (ECALL_VEVAL = 502)
	emit_li(REG_A7, 502); // ECALL_VEVAL
	emit_ecall();
}

// Variant field access helpers
void RISCVCodeGen::emit_load_variant_type(uint8_t rd, uint8_t base_reg, int32_t variant_offset) {
	// Load m_type field (4 bytes at variant_offset + 0)
	emit_lw(rd, base_reg, variant_offset + VARIANT_TYPE_OFFSET);
}

void RISCVCodeGen::emit_store_variant_type(uint8_t rs, uint8_t base_reg, int32_t variant_offset) {
	// Store m_type field (4 bytes at variant_offset + 0)
	emit_sw(rs, base_reg, variant_offset + VARIANT_TYPE_OFFSET);
}

void RISCVCodeGen::emit_load_variant_bool(uint8_t rd, uint8_t base_reg, int32_t variant_offset) {
	// Load boolean value (1 byte at variant_offset + 8)
	// Use unsigned load to ensure proper zero-extension
	emit_lbu(rd, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_store_variant_bool(uint8_t rs, uint8_t base_reg, int32_t variant_offset) {
	// Store boolean value (1 byte at variant_offset + 8)
	// Note: storing 8 bytes is fine too since bool is in a union, but byte is more precise
	emit_sb(rs, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_load_variant_int(uint8_t rd, uint8_t base_reg, int32_t variant_offset) {
	// Load int64 value (8 bytes at variant_offset + 8)
	emit_ld(rd, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

void RISCVCodeGen::emit_store_variant_int(uint8_t rs, uint8_t base_reg, int32_t variant_offset) {
	// Store int64 value (8 bytes at variant_offset + 8)
	emit_sd(rs, base_reg, variant_offset + VARIANT_DATA_OFFSET);
}

// Load/Store helpers with automatic large offset handling
void RISCVCodeGen::emit_ld(uint8_t rd, uint8_t rs1, int32_t offset) {
	if (offset >= -2048 && offset < 2048) {
		emit_i_type(0x03, rd, 3, rs1, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_i_type(0x03, rd, 3, REG_T2, 0);
	}
}

void RISCVCodeGen::emit_lw(uint8_t rd, uint8_t rs1, int32_t offset) {
	if (offset >= -2048 && offset < 2048) {
		emit_i_type(0x03, rd, 2, rs1, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_i_type(0x03, rd, 2, REG_T2, 0);
	}
}

void RISCVCodeGen::emit_lwu(uint8_t rd, uint8_t rs1, int32_t offset) {
	// LWU (Load Word Unsigned) - RV64I specific, zero-extends to 64 bits
	if (offset >= -2048 && offset < 2048) {
		emit_i_type(0x03, rd, 4, rs1, offset);  // funct3=4 for LWU
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_i_type(0x03, rd, 4, REG_T2, 0);  // funct3=4 for LWU
	}
}

void RISCVCodeGen::emit_lh(uint8_t rd, uint8_t rs1, int32_t offset) {
	if (offset >= -2048 && offset < 2048) {
		emit_i_type(0x03, rd, 1, rs1, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_i_type(0x03, rd, 1, REG_T2, 0);
	}
}

void RISCVCodeGen::emit_lb(uint8_t rd, uint8_t rs1, int32_t offset) {
	if (offset >= -2048 && offset < 2048) {
		emit_i_type(0x03, rd, 0, rs1, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_i_type(0x03, rd, 0, REG_T2, 0);
	}
}

void RISCVCodeGen::emit_lbu(uint8_t rd, uint8_t rs1, int32_t offset) {
	if (offset >= -2048 && offset < 2048) {
		emit_i_type(0x03, rd, 4, rs1, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_i_type(0x03, rd, 4, REG_T2, 0);
	}
}

void RISCVCodeGen::emit_sd(uint8_t rs2, uint8_t rs1, int32_t offset) {
	if (offset >= -2048 && offset < 2048) {
		emit_s_type(0x23, 3, rs1, rs2, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_s_type(0x23, 3, REG_T2, rs2, 0);
	}
}

void RISCVCodeGen::emit_sw(uint8_t rs2, uint8_t rs1, int32_t offset) {
	if (offset >= -2048 && offset < 2048) {
		emit_s_type(0x23, 2, rs1, rs2, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_s_type(0x23, 2, REG_T2, rs2, 0);
	}
}

void RISCVCodeGen::emit_sh(uint8_t rs2, uint8_t rs1, int32_t offset) {
	if (offset >= -2048 && offset < 2048) {
		emit_s_type(0x23, 1, rs1, rs2, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_s_type(0x23, 1, REG_T2, rs2, 0);
	}
}

void RISCVCodeGen::emit_sb(uint8_t rs2, uint8_t rs1, int32_t offset) {
	if (offset >= -2048 && offset < 2048) {
		emit_s_type(0x23, 0, rs1, rs2, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_s_type(0x23, 0, REG_T2, rs2, 0);
	}
}

// Floating-point load/store (RV64D extension)
void RISCVCodeGen::emit_fld(uint8_t rd, uint8_t rs1, int32_t offset) {
	// FLD: opcode=0x07, funct3=3
	if (offset >= -2048 && offset < 2048) {
		emit_i_type(0x07, rd, 3, rs1, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_i_type(0x07, rd, 3, REG_T2, 0);
	}
}

void RISCVCodeGen::emit_fsd(uint8_t rs2, uint8_t rs1, int32_t offset) {
	// FSD: opcode=0x27, funct3=3
	if (offset >= -2048 && offset < 2048) {
		emit_s_type(0x27, 3, rs1, rs2, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_s_type(0x27, 3, REG_T2, rs2, 0);
	}
}

void RISCVCodeGen::emit_flw(uint8_t rd, uint8_t rs1, int32_t offset) {
	// FLW: opcode=0x07, funct3=2 (RV32F/RV64F)
	if (offset >= -2048 && offset < 2048) {
		emit_i_type(0x07, rd, 2, rs1, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_i_type(0x07, rd, 2, REG_T2, 0);
	}
}

void RISCVCodeGen::emit_fsw(uint8_t rs2, uint8_t rs1, int32_t offset) {
	// FSW: opcode=0x27, funct3=2 (RV32F/RV64F)
	if (offset >= -2048 && offset < 2048) {
		emit_s_type(0x27, 2, rs1, rs2, offset);
	} else {
		emit_li(REG_T2, offset);
		emit_add(REG_T2, rs1, REG_T2);
		emit_s_type(0x27, 2, REG_T2, rs2, 0);
	}
}

void RISCVCodeGen::emit_fcvt_d_s(uint8_t rd, uint8_t rs1) {
	// FCVT.D.S: Convert float (32-bit) to double (64-bit)
	emit_r4_type(0b1010011, rd, 0, rs1, 0, 0b01000, 1);
}

void RISCVCodeGen::emit_fcvt_s_d(uint8_t rd, uint8_t rs1) {
	// FCVT.S.D: Convert double (64-bit) to float (32-bit)
	emit_r4_type(0b1010011, rd, 0, rs1, 1, 0b01000, 0);
}

void RISCVCodeGen::emit_fadd_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FADD.D: Double-precision FP add
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x01);
}

void RISCVCodeGen::emit_fsub_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FSUB.D: Double-precision FP subtract
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x05);
}

void RISCVCodeGen::emit_fmul_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FMUL.D: Double-precision FP multiply
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x09);
}

void RISCVCodeGen::emit_fdiv_d(uint8_t rd, uint8_t rs1, uint8_t rs2) {
	// FDIV.D: Double-precision FP divide
	emit_r_type(0x53, rd, 0, rs1, rs2, 0x0D);
}

// Sign-extend word to doubleword (addiw rd, rs, 0)
void RISCVCodeGen::emit_sext_w(uint8_t rd, uint8_t rs) {
	// ADDIW: opcode=0x1b, funct3=0
	emit_i_type(0x1b, rd, 0, rs, 0);
}

// FP register allocation helpers
int RISCVCodeGen::allocate_fp_register(int vreg) {
	// Check if already allocated
	auto it = m_vreg_to_fpreg.find(vreg);
	if (it != m_vreg_to_fpreg.end()) {
		return it->second;
	}

	// Allocate from free pool
	if (m_free_fp_registers.empty()) {
		return -1; // No FP registers available
	}

	uint8_t fpreg = m_free_fp_registers.back();
	m_free_fp_registers.pop_back();
	m_vreg_to_fpreg[vreg] = fpreg;
	m_fpreg_to_vreg[fpreg] = vreg;
	return fpreg;
}

void RISCVCodeGen::free_fp_register(int vreg) {
	auto it = m_vreg_to_fpreg.find(vreg);
	if (it == m_vreg_to_fpreg.end()) {
		return;
	}

	uint8_t fpreg = it->second;
	m_free_fp_registers.push_back(fpreg);
	m_fpreg_to_vreg.erase(fpreg);
	m_vreg_to_fpreg.erase(it);
}

int RISCVCodeGen::get_fp_register(int vreg) const {
	auto it = m_vreg_to_fpreg.find(vreg);
	if (it != m_vreg_to_fpreg.end()) {
		return it->second;
	}
	return -1;
}

void RISCVCodeGen::materialize_raw_float_to_variant(int vreg) {
	int fpreg = get_fp_register(vreg);
	if (fpreg < 0) {
		return; // Not an FP register
	}

	// Get stack offset for the Variant
	int stack_offset = get_variant_stack_offset(vreg);

	// Set m_type = FLOAT (3)
	emit_li(REG_T0, 3);
	emit_sw(REG_T0, REG_SP, stack_offset);

	// Store the 64-bit double value at offset 8
	emit_fsd(fpreg, REG_SP, stack_offset + 8);

	// Mark as VARIANT (materialized)
	m_vreg_types[vreg] = ValueType::VARIANT;
}

} // namespace gdscript
