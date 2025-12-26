#include "ir_optimizer.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace gdscript {

IROptimizer::IROptimizer() {}

void IROptimizer::optimize(IRProgram& program) {
	for (auto& func : program.functions) {
		optimize_function(func);
	}
}

void IROptimizer::optimize_function(IRFunction& func) {
	// Multiple optimization passes
	// Run constant folding first as it can enable more optimizations
	constant_folding(func);

	// Copy propagation to eliminate redundant MOVEs after constant loads
	copy_propagation(func);

	// Peephole optimization to remove redundant moves and operations
	peephole_optimization(func);

	// Run peephole again to catch patterns that emerged after previous optimizations
	peephole_optimization(func);

	// Eliminate redundant store operations (run before dead code elimination)
	eliminate_redundant_stores(func);

	// Run peephole once more after eliminate_redundant_stores to clean up remaining patterns
	peephole_optimization(func);

	// Eliminate dead code (unused registers and instructions)
	eliminate_dead_code(func);

	// NOTE: reduce_register_pressure() is disabled for now because it breaks
	// the calling convention. Parameters are in specific registers (r0-r6)
	// and return value must be in r0. We need to be more careful about
	// which registers we renumber.
	// reduce_register_pressure(func);

	// Recalculate max_registers after optimizations
	int max_reg = 0;
	for (const auto& instr : func.instructions) {
		for (const auto& op : instr.operands) {
			if (op.type == IRValue::Type::REGISTER) {
				int reg = std::get<int>(op.value);
				max_reg = std::max(max_reg, reg);
			}
		}
	}
	func.max_registers = max_reg + 1;
}

