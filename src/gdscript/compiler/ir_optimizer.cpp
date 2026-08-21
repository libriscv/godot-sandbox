#include "ir_optimizer.h"
#include "compiler_exception.h"
#include "ir_verifier.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace gdscript {

IROptimizer::IROptimizer() {
	// GDSC_PASSES names the passes to run, comma separated, and is how the
	// optimization-invariance test bisects a miscompile from a shell loop.
	// "all" (or an unset variable) runs everything, "none" runs nothing.
	if (const char* env = std::getenv("GDSC_PASSES")) {
		std::vector<std::string> names;
		std::string current;
		for (const char* p = env; ; p++) {
			if (*p == ',' || *p == '\0') {
				if (!current.empty()) {
					names.push_back(current);
					current.clear();
				}
				if (*p == '\0') break;
			} else if (*p != ' ' && *p != '\t') {
				current.push_back(*p);
			}
		}
		if (names.empty()) {
			names.push_back("none");
		}
		set_enabled_passes(names);
	}
}

const std::vector<IRPass>& IROptimizer::pipeline() {
	// Peephole appears three times on purpose: each earlier pass exposes
	// patterns the previous peephole run could not see.
	static const std::vector<IRPass> passes = {
		{ "constant-folding", &IROptimizer::constant_folding },
		{ "copy-propagation", &IROptimizer::copy_propagation },
		{ "enhanced-copy-propagation", &IROptimizer::enhanced_copy_propagation },
		{ "licm", &IROptimizer::loop_invariant_code_motion },
		{ "peephole", &IROptimizer::peephole_optimization },
		{ "peephole", &IROptimizer::peephole_optimization },
		{ "redundant-stores", &IROptimizer::eliminate_redundant_stores },
		{ "peephole", &IROptimizer::peephole_optimization },
		{ "dead-code", &IROptimizer::eliminate_dead_code },
	};
	return passes;
}

void IROptimizer::set_enabled_passes(const std::vector<std::string>& names) {
	m_enabled_passes.clear();
	for (const auto& name : names) {
		if (name == "all") {
			// "all" is the default, expressed as "no restriction".
			m_enabled_passes.clear();
			return;
		}
		if (name == "none") {
			continue;
		}
		bool known = false;
		for (const auto& pass : pipeline()) {
			if (name == pass.name) {
				known = true;
				break;
			}
		}
		if (!known) {
			throw CompilerException(ErrorType::OPTIMIZER_ERROR,
				"Unknown optimizer pass: " + name);
		}
		m_enabled_passes.insert(name);
	}
	// An explicit selection that names no pass still has to mean "no pass", not
	// "every pass", so mark the set non-empty with a name nothing matches.
	m_enabled_passes.insert("");
}

bool IROptimizer::is_pass_enabled(const char* name) const {
	if (m_enabled_passes.empty()) {
		return true;
	}
	return m_enabled_passes.find(name) != m_enabled_passes.end();
}

void IROptimizer::optimize(IRProgram& program) {
	for (auto& func : program.functions) {
		optimize_function(func);
	}
	// The global init function is a normal IR function and has to be optimized
	// with the rest, otherwise it is the one function in the program that the
	// backend sees in unoptimized form.
	if (program.has_global_init) {
		optimize_function(program.global_init);
	}
}

