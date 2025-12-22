#include "riscv_codegen.h"
#include <stdexcept>
#include <cstring>

namespace gdscript {

RISCVCodeGen::RISCVCodeGen() {}

std::vector<uint8_t> RISCVCodeGen::generate(const IRProgram& program) {
	m_code.clear();
	m_labels.clear();
	m_label_uses.clear();
	m_functions.clear();
	m_variant_offsets.clear();
	m_string_constants = &program.string_constants;

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

	// Resolve all label references
	resolve_labels();

	return m_code;
}

void RISCVCodeGen::gen_function(const IRFunction& func) {
	// Godot Sandbox calling convention with Variants:
	// a0 = pointer to return Variant (pre-allocated by caller)
	// a1-a7 = pointers to argument Variants
	m_variant_offsets.clear();
	m_num_params = func.parameters.size();
	m_next_variant_slot = 0;
	m_stack_frame_size = 0;
	m_current_instr_idx = 0;

	// Initialize register allocator
	m_allocator.init(func);

	// Calculate stack frame size
	// Need space for: spilled registers * VARIANT_SIZE + saved registers
	// We'll calculate this after allocation, but estimate based on max_registers
	int max_variants = func.max_registers;
	int variant_space = max_variants * VARIANT_SIZE;
	int saved_reg_space = 24; // Save ra, fp, and a0 (return pointer)
	m_stack_frame_size = variant_space + saved_reg_space;

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
	emit_s_type(0x23, 3, REG_SP, REG_RA, 0); // SD

	// Save frame pointer: sd fp, 8(sp)
	emit_s_type(0x23, 3, REG_SP, REG_FP, 8); // SD

	// Save a0 (return Variant pointer): sd a0, 16(sp)
	emit_s_type(0x23, 3, REG_SP, REG_A0, 16); // SD

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
			emit_i_type(0x03, REG_T0, 3, arg_reg, j * 8); // LD t0, j*8(arg_reg)
			if (dst_offset + j * 8 < 2048) {
				emit_s_type(0x23, 3, REG_SP, REG_T0, dst_offset + j * 8); // SD t0, offset(sp)
			} else {
				emit_li(REG_T1, dst_offset + j * 8);
				emit_add(REG_T1, REG_SP, REG_T1);
				emit_s_type(0x23, 3, REG_T1, REG_T0, 0);
			}
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

				// Allocate register for Variant pointer (stack offset)
				int preg = m_allocator.allocate_register(vreg, m_current_instr_idx);
				int stack_offset = get_variant_stack_offset(vreg);

				// Create integer Variant at stack offset
				emit_variant_create_int(stack_offset, value);

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

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int src_offset = get_variant_stack_offset(src_vreg);

				// Copy Variant: use sys_vclone or memcpy (24 bytes)
				emit_variant_copy(dst_offset, src_offset);
				break;
			}

			case IROpcode::ADD:
			case IROpcode::SUB:
			case IROpcode::MUL:
			case IROpcode::DIV:
			case IROpcode::MOD: {
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int lhs_vreg = std::get<int>(instr.operands[1].value);
				int rhs_vreg = std::get<int>(instr.operands[2].value);

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int lhs_offset = get_variant_stack_offset(lhs_vreg);
				int rhs_offset = get_variant_stack_offset(rhs_vreg);

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

				emit_variant_eval(dst_offset, lhs_offset, rhs_offset, variant_op);
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
				int dst_vreg = std::get<int>(instr.operands[0].value);
				int lhs_vreg = std::get<int>(instr.operands[1].value);
				int rhs_vreg = std::get<int>(instr.operands[2].value);

				int dst_offset = get_variant_stack_offset(dst_vreg);
				int lhs_offset = get_variant_stack_offset(lhs_vreg);
				int rhs_offset = get_variant_stack_offset(rhs_vreg);

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

				emit_variant_eval(dst_offset, lhs_offset, rhs_offset, variant_op);
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
				int offset = get_variant_stack_offset(vreg);

				// Load Variant's value to check if it's falsy
				// For booleans/integers: check v.i field (offset +8 from Variant base)
				// Load v.i field: ld t0, offset+8(sp)
				if (offset + 8 < 2048) {
					emit_i_type(0x03, REG_T0, 3, REG_SP, offset + 8); // LD
				} else {
					emit_li(REG_T1, offset + 8);
					emit_add(REG_T1, REG_SP, REG_T1);
					emit_i_type(0x03, REG_T0, 3, REG_T1, 0); // LD from computed address
				}

				// Branch if zero
				mark_label_use(std::get<std::string>(instr.operands[1].value), m_code.size());
				emit_beq(REG_T0, REG_ZERO, 0);
				break;
			}