void IROptimizer::constant_folding(IRFunction& func) {
	m_constants.clear();
	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());

	for (const auto& instr : func.instructions) {
		bool folded = false;

		// Important: invalidate all constants when we encounter control flow targets
		// because we don't know what path we took to reach this point
		if (instr.opcode == IROpcode::LABEL) {
			m_constants.clear();
			new_instructions.push_back(instr);
			continue;
		}

		switch (instr.opcode) {
			case IROpcode::LOAD_IMM: {
				int reg = std::get<int>(instr.operands[0].value);
				int64_t val = std::get<int64_t>(instr.operands[1].value);
				ConstantValue cv;
				cv.type = ConstantValue::Type::INT;
				cv.int_value = val;
				set_register_constant(reg, cv);
				new_instructions.push_back(instr);
				break;
			}

			case IROpcode::LOAD_FLOAT_IMM: {
				int reg = std::get<int>(instr.operands[0].value);
				double val = std::get<double>(instr.operands[1].value);
				ConstantValue cv;
				cv.type = ConstantValue::Type::FLOAT;
				cv.float_value = val;
				set_register_constant(reg, cv);
				new_instructions.push_back(instr);
				break;
			}

			case IROpcode::LOAD_BOOL: {
				int reg = std::get<int>(instr.operands[0].value);
				int64_t val = std::get<int64_t>(instr.operands[1].value);
				ConstantValue cv;
				cv.type = ConstantValue::Type::BOOL;
				cv.bool_value = (val != 0);
				set_register_constant(reg, cv);
				new_instructions.push_back(instr);
				break;
			}

			case IROpcode::LOAD_STRING: {
				int reg = std::get<int>(instr.operands[0].value);
				invalidate_register(reg);
				new_instructions.push_back(instr);
				break;
			}

			case IROpcode::MOVE: {
				int dst = std::get<int>(instr.operands[0].value);
				int src = std::get<int>(instr.operands[1].value);

				// Propagate constant value
				if (m_constants.count(src)) {
					m_constants[dst] = m_constants[src];
				} else {
					invalidate_register(dst);
				}
				new_instructions.push_back(instr);
				break;
			}

			case IROpcode::ADD:
			case IROpcode::SUB:
			case IROpcode::MUL:
			case IROpcode::DIV:
			case IROpcode::MOD: {
				// Check that all operands are registers before attempting constant folding
				if (instr.operands.size() < 3 ||
				    instr.operands[0].type != IRValue::Type::REGISTER ||
				    instr.operands[1].type != IRValue::Type::REGISTER ||
				    instr.operands[2].type != IRValue::Type::REGISTER) {
					// Can't fold if operands aren't all registers
					// Invalidate destination if it's a register
					if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
						int dst = std::get<int>(instr.operands[0].value);
						invalidate_register(dst);
					}
					new_instructions.push_back(instr);
					break;
				}

				int dst = std::get<int>(instr.operands[0].value);
				int lhs_reg = std::get<int>(instr.operands[1].value);
				int rhs_reg = std::get<int>(instr.operands[2].value);

				// Try to fold arithmetic operations
				if (m_constants.count(lhs_reg) && m_constants.count(rhs_reg)) {
					ConstantValue result;
					if (try_fold_binary_op(instr.opcode, instr.type_hint, m_constants[lhs_reg], m_constants[rhs_reg], result)) {
						// Replace with appropriate load instruction based on result type
						if (result.type == ConstantValue::Type::FLOAT) {
							new_instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(dst), IRValue::fimm(result.float_value));
							new_instructions.back().type_hint = IRInstruction::TypeHint::VARIANT_FLOAT;
						} else {
							new_instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(dst), IRValue::imm(result.int_value));
							if (instr.type_hint == IRInstruction::TypeHint::VARIANT_INT) {
								new_instructions.back().type_hint = IRInstruction::TypeHint::VARIANT_INT;
							}
						}
						set_register_constant(dst, result);
						folded = true;
					}
				}

				if (!folded) {
					invalidate_register(dst);
					new_instructions.push_back(instr);
				}
				break;
			}

			case IROpcode::CMP_EQ:
			case IROpcode::CMP_NEQ:
			case IROpcode::CMP_LT:
			case IROpcode::CMP_LTE:
			case IROpcode::CMP_GT:
			case IROpcode::CMP_GTE: {
				// Check that all operands are registers before attempting constant folding
				if (instr.operands.size() < 3 ||
				    instr.operands[0].type != IRValue::Type::REGISTER ||
				    instr.operands[1].type != IRValue::Type::REGISTER ||
				    instr.operands[2].type != IRValue::Type::REGISTER) {
					// Can't fold if operands aren't all registers
					if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
						int dst = std::get<int>(instr.operands[0].value);
						invalidate_register(dst);
					}
					new_instructions.push_back(instr);
					break;
				}

				int dst = std::get<int>(instr.operands[0].value);
				int lhs_reg = std::get<int>(instr.operands[1].value);
				int rhs_reg = std::get<int>(instr.operands[2].value);

				// Try to fold comparisons
				if (m_constants.count(lhs_reg) && m_constants.count(rhs_reg)) {
					ConstantValue result;
					if (try_fold_binary_op(instr.opcode, instr.type_hint, m_constants[lhs_reg], m_constants[rhs_reg], result)) {
						// Replace with LOAD_BOOL
						new_instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(dst), IRValue::imm(result.bool_value ? 1 : 0));
						set_register_constant(dst, result);
						folded = true;
					}
				}

				if (!folded) {
					invalidate_register(dst);
					new_instructions.push_back(instr);
				}
				break;
			}

			case IROpcode::NEG: {
				int dst = std::get<int>(instr.operands[0].value);
				int src = std::get<int>(instr.operands[1].value);

				if (m_constants.count(src)) {
					const auto& cv = m_constants[src];
					ConstantValue result;

					if (cv.type == ConstantValue::Type::INT) {
						result.type = ConstantValue::Type::INT;
						result.int_value = -cv.int_value;
						new_instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(dst), IRValue::imm(result.int_value));
						set_register_constant(dst, result);
						folded = true;
					} else if (cv.type == ConstantValue::Type::FLOAT) {
						result.type = ConstantValue::Type::FLOAT;
						result.float_value = -cv.float_value;
						new_instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(dst), IRValue::fimm(result.float_value));
						new_instructions.back().type_hint = IRInstruction::TypeHint::VARIANT_FLOAT;
						set_register_constant(dst, result);
						folded = true;
					}
				}

				if (!folded) {
					invalidate_register(dst);
					new_instructions.push_back(instr);
				}
				break;
			}

			case IROpcode::NOT: {
				int dst = std::get<int>(instr.operands[0].value);
				int src = std::get<int>(instr.operands[1].value);

				if (m_constants.count(src) && m_constants[src].type == ConstantValue::Type::BOOL) {
					ConstantValue result;
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = !m_constants[src].bool_value;
					new_instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(dst), IRValue::imm(result.bool_value ? 1 : 0));
					set_register_constant(dst, result);
					folded = true;
				}

				if (!folded) {
					invalidate_register(dst);
					new_instructions.push_back(instr);
				}
				break;
			}

			case IROpcode::AND:
			case IROpcode::OR: {
				// Check that all operands are registers before attempting constant folding
				if (instr.operands.size() < 3 ||
				    instr.operands[0].type != IRValue::Type::REGISTER ||
				    instr.operands[1].type != IRValue::Type::REGISTER ||
				    instr.operands[2].type != IRValue::Type::REGISTER) {
					// Can't fold if operands aren't all registers
					if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
						int dst = std::get<int>(instr.operands[0].value);
						invalidate_register(dst);
					}
					new_instructions.push_back(instr);
					break;
				}

				int dst = std::get<int>(instr.operands[0].value);
				int lhs_reg = std::get<int>(instr.operands[1].value);
				int rhs_reg = std::get<int>(instr.operands[2].value);

				// Try to fold logical operations
				if (m_constants.count(lhs_reg) && m_constants.count(rhs_reg)) {
					const auto& lhs_cv = m_constants[lhs_reg];
					const auto& rhs_cv = m_constants[rhs_reg];

					// Only fold if both are boolean constants
					if (lhs_cv.type == ConstantValue::Type::BOOL && rhs_cv.type == ConstantValue::Type::BOOL) {
						ConstantValue result;
						result.type = ConstantValue::Type::BOOL;

						if (instr.opcode == IROpcode::AND) {
							result.bool_value = lhs_cv.bool_value && rhs_cv.bool_value;
						} else {
							result.bool_value = lhs_cv.bool_value || rhs_cv.bool_value;
						}

						new_instructions.emplace_back(IROpcode::LOAD_BOOL, IRValue::reg(dst), IRValue::imm(result.bool_value ? 1 : 0));
						set_register_constant(dst, result);
						folded = true;
					}
				}

				if (!folded) {
					invalidate_register(dst);
					new_instructions.push_back(instr);
				}
				break;
			}

			// System calls and variant operations - invalidate destination only
			case IROpcode::VCALL:
			case IROpcode::VGET:
			case IROpcode::VSET:
			case IROpcode::CALL_SYSCALL:
			case IROpcode::CALL:
				// These write to the first operand (destination register)
				// but we shouldn't invalidate the input operands
				if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
					invalidate_register(std::get<int>(instr.operands[0].value));
				}
				new_instructions.push_back(instr);
				break;

			default:
				// Clear constant tracking for any instruction that might modify registers
				for (const auto& op : instr.operands) {
					if (op.type == IRValue::Type::REGISTER) {
						int reg = std::get<int>(op.value);
						invalidate_register(reg);
					}
				}
				new_instructions.push_back(instr);
				break;
		}
	}

	func.instructions = std::move(new_instructions);
}

