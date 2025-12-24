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
	// TEMPORARILY DISABLED - suspected to be causing test failures
	// copy_propagation(func);

	// Peephole optimization to remove redundant moves and operations
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

	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];
		bool skip = false;

		// Pattern 1: MOVE r0, r0 -> eliminate
		if (instr.opcode == IROpcode::MOVE) {
			int dst = std::get<int>(instr.operands[0].value);
			int src = std::get<int>(instr.operands[1].value);
			if (dst == src) {
				skip = true; // Eliminate self-move
			}
		}

		// Pattern 2: MOVE r1, r0; MOVE r2, r1 -> MOVE r2, r0 (if r1 not used after)
		// BUT: don't do this optimization - it's too risky with loops
		// The is_register_used_after check doesn't account for control flow (jumps/branches)
		// where a register might be used by an earlier instruction via a jump
		//
		// Disabled for safety - the gain is minimal anyway
		/*
		if (!skip && instr.opcode == IROpcode::MOVE && i + 1 < func.instructions.size()) {
			const auto& next = func.instructions[i + 1];
			if (next.opcode == IROpcode::MOVE) {
				int dst1 = std::get<int>(instr.operands[0].value);
				int src1 = std::get<int>(instr.operands[1].value);
				int dst2 = std::get<int>(next.operands[0].value);
				int src2 = std::get<int>(next.operands[1].value);

				if (dst1 == src2 && !is_register_used_after(func, dst1, i + 2)) {
					// Merge into single move
					new_instructions.emplace_back(IROpcode::MOVE, IRValue::reg(dst2), IRValue::reg(src1));
					i++; // Skip next instruction
					skip = true;
				}
			}
		}
		*/

		if (!skip) {
			new_instructions.push_back(instr);
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

} // namespace gdscript