void IROptimizer::optimize_function(IRFunction& func) {
	// NOTE: reduce_register_pressure() is not in the pipeline because it breaks
	// the calling convention. Parameters are in specific registers (r0-r6)
	// and return value must be in r0. We need to be more careful about
	// which registers we renumber.
	const auto& passes = pipeline();
	const size_t limit = std::min(m_pass_limit, passes.size());
	const bool verify = ir_verification_enabled();

	// The IR going in has to be well-formed before a pass can be blamed for
	// breaking it.
	if (verify) {
		ir_verify(func, "codegen");
	}

	for (size_t i = 0; i < limit; i++) {
		if (!is_pass_enabled(passes[i].name)) {
			continue;
		}
		(this->*passes[i].run)(func);
		// Verifying between passes is what turns "a pass corrupted the IR" from
		// something noticed when the generated code misbehaves, and only if a
		// test happens to exercise it, into a failure that names the pass.
		if (verify) {
			ir_verify(func, passes[i].name);
		}
	}

	// Recalculate max_registers after optimizations. This is bookkeeping rather
	// than an optimization, so it runs even when every pass is disabled.
	int max_reg = 0;
	for (const auto& instr : func.instructions) {
		for (const auto& op : instr.operands) {
			if (op.type == IRValue::Type::REGISTER) {
				int reg = std::get<int>(op.value);
				max_reg = std::max(max_reg, reg);
			}
		}
	}
	// Parameters occupy registers 0..N-1 even when no instruction mentions them:
	// the prologue copies every incoming Variant into its slot. Sizing the frame
	// from instruction operands alone puts that copy out of frame.
	func.max_registers = std::max(max_reg + 1, static_cast<int>(func.parameters.size()));

	// The recomputation is the last thing that touches the function, and it is
	// the one step no per-pass verification covers: every ir_verify() above ran
	// against the count the previous round left behind.
	if (verify) {
		ir_verify(func, "max-registers");
	}
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
			case IROpcode::MOD:
			case IROpcode::BIT_AND:
			case IROpcode::BIT_OR:
			case IROpcode::BIT_XOR:
			case IROpcode::SHL:
			case IROpcode::SHR: {
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
							new_instructions.back().type_hint = Variant::FLOAT;
						} else {
							new_instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(dst), IRValue::imm(result.int_value));
							if (instr.type_hint == Variant::INT) {
								new_instructions.back().type_hint = Variant::INT;
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
						// Negating INT64_MIN wraps, as it does on the machine.
						result.int_value = static_cast<int64_t>(0u - static_cast<uint64_t>(cv.int_value));
						new_instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(dst), IRValue::imm(result.int_value));
						set_register_constant(dst, result);
						folded = true;
					} else if (cv.type == ConstantValue::Type::FLOAT) {
						result.type = ConstantValue::Type::FLOAT;
						result.float_value = -cv.float_value;
						new_instructions.emplace_back(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(dst), IRValue::fimm(result.float_value));
						new_instructions.back().type_hint = Variant::FLOAT;
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
			case IROpcode::PRINT:
			case IROpcode::GLOBAL_CALL:
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

			// Opcodes this pass does not model. Every one of them is listed
			// rather than caught by a `default:`, so that adding an opcode is a
			// compile error here and has to be given an answer: fold it, or say
			// out loud that folding across it is not safe.
			case IROpcode::BIT_NOT:
			case IROpcode::BRANCH_EQ:
			case IROpcode::BRANCH_GT:
			case IROpcode::BRANCH_GTE:
			case IROpcode::BRANCH_LT:
			case IROpcode::BRANCH_LTE:
			case IROpcode::BRANCH_NEQ:
			case IROpcode::BRANCH_NOT_ZERO:
			case IROpcode::BRANCH_ZERO:
			case IROpcode::CONVERT:
			case IROpcode::JUMP:
			case IROpcode::LABEL:
			case IROpcode::LOAD_GLOBAL:
			case IROpcode::MAKE_ARRAY:
			case IROpcode::MAKE_COLOR:
			case IROpcode::MAKE_DICTIONARY:
			case IROpcode::MAKE_PACKED_BYTE_ARRAY:
			case IROpcode::MAKE_PACKED_COLOR_ARRAY:
			case IROpcode::MAKE_PACKED_FLOAT32_ARRAY:
			case IROpcode::MAKE_PACKED_FLOAT64_ARRAY:
			case IROpcode::MAKE_PACKED_INT32_ARRAY:
			case IROpcode::MAKE_PACKED_INT64_ARRAY:
			case IROpcode::MAKE_PACKED_STRING_ARRAY:
			case IROpcode::MAKE_PACKED_VECTOR2_ARRAY:
			case IROpcode::MAKE_PACKED_VECTOR3_ARRAY:
			case IROpcode::MAKE_PACKED_VECTOR4_ARRAY:
			case IROpcode::MAKE_PLANE:
			case IROpcode::MAKE_RECT2:
			case IROpcode::MAKE_RECT2I:
			case IROpcode::MAKE_VECTOR2:
			case IROpcode::MAKE_VECTOR2I:
			case IROpcode::MAKE_VECTOR3:
			case IROpcode::MAKE_VECTOR3I:
			case IROpcode::MAKE_VECTOR4:
			case IROpcode::MAKE_VECTOR4I:
			case IROpcode::RETURN:
			case IROpcode::STORE_GLOBAL:
			// POW and IN are host-evaluated: nothing to fold, they only
			// invalidate what they write.
			case IROpcode::POW:
			case IROpcode::IN:
			case IROpcode::TYPE_TEST:
			case IROpcode::SWITCH:
			case IROpcode::VGET_INLINE:
			case IROpcode::VSET_INLINE:
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
	bool is_float_op = (type_hint == Variant::FLOAT ||
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
				// Folding has to produce the value the machine would: a RISC-V
				// `add` wraps modulo 2^64, and computing it as signed int64_t
				// here would be undefined behaviour as well as a disagreement.
				result.int_value = static_cast<int64_t>(
					static_cast<uint64_t>(lhs.int_value) + static_cast<uint64_t>(rhs.int_value));
				return true;

			case IROpcode::SUB:
				result.type = ConstantValue::Type::INT;
				result.int_value = static_cast<int64_t>(
					static_cast<uint64_t>(lhs.int_value) - static_cast<uint64_t>(rhs.int_value));
				return true;

			case IROpcode::MUL:
				result.type = ConstantValue::Type::INT;
				result.int_value = static_cast<int64_t>(
					static_cast<uint64_t>(lhs.int_value) * static_cast<uint64_t>(rhs.int_value));
				return true;

			case IROpcode::DIV:
				// Division by zero, and INT64_MIN / -1, are left for run time
				// rather than folded to something the machine would not produce.
				if (rhs.int_value == 0) return false;
				if (rhs.int_value == -1 && lhs.int_value == INT64_MIN) return false;
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value / rhs.int_value;
				return true;

			case IROpcode::MOD:
				if (rhs.int_value == 0) return false; // Don't fold modulo by zero
				if (rhs.int_value == -1 && lhs.int_value == INT64_MIN) return false;
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value % rhs.int_value;
				return true;

			case IROpcode::BIT_AND:
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value & rhs.int_value;
				return true;

			case IROpcode::BIT_OR:
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value | rhs.int_value;
				return true;

			case IROpcode::BIT_XOR:
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value ^ rhs.int_value;
				return true;

			case IROpcode::SHL:
				// Shift counts are masked to 0-63 to match the RISC-V backend
				result.type = ConstantValue::Type::INT;
				result.int_value = static_cast<int64_t>(static_cast<uint64_t>(lhs.int_value) << (rhs.int_value & 63));
				return true;

			case IROpcode::SHR:
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value >> (rhs.int_value & 63);
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

		// Pattern 0: Fuse CMP + BRANCH -> BRANCH_CMP
		// Detect pattern: CMP_* dst, lhs, rhs; BRANCH_ZERO/NOT_ZERO dst, label
		// Transform to: BRANCH_CMP lhs, rhs, label (where CMP is the comparison type)
		// This eliminates the need to store and load the comparison result
		// XXX: This optimization is known to fail untyped recursive fibonacci(20)
		if (!skip && !handled && i + 1 < func.instructions.size()) {
			const auto& cmp_instr = func.instructions[i];
			const auto& branch_instr = func.instructions[i + 1];

			// Check if this is a comparison instruction
			bool is_cmp = (cmp_instr.opcode == IROpcode::CMP_EQ ||
			               cmp_instr.opcode == IROpcode::CMP_NEQ ||
			               cmp_instr.opcode == IROpcode::CMP_LT ||
			               cmp_instr.opcode == IROpcode::CMP_LTE ||
			               cmp_instr.opcode == IROpcode::CMP_GT ||
			               cmp_instr.opcode == IROpcode::CMP_GTE);

			// Check if next instruction branches on the comparison result
			bool is_branch_on_cmp = (branch_instr.opcode == IROpcode::BRANCH_ZERO ||
			                          branch_instr.opcode == IROpcode::BRANCH_NOT_ZERO);

			if (is_cmp && is_branch_on_cmp && cmp_instr.operands.size() >= 3 && branch_instr.operands.size() >= 2) {
				// Get comparison destination register
				int cmp_dst = std::get<int>(cmp_instr.operands[0].value);
				// Get branch condition register
				int branch_reg = std::get<int>(branch_instr.operands[0].value);

				// Fusing deletes the comparison's destination, so it has to be
				// read by nothing but the branch being fused into it. Scanning
				// only forward, and stopping at the first label, misses a read
				// on a later iteration of the loop the comparison sits in.
				bool reg_not_used_after = !is_reg_read_outside(func, cmp_dst, i, i + 1);
				if (cmp_dst == branch_reg && reg_not_used_after) {
					// Fuse the instructions
					IROpcode fused_opcode;
					bool invert = (branch_instr.opcode == IROpcode::BRANCH_ZERO);

					// Map CMP + BRANCH_ZERO to direct branch (or invert for BRANCH_NOT_ZERO)
					if (invert) {
						// BRANCH_ZERO means branch when condition is false, so invert comparison
						switch (cmp_instr.opcode) {
							case IROpcode::CMP_EQ:  fused_opcode = IROpcode::BRANCH_NEQ; break;
							case IROpcode::CMP_NEQ: fused_opcode = IROpcode::BRANCH_EQ; break;
							case IROpcode::CMP_LT:  fused_opcode = IROpcode::BRANCH_GTE; break;
							case IROpcode::CMP_LTE: fused_opcode = IROpcode::BRANCH_GT; break;
							case IROpcode::CMP_GT:  fused_opcode = IROpcode::BRANCH_LTE; break;
							case IROpcode::CMP_GTE: fused_opcode = IROpcode::BRANCH_LT; break;
							default: throw CompilerException(ErrorType::OPTIMIZER_ERROR, "Unexpected comparison opcode in peephole optimization");
						}
					} else {
						// BRANCH_NOT_ZERO means branch when condition is true, use same comparison
						switch (cmp_instr.opcode) {
							case IROpcode::CMP_EQ:  fused_opcode = IROpcode::BRANCH_EQ; break;
							case IROpcode::CMP_NEQ: fused_opcode = IROpcode::BRANCH_NEQ; break;
							case IROpcode::CMP_LT:  fused_opcode = IROpcode::BRANCH_LT; break;
							case IROpcode::CMP_LTE: fused_opcode = IROpcode::BRANCH_LTE; break;
							case IROpcode::CMP_GT:  fused_opcode = IROpcode::BRANCH_GT; break;
							case IROpcode::CMP_GTE: fused_opcode = IROpcode::BRANCH_GTE; break;
							default: throw CompilerException(ErrorType::OPTIMIZER_ERROR, "Unexpected comparison opcode in peephole optimization");
						}
					}

					// Create fused instruction: BRANCH_CMP lhs, rhs, label
					IRInstruction fused(fused_opcode);
					fused.operands.push_back(cmp_instr.operands[1]); // lhs
					fused.operands.push_back(cmp_instr.operands[2]); // rhs
					fused.operands.push_back(branch_instr.operands[1]); // label
					fused.type_hint = cmp_instr.type_hint;

					new_instructions.push_back(fused);
					i += 2; // Skip both instructions
					skip = true;
					handled = true;
				}
			}
		}

		// Pattern 1: MOVE r0, r0 -> eliminate
		if (!skip && !handled && instr.opcode == IROpcode::MOVE) {
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
				    ir_has_effect(op.opcode, IR_ARITHMETIC) &&
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
							// The rewrite deletes the definitions of tmp1, tmp2 and
							// dst, so nothing outside these four instructions may
							// read any of them.
							bool tmp1_safe = !is_reg_read_outside(func, move1_dst, i, i + 3);
							bool tmp2_safe = !is_reg_read_outside(func, move2_dst, i, i + 3);
							bool dst_safe = !is_reg_read_outside(func, op_dst, i, i + 3);

							// The rewritten instruction reads the sources at i
							// instead of at i+2, so nothing in between may have
							// changed them.
							bool sources_safe = move2_dst != move1_src &&
								op_dst != move1_src && op_dst != move2_src;

							if (tmp1_safe && tmp2_safe && dst_safe && sources_safe) {
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

				if (ir_has_effect(op.opcode, IR_ARITHMETIC) &&
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
						    !is_reg_read_outside(func, move1_dst, i, i + 2) &&
						    !is_reg_read_outside(func, op_dst, i, i + 2) &&
						    op_dst != move1_src) {

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
						    !is_reg_read_outside(func, move1_dst, i, i + 2) &&
						    !is_reg_read_outside(func, op_dst, i, i + 2) &&
						    op_dst != move1_src) {

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
				    ir_has_effect(op.opcode, IR_ARITHMETIC) &&
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
							// The rewrite deletes the definitions of tmp and dst,
							// so nothing outside these four instructions may read
							// either. The constant load survives, so load_dst may
							// still be read anywhere.
							bool tmp1_safe = !is_reg_read_outside(func, move1_dst, i, i + 3);
							bool dst_safe = !is_reg_read_outside(func, op_dst, i, i + 3);

							if (tmp1_safe && dst_safe) {
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
				// This is a redundant swap pattern - just eliminate both, which
				// is only safe when nothing else reads the temporary.
				if (move1_dst == move2_src && move1_src == move2_dst && move1_dst != move1_src &&
				    !is_reg_read_outside(func, move1_dst, i, i + 1)) {
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
			if (ir_has_effect(func.instructions[i].opcode, IR_ARITHMETIC) &&
			    func.instructions[i + 1].opcode == IROpcode::MOVE) {

				const auto& op = func.instructions[i];
				const auto& move = func.instructions[i + 1];

				if (op.operands.size() >= 1 && move.operands.size() >= 2) {
					int op_dst = std::get<int>(op.operands[0].value);
					int move_dst = std::get<int>(move.operands[0].value);
					int move_src = std::get<int>(move.operands[1].value);

					// Folding the MOVE into the OP deletes the OP's own
					// destination, so nothing else may read it.
					if (move_src == op_dst &&
					    !is_reg_read_outside(func, op_dst, i, i + 1)) {

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

		// Mark the destination register as "killed" - it's no longer a constant
		// we can propagate. CALL holds its destination in operand 1, so this has
		// to go through the operand-role table: missing the kill would let a
		// stale constant be propagated over a call result.
		const int killed = ir_destination_register(instr);
		if (killed >= 0) {
			constant_regs.erase(killed);
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
	// An instruction can go when both hold:
	//
	//   - it is pure, so deleting it cannot change anything the program
	//     observes (the metadata table answers this, not a list kept here,
	//     and it is asked about the instruction rather than the opcode: an
	//     unread randi() still has to be called), and
	//   - it defines a register nothing reads.
	//
	// That an instruction reads registers of its own is not a reason to keep
	// it. Its inputs were only live because this instruction read them, so
	// deleting it can make their definitions dead in turn -- which is why this
	// runs to a fixed point rather than once. What the rule does depend on
	// entirely is find_live_registers() accounting for every read in the
	// program, including the return value, which RETURN reads without naming.
	// A register operand missed there turns into a deleted definition and a
	// silently wrong program, so that function, not this one, is where the
	// conservatism lives.
	//
	// The bound is a backstop, not a schedule: each round deletes at least one
	// instruction or is the last, so the loop cannot run longer than the
	// function it is shrinking.
	for (size_t round = 0; round < func.instructions.size() + 1; round++) {
		const auto live_regs = find_live_registers(func);

		std::vector<IRInstruction> new_instructions;
		new_instructions.reserve(func.instructions.size());

		for (const auto& instr : func.instructions) {
			const int dst = ir_destination_register(instr);
			if (dst >= 0 && live_regs.count(dst) == 0 && ir_instruction_is_pure(instr)) {
				continue; // Dead: pure, defines a register nothing reads.
			}
			new_instructions.push_back(instr);
		}

		if (new_instructions.size() == func.instructions.size()) {
			return; // Nothing left to delete.
		}
		func.instructions = std::move(new_instructions);
	}
}

std::unordered_set<int> IROptimizer::find_live_registers(const IRFunction& func) {
	std::unordered_set<int> live;

	// Mark every register that is READ by some instruction as live.
	//
	// This must be conservative: any register operand we fail to account for
	// here can have its defining instruction deleted by eliminate_dead_code(),
	// silently miscompiling the program. Every register operand that is not the
	// instruction's destination counts as a read, with the destination located
	// through the shared operand-role table in ir.cpp.
	std::vector<int> reads;
	for (const auto& instr : func.instructions) {
		reads.clear();
		ir_collect_read_registers(instr, reads);
		live.insert(reads.begin(), reads.end());
	}

	// The return value needs no special case here: RETURN names no operand, but
	// ir_collect_read_registers() reports it as reading the return register
	// anyway, which is what keeps the definition that produced the return value
	// off the list of things to delete.

	return live;
}


bool IROptimizer::is_register_used_after(const IRFunction& func, int reg, size_t instr_idx) {
	std::vector<int> reads;
	bool crossed_control_flow = false;
	for (size_t i = instr_idx; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		// A read anywhere in the instruction keeps the register live.
		reads.clear();
		ir_collect_read_registers(instr, reads);
		for (int r : reads) {
			if (r == reg) {
				return true;
			}
		}

		// A definition that is not also a read kills liveness, but only while the
		// scan is still straight-line: past a label or a branch the definition
		// may sit on a path that is not taken, and treating it as a kill would
		// declare a still-live register dead.
		if (ir_is_control_flow(instr.opcode)) {
			crossed_control_flow = true;
			continue;
		}
		if (!crossed_control_flow && ir_destination_register(instr) == reg) {
			return false;
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

bool IROptimizer::is_reg_read_outside(const IRFunction& func, int reg, size_t first, size_t last) {
	std::vector<int> reads;
	for (size_t i = 0; i < func.instructions.size(); i++) {
		if (i >= first && i <= last) {
			continue;
		}
		reads.clear();
		ir_collect_read_registers(func.instructions[i], reads);
		for (int r : reads) {
			if (r == reg) {
				return true;
			}
		}
	}
	return false;
}

bool IROptimizer::is_reg_used_between_exclusive(const IRFunction& func, int reg, size_t start_idx, size_t end_idx, bool conservative_at_labels) {
	// Check if register is read in the instruction range (start_idx, end_idx),
	// exclusive of both boundaries.
	//
	// Which operands an instruction reads comes from the metadata table: the
	// fused branches read both of their register operands, CALL writes the
	// operand right after its name, and VSET reads the one every other opcode
	// defines. Spelling any of that out here again is how the copies drift.
	end_idx = std::min(end_idx, func.instructions.size());

	std::vector<int> reads;
	for (size_t i = start_idx + 1; i < end_idx; i++) {
		const auto& instr = func.instructions[i];

		// LABEL is a control flow boundary - be conservative (unless told otherwise)
		if (instr.opcode == IROpcode::LABEL) {
			return conservative_at_labels;
		}

		reads.clear();
		ir_collect_read_registers(instr, reads);
		for (int r : reads) {
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
	// Operand 0 is a read for the branches, RETURN, STORE_GLOBAL and VSET, and
	// is not the destination for CALL. Delaying a store past an instruction that
	// reads it would hand that instruction a stale value, so the roles come from
	// the shared table rather than from a per-pass guess.
	static thread_local std::vector<int> reads;
	reads.clear();
	ir_collect_read_registers(instr, reads);
	for (int reg : reads) {
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

		// Anything that is not pure ends the straight-line run this pass reasons
		// over: a label or a branch because the next instruction may be reached
		// from elsewhere, a call or a store because it can observe the value.
		// Asking the metadata table is what keeps this list from going stale the
		// next time an opcode is added.
		if (!ir_instruction_is_pure(instr)) {
			flush_pending(new_instructions, pending_stores, func);
			new_instructions.push_back(instr);
			continue;
		}

		// Check if this instruction reads from a register with a pending store
		if (reads_pending_store(instr, pending_stores)) {
			flush_pending(new_instructions, pending_stores, func);
		}

		// Check if this is a pure load/store operation to a register
		if (ir_has_effect(instr.opcode, IR_SIMPLE_LOAD) && !instr.operands.empty() &&
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

		// Any other instruction that defines a register with a pending store
		// makes that store dead: nothing read it (a read would have flushed
		// above), and re-emitting it at the next flush point would land after
		// this definition and clobber it.
		const int defined = ir_destination_register(instr);
		if (defined >= 0) {
			pending_stores.erase(defined);
		}

		// Not a store operation - add it directly
		new_instructions.push_back(instr);
	}

	// Flush any remaining pending stores
	flush_pending(new_instructions, pending_stores, func);

	func.instructions = std::move(new_instructions);
}

// ============================================================================
// Loop-Invariant Code Motion (LICM)
// ============================================================================

std::vector<IROptimizer::LoopInfo> IROptimizer::identify_loops(const IRFunction& func) {
	std::vector<LoopInfo> loops;

	// Map label names to instruction indices
	std::unordered_map<std::string, size_t> label_positions;
	for (size_t i = 0; i < func.instructions.size(); i++) {
		if (func.instructions[i].opcode == IROpcode::LABEL) {
			std::string label = std::get<std::string>(func.instructions[i].operands[0].value);
			label_positions[label] = i;
		}
	}

	// Find all loops by detecting back edges (JUMP to earlier label)
	// Also find loop exit labels
	std::unordered_map<std::string, std::string> loop_to_exit;  // header -> exit label

	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		// Look for JUMP instructions
		if (instr.opcode == IROpcode::JUMP && !instr.operands.empty() &&
		    instr.operands[0].type == IRValue::Type::LABEL) {
			std::string target_label = std::get<std::string>(instr.operands[0].value);

			// Check if this is a back edge (jumping to earlier instruction)
			auto it = label_positions.find(target_label);
			if (it != label_positions.end() && it->second <= i) {
				size_t header_idx = it->second;

				// Try to find the loop exit label
				// Look for branch instructions between header and back edge
				std::string exit_label;
				for (size_t j = header_idx; j < i; j++) {
					const auto& loop_instr = func.instructions[j];
					if (ir_has_effect(loop_instr.opcode, IR_BRANCH)) {
						// Find the label operand (last operand for all branch types)
						size_t label_idx = loop_instr.operands.size() - 1;
						if (label_idx < loop_instr.operands.size() &&
						    loop_instr.operands[label_idx].type == IRValue::Type::LABEL) {
							exit_label = std::get<std::string>(loop_instr.operands[label_idx].value);
							break;
						}
					}
				}

				// Find the exit label position
				size_t exit_idx = i + 1;  // Default to after back edge
				if (!exit_label.empty()) {
					auto exit_it = label_positions.find(exit_label);
					if (exit_it != label_positions.end()) {
						exit_idx = exit_it->second;
					}
				}

				// Check if we already have this loop
				bool found = false;
				for (auto& loop : loops) {
					if (loop.header_idx == header_idx) {
						loop.back_edges.push_back(i);
						found = true;
						break;
					}
				}

				if (!found) {
					LoopInfo loop;
					loop.header_idx = header_idx;
					loop.end_idx = exit_idx;
					loop.header_label = target_label;
					loop.end_label = exit_label;
					loop.back_edges.push_back(i);
					loops.push_back(loop);
				}
			}
		}
	}

	return loops;
}

bool IROptimizer::is_loop_invariant(const IRInstruction& instr, const LoopInfo& loop,
                                    const IRFunction& func, const std::unordered_set<int>& invariant_regs) {
	// Only consider pure operations
	if (!ir_has_effect(instr.opcode, IR_SIMPLE_LOAD)) {
		return false;
	}

	// LOAD_IMM, LOAD_FLOAT_IMM, LOAD_BOOL are always invariant (no register operands)
	if (instr.opcode == IROpcode::LOAD_IMM ||
	    instr.opcode == IROpcode::LOAD_FLOAT_IMM ||
	    instr.opcode == IROpcode::LOAD_BOOL ||
	    instr.opcode == IROpcode::LOAD_STRING) {
		return true;
	}

	// MOVE is invariant if source register is invariant
	if (instr.opcode == IROpcode::MOVE && instr.operands.size() >= 2 &&
	    instr.operands[1].type == IRValue::Type::REGISTER) {
		int src_reg = std::get<int>(instr.operands[1].value);
		return invariant_regs.count(src_reg) > 0;
	}

	return false;
}

bool IROptimizer::can_safely_hoist(const IRInstruction& instr, size_t instr_idx, const LoopInfo& loop, const IRFunction& func) {
	// Hoisting an instruction out of a loop moves it from "executed on every
	// iteration, in this position" to "executed once, before the loop". That is
	// only the same program when all of the following hold:
	//
	//   1. The instruction writes a register at all.
	//   2. Its inputs are not modified anywhere in the loop.
	//   3. Its destination is not read in the loop before it is written.
	//   4. Its destination is written nowhere else in the loop.
	//   5. It is executed on every iteration -- not inside an `if` within the
	//      loop body.
	//
	// The last two are what a fuzzer found missing. A loop containing
	//
	//     if c:  x = 1
	//     else:  x = 0
	//
	// has two invariant definitions of the same register on different paths;
	// hoisting either one makes the register hold that value on both paths, and
	// hoisting both makes it hold whichever was hoisted second. The program
	// then computes something else entirely, with nothing to see but a wrong
	// answer at run time.

	const int dst_reg = ir_destination_register(instr);
	if (dst_reg < 0) {
		return false;
	}

	// Collect source registers from this instruction
	std::vector<int> reads;
	ir_collect_read_registers(instr, reads);
	std::unordered_set<int> src_regs(reads.begin(), reads.end());

	// Check the entire loop body (from header to all back edges)
	for (size_t i = loop.header_idx; i < loop.end_idx && i < func.instructions.size(); i++) {
		const auto& loop_instr = func.instructions[i];

		// Skip the instruction we're trying to hoist
		if (i == instr_idx) {
			continue;
		}

		// Check if any source register is modified in the loop. A CALL writes
		// the operand after its name, not operand 0, so looking at operand 0
		// here would let an instruction be hoisted over a call that redefines
		// one of its inputs.
		const int modified_reg = ir_destination_register(loop_instr);
		if (modified_reg >= 0 && src_regs.count(modified_reg)) {
			return false;  // Can't hoist - source register is modified in loop
		}

		// A second definition of the destination inside the loop means the
		// register does not hold one value per iteration, and hoisting one of
		// the definitions decides which value it holds for good.
		if (modified_reg == dst_reg) {
			return false;
		}

		// Check if dst_reg is read before being written (only before this instruction)
		if (i < instr_idx) {
			reads.clear();
			ir_collect_read_registers(loop_instr, reads);
			for (int reg : reads) {
				if (reg == dst_reg) {
					return false;  // Can't hoist - dst is read before being written
				}
			}
		}
	}

	return is_unconditional_in_loop(instr_idx, loop, func);
}

bool IROptimizer::is_unconditional_in_loop(size_t instr_idx, const LoopInfo& loop, const IRFunction& func) {
	// Whether every iteration that enters the loop body reaches this
	// instruction. Walking from the header, anything that can route control
	// past the instruction makes it conditional:
	//
	//   - a LABEL, which is a join and therefore the end of a region something
	//     jumped over,
	//   - an unconditional JUMP, which skips whatever follows it,
	//   - a conditional branch to a label inside the loop, which is an `if`
	//     within the body. A branch out of the loop is not one of these: taking
	//     it ends the iteration rather than skipping part of it.
	std::unordered_map<std::string, size_t> label_positions;
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& candidate = func.instructions[i];
		if (ir_has_effect(candidate.opcode, IR_LABEL) && !candidate.operands.empty()) {
			label_positions[std::get<std::string>(candidate.operands[0].value)] = i;
		}
	}

	for (size_t i = loop.header_idx + 1; i < instr_idx && i < func.instructions.size(); i++) {
		const auto& between = func.instructions[i];

		if (ir_has_effect(between.opcode, IR_LABEL)) {
			return false;
		}
		if (ir_has_effect(between.opcode, IR_TERMINATOR)) {
			return false;
		}
		if (!ir_has_effect(between.opcode, IR_BRANCH)) {
			continue;
		}

		// The branch target is the last operand of every branch shape.
		if (between.operands.empty()) {
			return false;
		}
		const IRValue& target = between.operands.back();
		if (target.type != IRValue::Type::LABEL) {
			return false;
		}
		auto it = label_positions.find(std::get<std::string>(target.value));
		if (it == label_positions.end()) {
			return false;
		}
		const size_t target_idx = it->second;
		const bool leaves_the_loop = target_idx < loop.header_idx || target_idx >= loop.end_idx;
		if (!leaves_the_loop) {
			return false;
		}
	}

	return true;
}

void IROptimizer::loop_invariant_code_motion(IRFunction& func) {
	auto loops = identify_loops(func);

	if (loops.empty()) {
		return;  // No loops to optimize
	}

	// Check for nested loops - if any loop is nested, skip LICM entirely
	// A loop is nested if its header is between another loop's header and end
	for (const auto& loop : loops) {
		for (const auto& other : loops) {
			if (&loop == &other) continue;
			// Check if this loop is inside the other loop
			if (loop.header_idx > other.header_idx && loop.header_idx < other.end_idx) {
				// Found nested loops - skip LICM to avoid complexity
				return;
			}
		}
	}

	// No nested loops - safe to optimize
	// Process each loop
	for (const auto& loop : loops) {
			// Build set of invariant registers
			// Start with registers defined outside the loop
			std::unordered_set<int> invariant_regs;

			for (size_t i = 0; i < loop.header_idx; i++) {
				const auto& instr = func.instructions[i];
				if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
					invariant_regs.insert(std::get<int>(instr.operands[0].value));
				}
			}

			// Iteratively find invariant instructions within the loop
			std::unordered_set<size_t> invariant_instrs;
			bool loop_changed = true;

			while (loop_changed) {
				loop_changed = false;

				for (size_t i = loop.header_idx + 1; i < loop.end_idx && i < func.instructions.size(); i++) {
					if (invariant_instrs.count(i)) {
						continue;  // Already marked as invariant
					}

					const auto& instr = func.instructions[i];

					// Skip labels and control flow
					if (ir_is_control_flow(instr.opcode)) {
						continue;
					}

					if (is_loop_invariant(instr, loop, func, invariant_regs) &&
					    can_safely_hoist(instr, i, loop, func)) {
						invariant_instrs.insert(i);

						// Add destination register to invariant set
						if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
							invariant_regs.insert(std::get<int>(instr.operands[0].value));
						}

						loop_changed = true;
					}
				}
			}

		// Hoist invariant instructions
		if (!invariant_instrs.empty()) {
			std::vector<IRInstruction> hoisted;
			std::vector<IRInstruction> remaining;

			for (size_t i = 0; i < func.instructions.size(); i++) {
				if (i >= loop.header_idx && i < loop.end_idx && invariant_instrs.count(i)) {
					// This instruction will be hoisted
					hoisted.push_back(func.instructions[i]);
				} else {
					remaining.push_back(func.instructions[i]);
				}
			}

			// Rebuild function with hoisted instructions inserted before loop header
			std::vector<IRInstruction> new_instructions;
			for (size_t i = 0; i < remaining.size(); i++) {
				if (remaining[i].opcode == IROpcode::LABEL && i < remaining.size() &&
				    std::get<std::string>(remaining[i].operands[0].value) == loop.header_label) {
					// Insert hoisted instructions before loop header
					new_instructions.insert(new_instructions.end(), hoisted.begin(), hoisted.end());
				}
				new_instructions.push_back(remaining[i]);
			}

			func.instructions = std::move(new_instructions);
		}
	}
}

// ============================================================================
// Enhanced Copy Propagation
// ============================================================================

bool IROptimizer::register_unmodified_between(const IRFunction& func, int reg,
                                              size_t start_idx, size_t end_idx) {
	for (size_t i = start_idx + 1; i < end_idx && i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		// Check if this instruction writes to the register
		if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
			int dst = std::get<int>(instr.operands[0].value);
			if (dst == reg) {
				return false;  // Register is modified
			}
		}

		// Control flow boundaries - be conservative
		if (instr.opcode == IROpcode::LABEL) {
			return false;
		}
	}

	return true;
}

void IROptimizer::enhanced_copy_propagation(IRFunction& func) {
	// Track copies: register -> source register
	std::unordered_map<int, CopyInfo> copies;
	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());

	for (size_t i = 0; i < func.instructions.size(); i++) {
		auto instr = func.instructions[i];  // Make a copy we can modify

		// Clear copy tracking at anything that is not pure: a control flow join
		// invalidates the straight-line reasoning, and a call or a store can
		// change what a register holds without a visible definition.
		//
		// Deferred until after this instruction's operands are rewritten below:
		// an instruction reads its operands before it has its effect. Clearing
		// first meant no impure instruction ever saw a copy, and every branch is
		// impure, so `MOVE rb, ra; BRANCH_EQ rb, ...` reached the backend as a
		// full 24-byte Variant copy the branch then read 8 bytes of.
		const bool clear_after = !ir_instruction_is_pure(instr);

		// Propagate copies into every operand the instruction reads. Asking the
		// operand-role table rather than starting at operand 1 is what keeps the
		// branch operands (which are read at operand 0) and the CALL result
		// (which is written at operand 1) on the right sides of the line.
		for (size_t j = 0; j < instr.operands.size(); j++) {
			if (!ir_reads_operand(instr, j)) {
				continue;
			}
			int reg = std::get<int>(instr.operands[j].value);

			// Check if we can propagate this copy
			if (copies.count(reg)) {
				const auto& copy_info = copies[reg];

				// Verify the source register hasn't been modified
				if (register_unmodified_between(func, copy_info.source_reg, copy_info.def_idx, i)) {
					instr.operands[j].value = copy_info.source_reg;
				}
			}
		}

		if (clear_after) {
			copies.clear();
		}

		// Invalidate copies when the destination register is written
		const int written = ir_destination_register(instr);
		if (written >= 0) {
			// Remove any copies of this register
			copies.erase(written);

			// Remove any copies that use this register as source
			for (auto it = copies.begin(); it != copies.end(); ) {
				if (it->second.source_reg == written) {
					it = copies.erase(it);
				} else {
					++it;
				}
			}
		}

		// Record this MOVE as a copy, after the invalidation above and not
		// before it: a MOVE writes the register it is recorded under, so
		// tracking it first meant the invalidation immediately erased what had
		// just been learned. Nothing downstream ever saw a copy, and a chain
		// `MOVE rb, ra; MOVE rc, rb` reached the backend as two Variant copies
		// where one would do. The source is read from the propagated operand,
		// so a chain collapses onto its original source in one pass.
		if (instr.opcode == IROpcode::MOVE && instr.operands.size() >= 2 &&
		    instr.operands[0].type == IRValue::Type::REGISTER &&
		    instr.operands[1].type == IRValue::Type::REGISTER) {
			const int dst = std::get<int>(instr.operands[0].value);
			const int src = std::get<int>(instr.operands[1].value);
			if (dst != src) {
				copies[dst] = {src, i};
			}
		}

		new_instructions.push_back(instr);
	}

	func.instructions = std::move(new_instructions);
}

} // namespace gdscript