bool IROptimizer::try_fold_binary_op(IROpcode op, IRInstruction::TypeHint type_hint, const ConstantValue& lhs, const ConstantValue& rhs, ConstantValue& result) {
	// Handle float arithmetic - if type_hint is VARIANT_FLOAT or either operand is float,
	// we must perform float arithmetic (GDScript semantics)
	bool is_float_op = (type_hint == IRInstruction::TypeHint::VARIANT_FLOAT ||
	                    lhs.type == ConstantValue::Type::FLOAT ||
	                    rhs.type == ConstantValue::Type::FLOAT);

	// For arithmetic operations, promote to float if needed
	if (is_float_op) {
		// Convert int operands to float for the operation
		double lhs_val = (lhs.type == ConstantValue::Type::FLOAT) ? lhs.float_value : static_cast<double>(lhs.int_value);
		double rhs_val = (rhs.type == ConstantValue::Type::FLOAT) ? rhs.float_value : static_cast<double>(rhs.int_value);

		switch (op) {
			case IROpcode::ADD:
				result.type = ConstantValue::Type::FLOAT;
				result.float_value = lhs_val + rhs_val;
				return true;

			case IROpcode::SUB:
				result.type = ConstantValue::Type::FLOAT;
				result.float_value = lhs_val - rhs_val;
				return true;

			case IROpcode::MUL:
				result.type = ConstantValue::Type::FLOAT;
				result.float_value = lhs_val * rhs_val;
				return true;

			case IROpcode::DIV:
				if (rhs_val == 0.0) return false; // Don't fold division by zero
				result.type = ConstantValue::Type::FLOAT;
				result.float_value = lhs_val / rhs_val;
				return true;

			case IROpcode::MOD:
				if (rhs_val == 0.0) return false; // Don't fold modulo by zero
				result.type = ConstantValue::Type::FLOAT;
				result.float_value = std::fmod(lhs_val, rhs_val);
				return true;

			default:
				break; // Fall through to comparison handling
		}
	}

	// Handle integer arithmetic (only when both operands are int and not a float operation)
	if (!is_float_op && lhs.type == ConstantValue::Type::INT && rhs.type == ConstantValue::Type::INT) {
		switch (op) {
			case IROpcode::ADD:
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value + rhs.int_value;
				return true;

			case IROpcode::SUB:
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value - rhs.int_value;
				return true;

			case IROpcode::MUL:
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value * rhs.int_value;
				return true;

			case IROpcode::DIV:
				if (rhs.int_value == 0) return false; // Don't fold division by zero
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value / rhs.int_value;
				return true;

			case IROpcode::MOD:
				if (rhs.int_value == 0) return false; // Don't fold modulo by zero
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value % rhs.int_value;
				return true;

			default:
				break; // Fall through to comparison handling
		}
	}

	// Handle comparisons - work for both int and float
	bool comparable = (lhs.type == ConstantValue::Type::INT && rhs.type == ConstantValue::Type::INT) ||
	                  (lhs.type == ConstantValue::Type::FLOAT && rhs.type == ConstantValue::Type::FLOAT) ||
	                  (lhs.type == ConstantValue::Type::INT && rhs.type == ConstantValue::Type::FLOAT) ||
	                  (lhs.type == ConstantValue::Type::FLOAT && rhs.type == ConstantValue::Type::INT);

	if (comparable) {
		// Get comparable values
		bool lhs_is_float = (lhs.type == ConstantValue::Type::FLOAT);
		bool rhs_is_float = (rhs.type == ConstantValue::Type::FLOAT);

		if (lhs_is_float || rhs_is_float) {
			// Float comparison
			double lhs_val = lhs_is_float ? lhs.float_value : static_cast<double>(lhs.int_value);
			double rhs_val = rhs_is_float ? rhs.float_value : static_cast<double>(rhs.int_value);

			switch (op) {
				case IROpcode::CMP_EQ:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs_val == rhs_val);
					return true;

				case IROpcode::CMP_NEQ:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs_val != rhs_val);
					return true;

				case IROpcode::CMP_LT:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs_val < rhs_val);
					return true;

				case IROpcode::CMP_LTE:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs_val <= rhs_val);
					return true;

				case IROpcode::CMP_GT:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs_val > rhs_val);
					return true;

				case IROpcode::CMP_GTE:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs_val >= rhs_val);
					return true;

				default:
					return false;
			}
		} else {
			// Integer comparison
			switch (op) {
				case IROpcode::CMP_EQ:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs.int_value == rhs.int_value);
					return true;

				case IROpcode::CMP_NEQ:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs.int_value != rhs.int_value);
					return true;

				case IROpcode::CMP_LT:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs.int_value < rhs.int_value);
					return true;

				case IROpcode::CMP_LTE:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs.int_value <= rhs.int_value);
					return true;

				case IROpcode::CMP_GT:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs.int_value > rhs.int_value);
					return true;

				case IROpcode::CMP_GTE:
					result.type = ConstantValue::Type::BOOL;
					result.bool_value = (lhs.int_value >= rhs.int_value);
					return true;

				default:
					return false;
			}
		}
	}

	return false;
}