			case IROpcode::BRANCH_NOT_ZERO: {
				int vreg = std::get<int>(instr.operands[0].value);
				int offset = get_variant_stack_offset(vreg);

				// Load v.i field: ld t0, offset+8(sp)
				if (offset + 8 < 2048) {
					emit_i_type(0x03, REG_T0, 3, REG_SP, offset + 8); // LD
				} else {
					emit_li(REG_T1, offset + 8);
					emit_add(REG_T1, REG_SP, REG_T1);
					emit_i_type(0x03, REG_T0, 3, REG_T1, 0);
				}

				// Branch if not zero
				mark_label_use(std::get<std::string>(instr.operands[1].value), m_code.size());
				emit_bne(REG_T0, REG_ZERO, 0);
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
				emit_i_type(0x03, REG_A0, 3, REG_SP, 16); // LD a0, 16(sp)

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
						emit_i_type(0x03, REG_T1, 3, REG_T0, i * 8); // LD t1, i*8(t0)
						emit_s_type(0x23, 3, REG_A0, REG_T1, i * 8); // SD t1, i*8(a0)
					}
				}

				// Function epilogue - restore registers and deallocate stack
				// Restore return address: ld ra, 0(sp)
				emit_i_type(0x03, REG_RA, 3, REG_SP, 0); // LD

				// Restore frame pointer: ld fp, 8(sp)
				emit_i_type(0x03, REG_FP, 3, REG_SP, 8); // LD

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
							if (arg_src_offset + j * 8 < 2048) {
								emit_i_type(0x03, REG_T0, 3, REG_SP, arg_src_offset + j * 8); // LD
							} else {
								emit_li(REG_T1, arg_src_offset + j * 8);
								emit_add(REG_T1, REG_SP, REG_T1);
								emit_i_type(0x03, REG_T0, 3, REG_T1, 0);
							}

							// Store to destination
							if (arg_dst_offset + j * 8 < 2048) {
								emit_s_type(0x23, 3, REG_SP, REG_T0, arg_dst_offset + j * 8); // SD
							} else {
								emit_li(REG_T1, arg_dst_offset + j * 8);
								emit_add(REG_T1, REG_SP, REG_T1);
								emit_s_type(0x23, 3, REG_T1, REG_T0, 0);
							}
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
					emit_s_type(0x23, 0, REG_SP, REG_T0, i); // SB (store byte)
				}
				// Store null terminator
				emit_s_type(0x23, 0, REG_SP, REG_ZERO, method_len); // SB

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

			// Not implementing these for now
			case IROpcode::CALL_SYSCALL:
			case IROpcode::VGET:
			case IROpcode::VSET:
			case IROpcode::LOAD_VAR:
			case IROpcode::STORE_VAR:
				throw std::runtime_error("Opcode not yet implemented in RISC-V codegen");

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

// Higher-level instructions
void RISCVCodeGen::emit_li(uint8_t rd, int64_t imm) {
	// Load immediate - handles 64-bit values
	// For small values, use addi; for larger, use lui+addi
	if (imm >= -2048 && imm < 2048) {
		// addi rd, x0, imm
		emit_i_type(0x13, rd, 0, REG_ZERO, static_cast<int32_t>(imm));
	} else {
		// lui rd, imm[31:12]
		uint32_t upper = static_cast<uint32_t>((imm + 0x800) >> 12); // Add 0x800 for rounding
		emit_u_type(0x37, rd, upper << 12);

		// addi rd, rd, imm[11:0]
		int32_t lower = imm & 0xFFF;
		if (lower != 0) {
			emit_i_type(0x13, rd, 0, rd, lower);
		}
	}
}

