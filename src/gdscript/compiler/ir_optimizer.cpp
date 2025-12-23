#include "ir_optimizer.h"
#include <algorithm>

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
				// Disable constant folding for arithmetic operations
				// because floats are stored as bit patterns and shouldn't be added as integers
				int dst = std::get<int>(instr.operands[0].value);
				invalidate_register(dst);
				new_instructions.push_back(instr);
				break;
			}

			case IROpcode::CMP_EQ:
			case IROpcode::CMP_NEQ:
			case IROpcode::CMP_LT:
			case IROpcode::CMP_LTE:
			case IROpcode::CMP_GT:
			case IROpcode::CMP_GTE: {
				int dst = std::get<int>(instr.operands[0].value);
				int lhs_reg = std::get<int>(instr.operands[1].value);
				int rhs_reg = std::get<int>(instr.operands[2].value);

				// Try to fold comparisons
				if (m_constants.count(lhs_reg) && m_constants.count(rhs_reg)) {
					ConstantValue result;
					if (try_fold_binary_op(instr.opcode, m_constants[lhs_reg], m_constants[rhs_reg], result)) {
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

				if (m_constants.count(src) && m_constants[src].type == ConstantValue::Type::INT) {
					ConstantValue result;
					result.type = ConstantValue::Type::INT;
					result.int_value = -m_constants[src].int_value;
					new_instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(dst), IRValue::imm(result.int_value));
					set_register_constant(dst, result);
					folded = true;
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

bool IROptimizer::try_fold_binary_op(IROpcode op, const ConstantValue& lhs, const ConstantValue& rhs, ConstantValue& result) {
	// Only fold integer arithmetic for now
	if (lhs.type != ConstantValue::Type::INT || rhs.type != ConstantValue::Type::INT) {
		return false;
	}

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
			// Instructions that read from operands (not just write)
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
			case IROpcode::BRANCH_ZERO:
			case IROpcode::BRANCH_NOT_ZERO:
			case IROpcode::VCALL:
			case IROpcode::VGET:
			case IROpcode::VSET:
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