void IROptimizer::peephole_optimization(IRFunction& func) {
	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());

	size_t i = 0;
	while (i < func.instructions.size()) {
		const auto& instr = func.instructions[i];
		bool skip = false;
		bool handled = false;

		// Pattern 1: MOVE r0, r0 -> eliminate
		if (instr.opcode == IROpcode::MOVE) {
			int dst = std::get<int>(instr.operands[0].value);
			int src = std::get<int>(instr.operands[1].value);
			if (dst == src) {
				skip = true; // Eliminate self-move
				i++;
				continue;
			}

			// Pattern 2: Eliminate redundant MOVEs around arithmetic/logical operations
			// Pattern A: MOVE tmp1, src1; MOVE tmp2, src2; OP dst, tmp1, tmp2; MOVE result, dst
			//          -> OP result, src1, src2
			// Pattern B/C: MOVE tmp, src; OP dst, ..., tmp; MOVE result, dst
			//          -> OP result, ..., src
			//
			// Only apply if:
			// - The temporary registers (tmp1, tmp2, dst) are never used elsewhere
			// - No control flow boundaries between the instructions
			if (!skip && !handled && i + 3 < func.instructions.size()) {
				const auto& move1 = func.instructions[i];
				const auto& move2 = func.instructions[i + 1];
				const auto& op = func.instructions[i + 2];
				const auto& move3 = func.instructions[i + 3];

				// Check for Pattern A: MOVE; MOVE; OP; MOVE
				if (move2.opcode == IROpcode::MOVE &&
				    is_arithmetic_op(op.opcode) &&
				    move3.opcode == IROpcode::MOVE &&
				    op.operands.size() >= 3) {

					int move1_dst = std::get<int>(move1.operands[0].value);
					int move1_src = std::get<int>(move1.operands[1].value);
					int move2_dst = std::get<int>(move2.operands[0].value);
					int move2_src = std::get<int>(move2.operands[1].value);
					int op_dst = std::get<int>(op.operands[0].value);
					int move3_dst = std::get<int>(move3.operands[0].value);
					int move3_src = std::get<int>(move3.operands[1].value);

					// Check if operands match the pattern
					if (op.operands[1].type == IRValue::Type::REGISTER &&
					    op.operands[2].type == IRValue::Type::REGISTER) {
						int op_lhs = std::get<int>(op.operands[1].value);
						int op_rhs = std::get<int>(op.operands[2].value);

						// Pattern A: tmp1=move1_dst, tmp2=move2_dst, result=move3_dst
						if (move1_dst == op_lhs && move2_dst == op_rhs &&
						    move3_src == op_dst) {
							// Check that temps are not used elsewhere
							// tmp1 (move1_dst) should only be used by the OP at i+2
							// tmp2 (move2_dst) should only be used by the OP at i+2
							// dst (op_dst) should only be used by the MOVE at i+3
							bool tmp1_safe = !is_reg_used_between_exclusive(func, move1_dst, i, i + 2);
							bool tmp2_safe = !is_reg_used_between_exclusive(func, move2_dst, i + 1, i + 2);
							bool dst_safe = !is_reg_used_between_exclusive(func, op_dst, i + 2, i + 3);

							if (tmp1_safe && tmp2_safe && dst_safe) {
								// Emit optimized instruction
								IRInstruction new_op = op;
								new_op.operands[0] = IRValue::reg(move3_dst);
								new_op.operands[1] = IRValue::reg(move1_src);
								new_op.operands[2] = IRValue::reg(move2_src);
								new_instructions.push_back(new_op);

								i += 4;
								skip = true;
								handled = true;
							}
						}
					}
				}
			}

			// Pattern B/C: MOVE; OP; MOVE (only one move before op)
			if (!skip && !handled && i + 2 < func.instructions.size()) {
				const auto& move1 = func.instructions[i];
				const auto& op = func.instructions[i + 1];
				const auto& move2 = func.instructions[i + 2];

				if (is_arithmetic_op(op.opcode) &&
				    move2.opcode == IROpcode::MOVE &&
				    op.operands.size() >= 3) {

					int move1_dst = std::get<int>(move1.operands[0].value);
					int move1_src = std::get<int>(move1.operands[1].value);
					int op_dst = std::get<int>(op.operands[0].value);
					int move2_dst = std::get<int>(move2.operands[0].value);
					int move2_src = std::get<int>(move2.operands[1].value);

					// Check for Pattern B: tmp is first operand
					if (op.operands[1].type == IRValue::Type::REGISTER) {
						int op_lhs = std::get<int>(op.operands[1].value);

						if (move1_dst == op_lhs && move2_src == op_dst &&
						    !is_reg_used_between_exclusive(func, move1_dst, i, i + 1) &&
						    !is_reg_used_between_exclusive(func, op_dst, i + 1, i + 2)) {

							// Emit optimized instruction
							IRInstruction new_op = op;
							new_op.operands[0] = IRValue::reg(move2_dst);
							new_op.operands[1] = IRValue::reg(move1_src);
							// operand 2 stays the same
							new_instructions.push_back(new_op);

							i += 3;
							skip = true;
							handled = true;
						}
					}

					// Check for Pattern C: tmp is second operand
					if (!handled && op.operands[2].type == IRValue::Type::REGISTER) {
						int op_rhs = std::get<int>(op.operands[2].value);

						if (move1_dst == op_rhs && move2_src == op_dst &&
						    !is_reg_used_between_exclusive(func, move1_dst, i, i + 1) &&
						    !is_reg_used_between_exclusive(func, op_dst, i + 1, i + 2)) {

							// Emit optimized instruction
							IRInstruction new_op = op;
							new_op.operands[0] = IRValue::reg(move2_dst);
							// operand 1 stays the same
							new_op.operands[2] = IRValue::reg(move1_src);
							new_instructions.push_back(new_op);

							i += 3;
							skip = true;
							handled = true;
						}
					}
				}
			}

			// Pattern E: MOVE tmp, var; LOAD_IMM/LOAD_FLOAT_IMM const; OP dst, tmp, const; MOVE var, dst
			//          -> OP var, var, const
			// This handles the common "count = count + 1" pattern efficiently
			if (!skip && !handled && i + 3 < func.instructions.size()) {
				const auto& move1 = func.instructions[i];
				const auto& load = func.instructions[i + 1];
				const auto& op = func.instructions[i + 2];
				const auto& move2 = func.instructions[i + 3];

				if (move1.opcode == IROpcode::MOVE &&
				    (load.opcode == IROpcode::LOAD_IMM || load.opcode == IROpcode::LOAD_FLOAT_IMM) &&
				    is_arithmetic_op(op.opcode) &&
				    move2.opcode == IROpcode::MOVE &&
				    op.operands.size() >= 3) {

					int move1_dst = std::get<int>(move1.operands[0].value);
					int move1_src = std::get<int>(move1.operands[1].value);
					int load_dst = std::get<int>(load.operands[0].value);
					int op_dst = std::get<int>(op.operands[0].value);
					int move2_dst = std::get<int>(move2.operands[0].value);
					int move2_src = std::get<int>(move2.operands[1].value);

					// Check if operands match the pattern: MOVE tmp, var; LOAD const; OP dst, tmp, const; MOVE var, dst
					if (op.operands[1].type == IRValue::Type::REGISTER &&
					    op.operands[2].type == IRValue::Type::REGISTER) {
						int op_lhs = std::get<int>(op.operands[1].value);
						int op_rhs = std::get<int>(op.operands[2].value);

						// Pattern E requires: tmp=move1_dst=op_lhs, const=load_dst=op_rhs, var=move1_src=move2_dst, dst=op_dst=move2_src
						if (move1_dst == op_lhs && load_dst == op_rhs &&
						    move1_src == move2_dst && move2_src == op_dst) {
							// Check that temps are not used elsewhere (excluding the OP instruction at i+2)
							// tmp1 (move1_dst) should only be used by OP at i+2 - check before OP
							bool tmp1_safe = !is_reg_used_between_exclusive(func, move1_dst, i, i + 2);
							// const (load_dst) should only be used by OP at i+2 - check before and after (excluding OP)
							bool tmp2_safe = !is_reg_used_between_exclusive(func, load_dst, i + 1, i + 2) &&
							                 !is_reg_used_between_exclusive(func, load_dst, i + 2, i + 3);
							// dst (op_dst) should only be used by MOVE at i+3 - check between OP and MOVE
							bool dst_safe = !is_reg_used_between_exclusive(func, op_dst, i + 2, i + 3);

							if (tmp1_safe && tmp2_safe && dst_safe) {
								// Emit the LOAD_IMM instruction first (we still need the constant)
								new_instructions.push_back(load);

								// Emit optimized instruction: OP var, var, const
								IRInstruction new_op = op;
								new_op.operands[0] = IRValue::reg(move2_dst);  // var as destination
								new_op.operands[1] = IRValue::reg(move1_src);  // var as first operand
								// operand 2 (const) stays the same (load_dst)
								new_instructions.push_back(new_op);

								i += 4;
								skip = true;
								handled = true;
							}
						}
					}
				}
			}
		}

		// Pattern F: Handle MOVE r10, r0; MOVE r0, r10 -> eliminate (redundant pair)
		if (!skip && !handled && i + 1 < func.instructions.size()) {
			if (func.instructions[i].opcode == IROpcode::MOVE &&
			    func.instructions[i + 1].opcode == IROpcode::MOVE) {
				const auto& move1 = func.instructions[i];
				const auto& move2 = func.instructions[i + 1];

				int move1_dst = std::get<int>(move1.operands[0].value);
				int move1_src = std::get<int>(move1.operands[1].value);
				int move2_dst = std::get<int>(move2.operands[0].value);
				int move2_src = std::get<int>(move2.operands[1].value);

				// Check for: MOVE tmp, src; MOVE src, tmp
				// This is a redundant swap pattern - just eliminate both
				if (move1_dst == move2_src && move1_src == move2_dst && move1_dst != move1_src) {
					// Both moves are redundant - eliminate them
					i += 2;
					skip = true;
					handled = true;
					continue;
				}
			}
		}

		// Pattern D: OP dst, ...; MOVE result, dst (without preceding MOVE)
		if (!skip && !handled && i + 1 < func.instructions.size()) {
			if (is_arithmetic_op(func.instructions[i].opcode) &&
			    func.instructions[i + 1].opcode == IROpcode::MOVE) {

				const auto& op = func.instructions[i];
				const auto& move = func.instructions[i + 1];

				if (op.operands.size() >= 1 && move.operands.size() >= 2) {
					int op_dst = std::get<int>(op.operands[0].value);
					int move_dst = std::get<int>(move.operands[0].value);
					int move_src = std::get<int>(move.operands[1].value);

					if (move_src == op_dst &&
					    !is_reg_used_between_exclusive(func, op_dst, i, i + 2)) {

						// Emit optimized instruction
						IRInstruction new_op = op;
						new_op.operands[0] = IRValue::reg(move_dst);
						new_instructions.push_back(new_op);

						i += 2;
						skip = true;
						handled = true;
					}
				}
			}
		}

		if (!skip && !handled) {
			new_instructions.push_back(instr);
			i++;
		}
	}

	func.instructions = std::move(new_instructions);
}