void RISCVCodeGen::emit_mv(uint8_t rd, uint8_t rs) {
	// mv rd, rs = addi rd, rs, 0
	emit_i_type(0x13, rd, 0, rs, 0);
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
	if (stack_offset < 2048) {
		emit_s_type(0x23, 2, REG_SP, REG_T0, stack_offset); // SW
	} else {
		emit_li(REG_T2, stack_offset);
		emit_add(REG_T2, REG_SP, REG_T2);
		emit_s_type(0x23, 2, REG_T2, REG_T0, 0); // SW
	}

	// Store value at stack_offset + 8
	emit_li(REG_T0, value);
	if (stack_offset + 8 < 2048) {
		emit_s_type(0x23, 3, REG_SP, REG_T0, stack_offset + 8); // SD
	} else {
		emit_li(REG_T2, stack_offset + 8);
		emit_add(REG_T2, REG_SP, REG_T2);
		emit_s_type(0x23, 3, REG_T2, REG_T0, 0); // SD
	}
}

void RISCVCodeGen::emit_variant_create_bool(int stack_offset, bool value) {
	// Similar to create_int but with BOOL type (1)
	emit_li(REG_T0, 1); // BOOL type
	if (stack_offset < 2048) {
		emit_s_type(0x23, 2, REG_SP, REG_T0, stack_offset); // SW
	} else {
		emit_li(REG_T2, stack_offset);
		emit_add(REG_T2, REG_SP, REG_T2);
		emit_s_type(0x23, 2, REG_T2, REG_T0, 0);
	}

	emit_li(REG_T0, value ? 1 : 0);
	if (stack_offset + 8 < 2048) {
		emit_s_type(0x23, 3, REG_SP, REG_T0, stack_offset + 8); // SD
	} else {
		emit_li(REG_T2, stack_offset + 8);
		emit_add(REG_T2, REG_SP, REG_T2);
		emit_s_type(0x23, 3, REG_T2, REG_T0, 0);
	}
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
		emit_s_type(0x23, 0, REG_SP, REG_T0, i); // SB (store byte)
	}
	// Store null terminator
	emit_s_type(0x23, 0, REG_SP, REG_ZERO, str_len); // SB

	// Create struct at sp + str_space
	// struct.str = sp (pointer to string data)
	if (str_space < 2048) {
		emit_i_type(0x13, REG_T0, 0, REG_SP, 0); // T0 = SP + 0 (pointer to string)
	} else {
		emit_mv(REG_T0, REG_SP);
	}
	emit_s_type(0x23, 3, REG_SP, REG_T0, str_space); // SD (store pointer at sp + str_space)

	// struct.length = str_len
	emit_li(REG_T0, str_len);
	emit_s_type(0x23, 3, REG_SP, REG_T0, str_space + 8); // SD (store length)

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
		if (src_offset + i * 8 < 2048) {
			emit_i_type(0x03, REG_T0, 3, REG_SP, src_offset + i * 8); // LD
		} else {
			emit_li(REG_T1, src_offset + i * 8);
			emit_add(REG_T1, REG_SP, REG_T1);
			emit_i_type(0x03, REG_T0, 3, REG_T1, 0);
		}

		// Store to destination
		if (dst_offset + i * 8 < 2048) {
			emit_s_type(0x23, 3, REG_SP, REG_T0, dst_offset + i * 8); // SD
		} else {
			emit_li(REG_T1, dst_offset + i * 8);
			emit_add(REG_T1, REG_SP, REG_T1);
			emit_s_type(0x23, 3, REG_T1, REG_T0, 0);
		}
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

} // namespace gdscript