void IROptimizer::copy_propagation(IRFunction& func) {
	// This optimization eliminates redundant MOVE instructions after constant loads.
	// Pattern: LOAD_IMM r0, 5; MOVE r1, r0; ... (r0 never used again)
	// Optimize to: LOAD_IMM r1, 5

	// Track constant values in registers
	struct ConstantInfo {
		IROpcode opcode;
		IRValue value;  // The actual constant value
	};

	std::unordered_map<int, ConstantInfo> constant_regs;
	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());

	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		// Clear constant tracking at labels (control flow boundaries)
		if (instr.opcode == IROpcode::LABEL) {
			constant_regs.clear();
		}

		// Mark the destination register as "killed" - it's no longer a constant we can propagate
		if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
			int dst = std::get<int>(instr.operands[0].value);
			constant_regs.erase(dst);
		}

		// Track constant loads
		if (instr.opcode == IROpcode::LOAD_IMM || instr.opcode == IROpcode::LOAD_FLOAT_IMM) {
			if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER &&
			    instr.operands.size() >= 2) {
				int dst = std::get<int>(instr.operands[0].value);
				constant_regs[dst] = {instr.opcode, instr.operands[1]};
			}
		}

		// Try to optimize MOVE instructions
		if (instr.opcode == IROpcode::MOVE) {
			int dst = std::get<int>(instr.operands[0].value);
			int src = std::get<int>(instr.operands[1].value);

			// Check if source is a constant we just loaded
			if (constant_regs.count(src)) {
				// Replace MOVE with the appropriate constant load
				const auto& info = constant_regs[src];
				new_instructions.emplace_back(info.opcode, IRValue::reg(dst), info.value);
				// The new constant is now in dst
				constant_regs[dst] = info;
			} else {
				// Keep the original MOVE
				new_instructions.push_back(instr);
			}
		} else {
			new_instructions.push_back(instr);
		}
	}

	func.instructions = std::move(new_instructions);
}

void IROptimizer::eliminate_dead_code(IRFunction& func) {
	// Find all live registers (used after definition)
	auto live_regs = find_live_registers(func);

	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());

	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];
		bool keep = true;

		// Check if instruction produces a dead register
		// BUT: be conservative - only eliminate truly "pure" operations
		// Do NOT eliminate operations that might have side effects or are
		// part of control flow setup
		switch (instr.opcode) {
			case IROpcode::LOAD_IMM:
			case IROpcode::LOAD_BOOL: {
				// These are pure and can be safely eliminated if unused
				int dst = std::get<int>(instr.operands[0].value);
				if (live_regs.find(dst) == live_regs.end()) {
					keep = false; // Dead code
				}
				break;
			}

			// DO NOT eliminate these even if result appears unused:
			// - MOVE: might be part of calling convention
			// - Arithmetic ops: the input registers might have side effects
			// - Comparisons: often used for control flow
			// - LOAD_STRING: might be needed for vcall
			// Better to keep them than to break the program

			default:
				// Keep all other instructions
				break;
		}

		if (keep) {
			new_instructions.push_back(instr);
		}
	}

	func.instructions = std::move(new_instructions);
}

std::unordered_set<int> IROptimizer::find_live_registers(const IRFunction& func) {
	std::unordered_set<int> live;

	// Mark all read registers as live
	for (const auto& instr : func.instructions) {
		switch (instr.opcode) {
			// Branch instructions - FIRST operand is the register to check
			case IROpcode::BRANCH_ZERO:
			case IROpcode::BRANCH_NOT_ZERO:
				// Mark the first operand (register to check) as live
				if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
					live.insert(std::get<int>(instr.operands[0].value));
				}
				break;

			// Instructions that read from operands (not just write)
			case IROpcode::MOVE:
			case IROpcode::ADD:
			case IROpcode::SUB:
			case IROpcode::MUL:
			case IROpcode::DIV:
			case IROpcode::MOD:
			case IROpcode::NEG:
			case IROpcode::NOT:
			case IROpcode::AND:     // Logical AND reads operands
			case IROpcode::OR:      // Logical OR reads operands
			case IROpcode::CMP_EQ:
			case IROpcode::CMP_NEQ:
			case IROpcode::CMP_LT:
			case IROpcode::CMP_LTE:
			case IROpcode::CMP_GT:
			case IROpcode::CMP_GTE:
			case IROpcode::VCALL:
			case IROpcode::VGET:
			case IROpcode::VSET:
			case IROpcode::CALL:
			case IROpcode::CALL_SYSCALL:
			// Inline primitive construction - these read from their argument registers
			case IROpcode::MAKE_VECTOR2:
			case IROpcode::MAKE_VECTOR3:
			case IROpcode::MAKE_VECTOR4:
			case IROpcode::MAKE_VECTOR2I:
			case IROpcode::MAKE_VECTOR3I:
			case IROpcode::MAKE_VECTOR4I:
			case IROpcode::MAKE_COLOR:
			case IROpcode::MAKE_RECT2:
			case IROpcode::MAKE_RECT2I:
			case IROpcode::MAKE_PLANE:
			case IROpcode::MAKE_ARRAY:
			case IROpcode::MAKE_DICTIONARY:
			// Inline member access - these read from the object register
			case IROpcode::VGET_INLINE:
			case IROpcode::VSET_INLINE:
				// Mark all register operands (except first for most ops) as live
				for (size_t i = 1; i < instr.operands.size(); i++) {
					if (instr.operands[i].type == IRValue::Type::REGISTER) {
						live.insert(std::get<int>(instr.operands[i].value));
					}
				}
				break;

			case IROpcode::RETURN:
				// Return reads from register 0
				if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
					live.insert(std::get<int>(instr.operands[0].value));
				} else {
					// Implicit return of r0
					live.insert(0);
				}
				break;

			default:
				break;
		}
	}

	return live;
}

bool IROptimizer::is_register_used_after(const IRFunction& func, int reg, size_t instr_idx) {
	for (size_t i = instr_idx; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		// Check if register is read
		for (size_t j = 0; j < instr.operands.size(); j++) {
			if (instr.operands[j].type == IRValue::Type::REGISTER) {
				int r = std::get<int>(instr.operands[j].value);
				if (r == reg) {
					// For most instructions, first operand is destination, rest are sources
					// Exception: BRANCH_ZERO, BRANCH_NOT_ZERO read first operand
					if (j > 0 || instr.opcode == IROpcode::BRANCH_ZERO || instr.opcode == IROpcode::BRANCH_NOT_ZERO) {
						return true;
					}
				}
			}
		}

		// Check if register is written (kills liveness)
		switch (instr.opcode) {
			case IROpcode::LOAD_IMM:
			case IROpcode::LOAD_BOOL:
			case IROpcode::LOAD_STRING:
			case IROpcode::MOVE:
			case IROpcode::ADD:
			case IROpcode::SUB:
			case IROpcode::MUL:
			case IROpcode::DIV:
			case IROpcode::MOD:
			case IROpcode::NEG:
			case IROpcode::NOT:
			case IROpcode::CMP_EQ:
			case IROpcode::CMP_NEQ:
			case IROpcode::CMP_LT:
			case IROpcode::CMP_LTE:
			case IROpcode::CMP_GT:
			case IROpcode::CMP_GTE:
				if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
					int dst = std::get<int>(instr.operands[0].value);
					if (dst == reg) {
						return false; // Register is overwritten before use
					}
				}
				break;

			default:
				break;
		}
	}

	return false;
}

void IROptimizer::reduce_register_pressure(IRFunction& func) {
	// Build a mapping of old register numbers to new (compacted) register numbers
	std::unordered_map<int, int> reg_map;
	int next_reg = 0;

	// First pass: identify all used registers and assign new numbers
	for (const auto& instr : func.instructions) {
		for (const auto& op : instr.operands) {
			if (op.type == IRValue::Type::REGISTER) {
				int old_reg = std::get<int>(op.value);
				if (reg_map.find(old_reg) == reg_map.end()) {
					reg_map[old_reg] = next_reg++;
				}
			}
		}
	}

	// Second pass: rewrite all register references
	for (auto& instr : func.instructions) {
		for (auto& op : instr.operands) {
			if (op.type == IRValue::Type::REGISTER) {
				int old_reg = std::get<int>(op.value);
				op.value = reg_map[old_reg];
			}
		}
	}
}

void IROptimizer::set_register_constant(int reg, const ConstantValue& value) {
	m_constants[reg] = value;
}

void IROptimizer::invalidate_register(int reg) {
	m_constants.erase(reg);
}

IROptimizer::ConstantValue IROptimizer::get_constant(const IRValue& val) {
	ConstantValue cv;
	if (val.type == IRValue::Type::IMMEDIATE) {
		cv.type = ConstantValue::Type::INT;
		cv.int_value = std::get<int64_t>(val.value);
	}
	return cv;
}

bool IROptimizer::is_arithmetic_op(IROpcode op) {
	switch (op) {
		case IROpcode::ADD:
		case IROpcode::SUB:
		case IROpcode::MUL:
		case IROpcode::DIV:
		case IROpcode::MOD:
		case IROpcode::NEG:
		case IROpcode::AND:
		case IROpcode::OR:
		case IROpcode::CMP_EQ:
		case IROpcode::CMP_NEQ:
		case IROpcode::CMP_LT:
		case IROpcode::CMP_LTE:
		case IROpcode::CMP_GT:
		case IROpcode::CMP_GTE:
			return true;
		default:
			return false;
	}
}

bool IROptimizer::is_reg_used_between_exclusive(const IRFunction& func, int reg, size_t start_idx, size_t end_idx) {
	// Check if register is used in the instruction range (start_idx, end_idx)
	// Exclusive of both boundaries

	// Safety check
	if (start_idx >= func.instructions.size() || end_idx > func.instructions.size()) {
		return true; // Conservative: assume used if indices are out of bounds
	}

	for (size_t i = start_idx + 1; i < end_idx; i++) {
		const auto& instr = func.instructions[i];

		// LABEL is a control flow boundary - be conservative
		if (instr.opcode == IROpcode::LABEL) {
			return true;
		}

		// Check if register is read (used as source operand)
		for (size_t j = 1; j < instr.operands.size(); j++) {
			if (instr.operands[j].type == IRValue::Type::REGISTER) {
				int r = std::get<int>(instr.operands[j].value);
				if (r == reg) {
					return true;
				}
			}
		}

		// Special case: BRANCH_ZERO and BRANCH_NOT_ZERO read first operand
		if ((instr.opcode == IROpcode::BRANCH_ZERO || instr.opcode == IROpcode::BRANCH_NOT_ZERO) &&
		    instr.operands.size() >= 1 && instr.operands[0].type == IRValue::Type::REGISTER) {
			int r = std::get<int>(instr.operands[0].value);
			if (r == reg) {
				return true;
			}
		}
	}

	return false;
}

static void flush_pending(std::vector<IRInstruction>& new_instructions,
	std::unordered_map<int, size_t>& pending_stores, const IRFunction& func)
{
	if (!pending_stores.empty()) {
		// Sort by original instruction index to maintain order
		std::vector<std::pair<int, size_t>> sorted(pending_stores.begin(), pending_stores.end());
		std::sort(sorted.begin(), sorted.end(),
		         [](const auto& a, const auto& b) { return a.second < b.second; });

		for (const auto& [reg, idx] : sorted) {
			new_instructions.push_back(func.instructions[idx]);
		}
		pending_stores.clear();
	}
}
static bool reads_pending_store(const IRInstruction& instr,
	const std::unordered_map<int, size_t>& pending_stores)
{
	for (size_t i = 1; i < instr.operands.size(); i++) {
		const auto& op = instr.operands[i];
		if (op.type == IRValue::Type::REGISTER) {
			int reg = std::get<int>(op.value);
			if (pending_stores.count(reg)) {
				return true;
			}
		}
	}
	// Special case: BRANCH_ZERO and BRANCH_NOT_ZERO read the first operand
	if ((instr.opcode == IROpcode::BRANCH_ZERO || instr.opcode == IROpcode::BRANCH_NOT_ZERO) &&
		!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
		int reg = std::get<int>(instr.operands[0].value);
		if (pending_stores.count(reg)) {
			return true;
		}
	}
	return false;
}

void IROptimizer::eliminate_redundant_stores(IRFunction& func) {
	// This pass eliminates redundant store operations:
	// 1. Dead stores: Store to a register that is immediately overwritten
	//    Example: LOAD_IMM r0, 10; LOAD_IMM r0, 20  -> first instruction is dead
	// 2. Consecutive identical stores: Same value stored to same register
	//    Example: LOAD_IMM r0, 10; LOAD_IMM r0, 10  -> second is redundant
	//
	// This is done by delaying the emission of store instructions until we're sure
	// they're not dead (overwritten without being read) or redundant (same value).

	if (func.instructions.empty()) {
		return;
	}

	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());

	// Track pending stores that haven't been emitted yet
	std::unordered_map<int, size_t> pending_stores;  // reg -> instruction index

	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		// Control flow boundaries - flush all pending stores
		if (instr.opcode == IROpcode::LABEL ||
		    instr.opcode == IROpcode::JUMP ||
		    instr.opcode == IROpcode::BRANCH_ZERO ||
		    instr.opcode == IROpcode::BRANCH_NOT_ZERO ||
		    instr.opcode == IROpcode::CALL ||
		    instr.opcode == IROpcode::VCALL ||
		    instr.opcode == IROpcode::CALL_SYSCALL ||
		    instr.opcode == IROpcode::RETURN) {
			flush_pending(new_instructions, pending_stores, func);
			new_instructions.push_back(instr);
			continue;
		}

		// Check if this instruction reads from a register with a pending store
		if (reads_pending_store(instr, pending_stores)) {
			flush_pending(new_instructions, pending_stores, func);
		}

		// Check if this is a pure load/store operation to a register
		if (is_pure_load_op(instr.opcode) && !instr.operands.empty() &&
		    instr.operands[0].type == IRValue::Type::REGISTER) {
			int dst = std::get<int>(instr.operands[0].value);

			// Check if we have a previous pending store to the same register
			if (pending_stores.count(dst)) {
				size_t prev_idx = pending_stores[dst];
				const auto& prev_instr = func.instructions[prev_idx];

				// Check if stores are identical (same opcode and value)
				bool is_identical = (prev_instr.opcode == instr.opcode &&
				                     prev_instr.operands.size() == instr.operands.size());

				if (is_identical && instr.operands.size() >= 2) {
					const auto& curr_val = instr.operands[1];
					const auto& prev_val = prev_instr.operands[1];

					if (curr_val.type != prev_val.type) {
						is_identical = false;
					} else if (curr_val.type == IRValue::Type::IMMEDIATE) {
						is_identical = (std::get<int64_t>(curr_val.value) ==
						                std::get<int64_t>(prev_val.value));
					} else if (curr_val.type == IRValue::Type::FLOAT) {
						is_identical = (std::get<double>(curr_val.value) ==
						                std::get<double>(prev_val.value));
					} else if (curr_val.type == IRValue::Type::REGISTER) {
						// For MOVE: compare source register
						is_identical = (std::get<int>(curr_val.value) ==
						                std::get<int>(prev_val.value));
					} else {
						is_identical = false;
					}
				}

				if (is_identical) {
					// Consecutive identical stores - skip current one
					continue;
				}

				// Different store to same register - replace previous with this one
				pending_stores[dst] = i;
				continue;
			}

			// First store to this register - track it as pending
			pending_stores[dst] = i;
			continue;
		}

		// Not a store operation - add it directly
		new_instructions.push_back(instr);
	}

	// Flush any remaining pending stores
	flush_pending(new_instructions, pending_stores, func);

	func.instructions = std::move(new_instructions);
}

bool IROptimizer::is_pure_load_op(IROpcode op) {
	switch (op) {
		case IROpcode::LOAD_IMM:
		case IROpcode::LOAD_FLOAT_IMM:
		case IROpcode::LOAD_BOOL:
		case IROpcode::LOAD_STRING:
		case IROpcode::MOVE:
			return true;
		default:
			return false;
	}
}

} // namespace gdscript
