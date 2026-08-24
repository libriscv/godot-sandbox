#include "ir_optimizer.h"
#include "compiler_exception.h"
#include "ir_verifier.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace gdscript {

IROptimizer::IROptimizer() {
	// GDSC_PASSES: comma-separated pass names; "all"/unset = everything, "none" = nothing.
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
	// Peephole appears three times: each earlier pass exposes new patterns.
	static const std::vector<IRPass> passes = {
		{ "constant-folding", &IROptimizer::constant_folding },
		// Folding known branches strands arms; unreachable-code removes them.
		{ "unreachable-code", &IROptimizer::eliminate_unreachable_code },
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
	// Non-empty with an unmatchable name: "no pass", not "every pass".
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
	// global_init is a normal IR function; the backend needs it optimized too.
	if (program.has_member_init) {
		optimize_function(program.member_init);
	}
	if (program.has_global_init) {
		optimize_function(program.global_init);
	}
}

void IROptimizer::optimize_function(IRFunction& func) {
	// reduce_register_pressure() excluded: it renumbers r0-r6, breaking the ABI.
	const auto& passes = pipeline();
	const size_t limit = std::min(m_pass_limit, passes.size());
	const bool verify = ir_verification_enabled();

	// Verify pre-pass IR so a corruption can be blamed on the right pass.
	if (verify) {
		ir_verify(func, "codegen");
	}

	for (size_t i = 0; i < limit; i++) {
		if (!is_pass_enabled(passes[i].name)) {
			continue;
		}
		(this->*passes[i].run)(func);
		if (verify) {
			ir_verify(func, passes[i].name);
		}
	}

	// Bookkeeping, not an optimization: runs even with all passes disabled.
	int max_reg = 0;
	for (const auto& instr : func.instructions) {
		for (const auto& op : instr.operands) {
			if (op.type == IRValue::Type::REGISTER) {
				int reg = std::get<int>(op.value);
				max_reg = std::max(max_reg, reg);
			}
		}
	}
	// Parameters r0..N-1 occupy frame slots even when no instruction names them.
	func.max_registers = std::max(max_reg + 1, static_cast<int>(func.parameters.size()));

	// Verify after max_registers recomputation; per-pass checks ran against the old count.
	if (verify) {
		ir_verify(func, "max-registers");
	}
}

bool IROptimizer::ConstantValue::same_as(const ConstantValue& other) const {
	if (type != other.type) {
		return false;
	}
	switch (type) {
		case Type::NONE:   return true;
		case Type::INT:    return int_value == other.int_value;
		case Type::BOOL:   return bool_value == other.bool_value;
		case Type::STRING: return string_value == other.string_value;
		case Type::FLOAT:
			// Bitwise: NaN == NaN, -0.0 != 0.0. Prevents misfolding at joins.
			return std::memcmp(&float_value, &other.float_value, sizeof(double)) == 0;
	}
	return false;
}

bool IROptimizer::ConstantValue::truthiness(bool& truth) const {
	switch (type) {
		case Type::INT:
			truth = (int_value != 0);
			return true;
		case Type::BOOL:
			truth = bool_value;
			return true;
		case Type::FLOAT:
			// Matches Variant::booleanize(): -0.0 is false, NaN is true.
			truth = (float_value != 0.0);
			return true;
		case Type::NONE:
		case Type::STRING:
			return false;
	}
	return false;
}

std::vector<IROptimizer::ConstBlock> IROptimizer::build_blocks(const IRFunction& func) {
	std::vector<ConstBlock> blocks;
	const size_t count = func.instructions.size();
	if (count == 0) {
		return blocks;
	}

	std::unordered_map<std::string, size_t> label_index;
	for (size_t i = 0; i < count; i++) {
		const auto& instr = func.instructions[i];
		if (ir_has_effect(instr.opcode, IR_LABEL) && !instr.operands.empty()) {
			label_index.emplace(std::get<std::string>(instr.operands[0].value), i);
		}
	}

	std::vector<bool> is_leader(count, false);
	is_leader[0] = true;
	for (size_t i = 0; i < count; i++) {
		const IROpcode op = func.instructions[i].opcode;
		if (ir_has_effect(op, IR_LABEL)) {
			is_leader[i] = true;
		}
		if ((ir_has_effect(op, IR_BRANCH) || ir_has_effect(op, IR_TERMINATOR)) && i + 1 < count) {
			is_leader[i + 1] = true;
		}
	}

	std::vector<size_t> block_of(count, 0);
	for (size_t i = 0; i < count; i++) {
		if (is_leader[i]) {
			ConstBlock block;
			block.begin = i;
			blocks.push_back(block);
		}
		block_of[i] = blocks.size() - 1;
		blocks.back().end = i + 1;
	}

	for (size_t b = 0; b < blocks.size(); b++) {
		const IRInstruction& last = func.instructions[blocks[b].end - 1];
		// A branch target is a successor -- but a LABEL's own operand names
		// itself, not somewhere control goes.
		if (!ir_has_effect(last.opcode, IR_LABEL)) {
			for (const auto& operand : last.operands) {
				if (operand.type != IRValue::Type::LABEL) {
					continue;
				}
				auto it = label_index.find(std::get<std::string>(operand.value));
				if (it != label_index.end()) {
					blocks[b].successors.push_back(block_of[it->second]);
				}
			}
		}
		if (!ir_has_effect(last.opcode, IR_TERMINATOR) && b + 1 < blocks.size()) {
			blocks[b].successors.push_back(b + 1);
		}
	}
	return blocks;
}

bool IROptimizer::meet_constants(ConstantMap& into, const ConstantMap& from) {
	bool changed = false;
	for (auto it = into.begin(); it != into.end(); ) {
		auto other = from.find(it->first);
		if (other == from.end() || !it->second.same_as(other->second)) {
			it = into.erase(it);
			changed = true;
		} else {
			++it;
		}
	}
	return changed;
}

void IROptimizer::constant_folding(IRFunction& func) {
	std::vector<ConstBlock> blocks = build_blocks(func);
	if (blocks.empty()) {
		return;
	}

	// Forward dataflow to fixpoint; monotone by construction (entries only shrink).
	blocks[0].entry_initialized = true;

	// Cap: non-monotone transfer degrades to label-clearing instead of hanging.
	const size_t max_visits = blocks.size() * 16 + 1024;
	size_t visits = 0;
	bool converged = true;

	std::vector<size_t> worklist { 0 };
	while (!worklist.empty()) {
		if (++visits > max_visits) {
			converged = false;
			break;
		}
		const size_t index = worklist.back();
		worklist.pop_back();

		m_constants = blocks[index].entry;
		for (size_t i = blocks[index].begin; i < blocks[index].end; i++) {
			fold_instruction(func.instructions[i], nullptr);
		}

		for (size_t successor : blocks[index].successors) {
			ConstBlock& target = blocks[successor];
			bool changed = false;
			if (!target.entry_initialized) {
				target.entry = m_constants;
				target.entry_initialized = true;
				changed = true;
			} else {
				changed = meet_constants(target.entry, m_constants);
			}
			if (changed) {
				worklist.push_back(successor);
			}
		}
	}

	if (!converged) {
		for (size_t b = 1; b < blocks.size(); b++) {
			blocks[b].entry.clear();
		}
	}

	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());
	for (const auto& block : blocks) {
		// Unreachable blocks: no state, leave for eliminate_unreachable_code().
		m_constants = block.entry_initialized ? block.entry : ConstantMap {};
		for (size_t i = block.begin; i < block.end; i++) {
			fold_instruction(func.instructions[i], &new_instructions);
		}
	}
	func.instructions = std::move(new_instructions);
}

void IROptimizer::fold_instruction(const IRInstruction& instr, std::vector<IRInstruction>* out) {
	const auto emit = [out](const IRInstruction& produced) {
		if (out != nullptr) {
			out->push_back(produced);
		}
	};

	bool folded = false;

	switch (instr.opcode) {
		// Join point; entry state computed by constant_folding(), not changed here.
		case IROpcode::LABEL:
		case IROpcode::BREAKPOINT:
			emit(instr);
			break;

		case IROpcode::LOAD_IMM: {
			int reg = std::get<int>(instr.operands[0].value);
			int64_t val = std::get<int64_t>(instr.operands[1].value);
			ConstantValue cv;
			cv.type = ConstantValue::Type::INT;
			cv.int_value = val;
			set_register_constant(reg, cv);
			emit(instr);
			break;
		}

		case IROpcode::LOAD_FLOAT_IMM: {
			int reg = std::get<int>(instr.operands[0].value);
			double val = std::get<double>(instr.operands[1].value);
			ConstantValue cv;
			cv.type = ConstantValue::Type::FLOAT;
			cv.float_value = val;
			set_register_constant(reg, cv);
			emit(instr);
			break;
		}

		case IROpcode::LOAD_BOOL: {
			int reg = std::get<int>(instr.operands[0].value);
			int64_t val = std::get<int64_t>(instr.operands[1].value);
			ConstantValue cv;
			cv.type = ConstantValue::Type::BOOL;
			cv.bool_value = (val != 0);
			set_register_constant(reg, cv);
			emit(instr);
			break;
		}

		case IROpcode::LOAD_STRING:
		case IROpcode::LOAD_STRING_AS: {
			int reg = std::get<int>(instr.operands[0].value);
			invalidate_register(reg);
			emit(instr);
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
			emit(instr);
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
				if (instr.operands.size() < 3 ||
			    instr.operands[0].type != IRValue::Type::REGISTER ||
			    instr.operands[1].type != IRValue::Type::REGISTER ||
			    instr.operands[2].type != IRValue::Type::REGISTER) {
				if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
					int dst = std::get<int>(instr.operands[0].value);
					invalidate_register(dst);
				}
				emit(instr);
				break;
			}

			int dst = std::get<int>(instr.operands[0].value);
			int lhs_reg = std::get<int>(instr.operands[1].value);
			int rhs_reg = std::get<int>(instr.operands[2].value);

			if (m_constants.count(lhs_reg) && m_constants.count(rhs_reg)) {
				ConstantValue result;
				if (try_fold_binary_op(instr.opcode, instr.type_hint, m_constants[lhs_reg], m_constants[rhs_reg], result)) {
					if (result.type == ConstantValue::Type::FLOAT) {
						IRInstruction load(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(dst), IRValue::fimm(result.float_value));
						load.type_hint = Variant::FLOAT;
						emit(load);
					} else {
						// Explicit INT hint: untyped LOAD_IMM reads as unknown to later passes.
						IRInstruction load(IROpcode::LOAD_IMM, IRValue::reg(dst), IRValue::imm(result.int_value));
						load.type_hint = Variant::INT;
						emit(load);
					}
					set_register_constant(dst, result);
					folded = true;
				}
			}

			if (!folded) {
				invalidate_register(dst);
				emit(instr);
			}
			break;
		}

		case IROpcode::CMP_EQ:
		case IROpcode::CMP_NEQ:
		case IROpcode::CMP_LT:
		case IROpcode::CMP_LTE:
		case IROpcode::CMP_GT:
		case IROpcode::CMP_GTE: {
			if (instr.operands.size() < 3 ||
			    instr.operands[0].type != IRValue::Type::REGISTER ||
			    instr.operands[1].type != IRValue::Type::REGISTER ||
			    instr.operands[2].type != IRValue::Type::REGISTER) {
				if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
					int dst = std::get<int>(instr.operands[0].value);
					invalidate_register(dst);
				}
				emit(instr);
				break;
			}

			int dst = std::get<int>(instr.operands[0].value);
			int lhs_reg = std::get<int>(instr.operands[1].value);
			int rhs_reg = std::get<int>(instr.operands[2].value);

			if (m_constants.count(lhs_reg) && m_constants.count(rhs_reg)) {
				ConstantValue result;
				if (try_fold_binary_op(instr.opcode, instr.type_hint, m_constants[lhs_reg], m_constants[rhs_reg], result)) {
					emit(IRInstruction(IROpcode::LOAD_BOOL, IRValue::reg(dst), IRValue::imm(result.bool_value ? 1 : 0)));
					set_register_constant(dst, result);
					folded = true;
				}
			}

			if (!folded) {
				invalidate_register(dst);
				emit(instr);
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
					// Wrapping negate, matches RISC-V `neg`.
					result.int_value = static_cast<int64_t>(0u - static_cast<uint64_t>(cv.int_value));
					IRInstruction load(IROpcode::LOAD_IMM, IRValue::reg(dst), IRValue::imm(result.int_value));
					load.type_hint = Variant::INT;
					emit(load);
					set_register_constant(dst, result);
					folded = true;
				} else if (cv.type == ConstantValue::Type::FLOAT) {
					result.type = ConstantValue::Type::FLOAT;
					result.float_value = -cv.float_value;
					IRInstruction load(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(dst), IRValue::fimm(result.float_value));
					load.type_hint = Variant::FLOAT;
					emit(load);
					set_register_constant(dst, result);
					folded = true;
				}
			}

			if (!folded) {
				invalidate_register(dst);
				emit(instr);
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
				emit(IRInstruction(IROpcode::LOAD_BOOL, IRValue::reg(dst), IRValue::imm(result.bool_value ? 1 : 0)));
				set_register_constant(dst, result);
				folded = true;
			}

			if (!folded) {
				invalidate_register(dst);
				emit(instr);
			}
			break;
		}

		case IROpcode::AND:
		case IROpcode::OR: {
			if (instr.operands.size() < 3 ||
			    instr.operands[0].type != IRValue::Type::REGISTER ||
			    instr.operands[1].type != IRValue::Type::REGISTER ||
			    instr.operands[2].type != IRValue::Type::REGISTER) {
				if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
					int dst = std::get<int>(instr.operands[0].value);
					invalidate_register(dst);
				}
				emit(instr);
				break;
			}

			int dst = std::get<int>(instr.operands[0].value);
			int lhs_reg = std::get<int>(instr.operands[1].value);
			int rhs_reg = std::get<int>(instr.operands[2].value);

			if (m_constants.count(lhs_reg) && m_constants.count(rhs_reg)) {
				const auto& lhs_cv = m_constants[lhs_reg];
				const auto& rhs_cv = m_constants[rhs_reg];

				if (lhs_cv.type == ConstantValue::Type::BOOL && rhs_cv.type == ConstantValue::Type::BOOL) {
					ConstantValue result;
					result.type = ConstantValue::Type::BOOL;

					if (instr.opcode == IROpcode::AND) {
						result.bool_value = lhs_cv.bool_value && rhs_cv.bool_value;
					} else {
						result.bool_value = lhs_cv.bool_value || rhs_cv.bool_value;
					}

					emit(IRInstruction(IROpcode::LOAD_BOOL, IRValue::reg(dst), IRValue::imm(result.bool_value ? 1 : 0)));
					set_register_constant(dst, result);
					folded = true;
				}
			}

			if (!folded) {
				invalidate_register(dst);
				emit(instr);
			}
			break;
		}

		// Known condition: replace with JUMP (taken) or delete (not taken).
		// Branches define nothing, so the tested register stays valid.
		case IROpcode::BRANCH_ZERO:
		case IROpcode::BRANCH_NOT_ZERO: {
			if (instr.operands.size() >= 2 && instr.operands[0].type == IRValue::Type::REGISTER) {
				const int cond = std::get<int>(instr.operands[0].value);
				auto it = m_constants.find(cond);
				bool truth = false;
				if (it != m_constants.end() && it->second.truthiness(truth)) {
					const bool taken = (instr.opcode == IROpcode::BRANCH_ZERO) ? !truth : truth;
					if (taken) {
						emit(IRInstruction(IROpcode::JUMP, instr.operands[1]));
					}
					folded = true;
				}
			}
			if (!folded) {
				emit(instr);
			}
			break;
		}

		// Fused branches: created by peephole after this pass. Define nothing.
		case IROpcode::BRANCH_EQ:
		case IROpcode::BRANCH_GT:
		case IROpcode::BRANCH_GTE:
		case IROpcode::BRANCH_LT:
		case IROpcode::BRANCH_LTE:
		case IROpcode::BRANCH_NEQ:
			emit(instr);
			break;

		// Element writes: operand 0 is the container (not a destination), keep its constant.
		case IROpcode::ARRAY_SET:
		case IROpcode::DICT_SET:
			emit(instr);
			break;

		// Invalidate destination only; input constants survive.
		case IROpcode::ARRAY_APPEND:
		case IROpcode::ARRAY_GET:
		case IROpcode::PRINT:
		case IROpcode::GLOBAL_CALL:
		case IROpcode::VCALL:
		case IROpcode::VGET:
		case IROpcode::VSET:
		case IROpcode::CALL_SYSCALL:
		case IROpcode::GET_NODE:
		case IROpcode::LOAD_RESOURCE:
		case IROpcode::LOAD_RESOURCE_VAR:
		case IROpcode::MAKE_CALLABLE:
		case IROpcode::AWAIT: // Host-provided result; not foldable, not a block boundary.

		case IROpcode::CALL:
		case IROpcode::CALL_HOSTED:
			if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
				invalidate_register(std::get<int>(instr.operands[0].value));
			}
			emit(instr);
			break;

		// No default: new opcodes must be listed explicitly (compile error otherwise).
		case IROpcode::BIT_NOT:
		case IROpcode::CONVERT:
		case IROpcode::JUMP:
		case IROpcode::LOAD_NIL:
		case IROpcode::THROW:
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
		// POW and IN: host-evaluated via ECALL_VEVAL, nothing to fold.
		case IROpcode::POW:
		case IROpcode::IN:
		case IROpcode::TYPE_TEST:
		case IROpcode::TYPE_OF:
		case IROpcode::SWITCH:
		case IROpcode::VGET_INLINE:
		case IROpcode::VSET_INLINE:
			for (const auto& op : instr.operands) {
				if (op.type == IRValue::Type::REGISTER) {
					int reg = std::get<int>(op.value);
					invalidate_register(reg);
				}
			}
			emit(instr);
			break;
	}
}

// CFG reachability, not linear scan: folded branches strand labelled blocks.
// Distinct from DCE, which removes instructions defining unread registers.
void IROptimizer::eliminate_unreachable_code(IRFunction& func) {
	std::vector<ConstBlock> blocks = build_blocks(func);
	if (blocks.empty()) {
		return;
	}

	std::vector<bool> reachable(blocks.size(), false);
	std::vector<size_t> worklist { 0 };
	reachable[0] = true;
	while (!worklist.empty()) {
		const size_t index = worklist.back();
		worklist.pop_back();
		for (size_t successor : blocks[index].successors) {
			if (!reachable[successor]) {
				reachable[successor] = true;
				worklist.push_back(successor);
			}
		}
	}

	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());
	for (size_t b = 0; b < blocks.size(); b++) {
		if (!reachable[b]) {
			continue;
		}
		for (size_t i = blocks[b].begin; i < blocks[b].end; i++) {
			new_instructions.push_back(func.instructions[i]);
		}
	}
	func.instructions = std::move(new_instructions);
}

bool IROptimizer::try_fold_binary_op(IROpcode op, IRInstruction::TypeHint type_hint, const ConstantValue& lhs, const ConstantValue& rhs, ConstantValue& result) {
	// GDScript: any float operand or hint promotes to float arithmetic.
	bool is_float_op = (type_hint == Variant::FLOAT ||
	                    lhs.type == ConstantValue::Type::FLOAT ||
	                    rhs.type == ConstantValue::Type::FLOAT);

	if (is_float_op) {
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
				if (rhs_val == 0.0) return false;
				result.type = ConstantValue::Type::FLOAT;
				result.float_value = lhs_val / rhs_val;
				return true;

			case IROpcode::MOD:
				if (rhs_val == 0.0) return false;
				result.type = ConstantValue::Type::FLOAT;
				result.float_value = std::fmod(lhs_val, rhs_val);
				return true;

			default:
				break;
		}
	}

	if (!is_float_op && lhs.type == ConstantValue::Type::INT && rhs.type == ConstantValue::Type::INT) {
		switch (op) {
			case IROpcode::ADD:
				result.type = ConstantValue::Type::INT;
				// Wrapping arithmetic via unsigned cast: matches RISC-V `add`, avoids UB.
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
				// Division by zero and INT64_MIN / -1: leave for run time.
				if (rhs.int_value == 0) return false;
				if (rhs.int_value == -1 && lhs.int_value == INT64_MIN) return false;
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value / rhs.int_value;
				return true;

			case IROpcode::MOD:
				if (rhs.int_value == 0) return false;
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
				// Shift count masked to 0-63, matching RISC-V `sll`.
				result.type = ConstantValue::Type::INT;
				result.int_value = static_cast<int64_t>(static_cast<uint64_t>(lhs.int_value) << (rhs.int_value & 63));
				return true;

			case IROpcode::SHR:
				result.type = ConstantValue::Type::INT;
				result.int_value = lhs.int_value >> (rhs.int_value & 63);
				return true;

			default:
				break;
		}
	}

	bool comparable = (lhs.type == ConstantValue::Type::INT && rhs.type == ConstantValue::Type::INT) ||
	                  (lhs.type == ConstantValue::Type::FLOAT && rhs.type == ConstantValue::Type::FLOAT) ||
	                  (lhs.type == ConstantValue::Type::INT && rhs.type == ConstantValue::Type::FLOAT) ||
	                  (lhs.type == ConstantValue::Type::FLOAT && rhs.type == ConstantValue::Type::INT);

	if (comparable) {
		bool lhs_is_float = (lhs.type == ConstantValue::Type::FLOAT);
		bool rhs_is_float = (rhs.type == ConstantValue::Type::FLOAT);

		if (lhs_is_float || rhs_is_float) {
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

// CMP_* + BRANCH_ZERO/NOT_ZERO -> fused BRANCH_*.
// XXX: known to fail untyped recursive fibonacci(20)
bool IROptimizer::try_fuse_compare_and_branch(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions) {
	if (!(i + 1 < func.instructions.size())) {
		return false;
	}
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
		int cmp_dst = std::get<int>(cmp_instr.operands[0].value);
		int branch_reg = std::get<int>(branch_instr.operands[0].value);

		// cmp_dst must be dead outside the pair (whole-function check, not just forward).
		bool reg_not_used_after = !is_reg_read_outside(func, cmp_dst, i, i + 1);
		if (cmp_dst == branch_reg && reg_not_used_after) {
			// Fuse the instructions
			IROpcode fused_opcode;
			bool invert = (branch_instr.opcode == IROpcode::BRANCH_ZERO);

			if (invert) {
				// BRANCH_ZERO: branch when false, so invert comparison.
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

			IRInstruction fused(fused_opcode);
			fused.operands.push_back(cmp_instr.operands[1]); // lhs
			fused.operands.push_back(cmp_instr.operands[2]); // rhs
			fused.operands.push_back(branch_instr.operands[1]); // label
			fused.type_hint = cmp_instr.type_hint;

			new_instructions.push_back(fused);
			i += 2;
			return true; // Skip both instructions
		}
	}
	return false;
}

// Remove JUMP/branch whose target is the immediately following instruction
// (skipping labels). Common after `match` tail and unreachable-code removal.
bool IROptimizer::try_remove_branch_to_next(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions) {
	(void) new_instructions;   // the replacement is nothing at all
	const auto& instr = func.instructions[i];

	if (instr.opcode != IROpcode::JUMP && !ir_has_effect(instr.opcode, IR_BRANCH)) {
		return false;
	}
	// SWITCH fall-through is out-of-range behaviour, not redundancy.
	if (instr.opcode == IROpcode::SWITCH) {
		return false;
	}

	const IRValue* target = nullptr;
	for (const auto& operand : instr.operands) {
		if (operand.type == IRValue::Type::LABEL) {
			target = &operand;
		}
	}
	if (target == nullptr) {
		return false;
	}
	const std::string& name = std::get<std::string>(target->value);

	for (size_t j = i + 1; j < func.instructions.size(); j++) {
		const auto& next = func.instructions[j];
		if (!ir_has_effect(next.opcode, IR_LABEL)) {
			return false;
		}
		if (!next.operands.empty() && std::get<std::string>(next.operands[0].value) == name) {
			i++;
			return true;
		}
	}
	return false;
}

bool IROptimizer::try_eliminate_moves_around_op(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions) {
	const auto& instr = func.instructions[i];
	if (instr.opcode != IROpcode::MOVE) {
		return false;
	}
	int dst = std::get<int>(instr.operands[0].value);
	int src = std::get<int>(instr.operands[1].value);
	if (dst == src) {
		i++;
		return true;
	}

	// Pattern A: MOVE;MOVE;OP;MOVE -> OP with sources/dest substituted.
	// Pattern B/C: MOVE;OP;MOVE -> OP with one source/dest substituted.
	// All temps must be dead outside the pattern window.
	if (i + 3 < func.instructions.size()) {
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

			if (op.operands[1].type == IRValue::Type::REGISTER &&
			    op.operands[2].type == IRValue::Type::REGISTER) {
				int op_lhs = std::get<int>(op.operands[1].value);
				int op_rhs = std::get<int>(op.operands[2].value);

				if (move1_dst == op_lhs && move2_dst == op_rhs &&
				    move3_src == op_dst) {
					bool tmp1_safe = !is_reg_read_outside(func, move1_dst, i, i + 3);
					bool tmp2_safe = !is_reg_read_outside(func, move2_dst, i, i + 3);
					bool dst_safe = !is_reg_read_outside(func, op_dst, i, i + 3);

					// Sources read at i instead of i+2; nothing in between may redefine them.
					bool sources_safe = move2_dst != move1_src &&
						op_dst != move1_src && op_dst != move2_src;

					if (tmp1_safe && tmp2_safe && dst_safe && sources_safe) {
						IRInstruction new_op = op;
						new_op.operands[0] = IRValue::reg(move3_dst);
						new_op.operands[1] = IRValue::reg(move1_src);
						new_op.operands[2] = IRValue::reg(move2_src);
						new_instructions.push_back(new_op);

						i += 4;
						return true;
					}
				}
			}
		}
	}

	if (i + 2 < func.instructions.size()) {
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

			if (op.operands[1].type == IRValue::Type::REGISTER) {
				int op_lhs = std::get<int>(op.operands[1].value);

				if (move1_dst == op_lhs && move2_src == op_dst &&
				    !is_reg_read_outside(func, move1_dst, i, i + 2) &&
				    !is_reg_read_outside(func, op_dst, i, i + 2) &&
				    op_dst != move1_src) {

					IRInstruction new_op = op;
					new_op.operands[0] = IRValue::reg(move2_dst);
					new_op.operands[1] = IRValue::reg(move1_src);
					new_instructions.push_back(new_op);

					i += 3;
					return true;
				}
			}

			if (op.operands[2].type == IRValue::Type::REGISTER) {
				int op_rhs = std::get<int>(op.operands[2].value);

				if (move1_dst == op_rhs && move2_src == op_dst &&
				    !is_reg_read_outside(func, move1_dst, i, i + 2) &&
				    !is_reg_read_outside(func, op_dst, i, i + 2) &&
				    op_dst != move1_src) {

					IRInstruction new_op = op;
					new_op.operands[0] = IRValue::reg(move2_dst);
					new_op.operands[2] = IRValue::reg(move1_src);
					new_instructions.push_back(new_op);

					i += 3;
					return true;
				}
			}
		}
	}

	// Pattern E: MOVE tmp,var; LOAD const; OP dst,tmp,const; MOVE var,dst -> LOAD const; OP var,var,const
	if (i + 3 < func.instructions.size()) {
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

			if (op.operands[1].type == IRValue::Type::REGISTER &&
			    op.operands[2].type == IRValue::Type::REGISTER) {
				int op_lhs = std::get<int>(op.operands[1].value);
				int op_rhs = std::get<int>(op.operands[2].value);

				if (move1_dst == op_lhs && load_dst == op_rhs &&
				    move1_src == move2_dst && move2_src == op_dst) {
					// tmp and dst deleted; load_dst survives (constant load kept).
					bool tmp1_safe = !is_reg_read_outside(func, move1_dst, i, i + 3);
					bool dst_safe = !is_reg_read_outside(func, op_dst, i, i + 3);

					if (tmp1_safe && dst_safe) {
						new_instructions.push_back(load);

						IRInstruction new_op = op;
						new_op.operands[0] = IRValue::reg(move2_dst);
						new_op.operands[1] = IRValue::reg(move1_src);
						new_instructions.push_back(new_op);

						i += 4;
						return true;
					}
				}
			}
		}
	}
	return false;
}

// MOVE a,b; MOVE b,a -> delete both (tmp must be dead outside).
bool IROptimizer::try_eliminate_move_pair(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions) {
	if (!(i + 1 < func.instructions.size())) {
		return false;
	}
	if (func.instructions[i].opcode == IROpcode::MOVE &&
	    func.instructions[i + 1].opcode == IROpcode::MOVE) {
		const auto& move1 = func.instructions[i];
		const auto& move2 = func.instructions[i + 1];

		int move1_dst = std::get<int>(move1.operands[0].value);
		int move1_src = std::get<int>(move1.operands[1].value);
		int move2_dst = std::get<int>(move2.operands[0].value);
		int move2_src = std::get<int>(move2.operands[1].value);

		if (move1_dst == move2_src && move1_src == move2_dst && move1_dst != move1_src &&
		    !is_reg_read_outside(func, move1_dst, i, i + 1)) {
			i += 2;
			return true;
		}
	}
	return false;
}

// OP dst,...; MOVE result,dst -> OP result,... (dst must be dead outside).
bool IROptimizer::try_fold_move_after_op(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions) {
	if (!(i + 1 < func.instructions.size())) {
		return false;
	}
	if (ir_has_effect(func.instructions[i].opcode, IR_ARITHMETIC) &&
	    func.instructions[i + 1].opcode == IROpcode::MOVE) {

		const auto& op = func.instructions[i];
		const auto& move = func.instructions[i + 1];

		if (op.operands.size() >= 1 && move.operands.size() >= 2) {
			int op_dst = std::get<int>(op.operands[0].value);
			int move_dst = std::get<int>(move.operands[0].value);
			int move_src = std::get<int>(move.operands[1].value);

			if (move_src == op_dst &&
			    !is_reg_read_outside(func, op_dst, i, i + 1)) {

				IRInstruction new_op = op;
				new_op.operands[0] = IRValue::reg(move_dst);
				new_instructions.push_back(new_op);

				i += 2;
				return true;
			}
		}
	}
	return false;
}

void IROptimizer::peephole_optimization(IRFunction& func) {
	static const std::vector<PeepholePattern> patterns = {
		&IROptimizer::try_fuse_compare_and_branch,
		&IROptimizer::try_eliminate_moves_around_op,
		&IROptimizer::try_eliminate_move_pair,
		&IROptimizer::try_fold_move_after_op,
		&IROptimizer::try_remove_branch_to_next,
	};

	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());

	size_t i = 0;
	while (i < func.instructions.size()) {
		bool matched = false;
		for (const auto& pattern : patterns) {
			if ((this->*pattern)(func, i, new_instructions)) {
				matched = true;
				break;
			}
		}
		if (!matched) {
			new_instructions.push_back(func.instructions[i]);
			i++;
		}
	}

	func.instructions = std::move(new_instructions);
}

void IROptimizer::copy_propagation(IRFunction& func) {
	// LOAD_IMM rN, K; MOVE rM, rN -> LOAD_IMM rM, K (when rN is dead).
	struct ConstantInfo {
		IROpcode opcode;
		IRValue value;  // The actual constant value
		// Propagated to the copy; untyped LOAD_IMM reads as unknown to later passes.
		IRInstruction::TypeHint type_hint;
	};

	std::unordered_map<int, ConstantInfo> constant_regs;
	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());

	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		if (instr.opcode == IROpcode::LABEL) {
			constant_regs.clear();
		}

		// Kill via ir_destination_register (handles CALL's operand-1 destination).
		const int killed = ir_destination_register(instr);
		if (killed >= 0) {
			constant_regs.erase(killed);
		}

		if (instr.opcode == IROpcode::LOAD_IMM || instr.opcode == IROpcode::LOAD_FLOAT_IMM) {
			if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER &&
			    instr.operands.size() >= 2) {
				int dst = std::get<int>(instr.operands[0].value);
				constant_regs[dst] = {instr.opcode, instr.operands[1], instr.type_hint};
			}
		}

		if (instr.opcode == IROpcode::MOVE) {
			int dst = std::get<int>(instr.operands[0].value);
			int src = std::get<int>(instr.operands[1].value);

			if (constant_regs.count(src)) {
				const auto& info = constant_regs[src];
				new_instructions.emplace_back(info.opcode, IRValue::reg(dst), info.value);
				new_instructions.back().type_hint = info.type_hint;
				constant_regs[dst] = info;
			} else {
				new_instructions.push_back(instr);
			}
		} else {
			new_instructions.push_back(instr);
		}
	}

	func.instructions = std::move(new_instructions);
}

void IROptimizer::eliminate_dead_code(IRFunction& func) {
	// Delete pure instructions defining unread registers. Iterates to fixpoint:
	// deleting an instruction can make its inputs' definitions dead in turn.
	// Correctness depends on find_live_registers() catching every read.
	for (size_t round = 0; round < func.instructions.size() + 1; round++) {
		const auto live_regs = find_live_registers(func);

		std::vector<IRInstruction> new_instructions;
		new_instructions.reserve(func.instructions.size());

		for (const auto& instr : func.instructions) {
			const int dst = ir_destination_register(instr);
			if (dst >= 0 && live_regs.count(dst) == 0 && ir_instruction_is_pure(instr)) {
				continue;
			}
			new_instructions.push_back(instr);
		}

		if (new_instructions.size() == func.instructions.size()) {
			return;
		}
		func.instructions = std::move(new_instructions);
	}
}

std::unordered_set<int> IROptimizer::find_live_registers(const IRFunction& func) {
	std::unordered_set<int> live;

	// Conservative: a missed read lets DCE delete a live definition.
	std::vector<int> reads;
	for (const auto& instr : func.instructions) {
		reads.clear();
		ir_collect_read_registers(instr, reads);
		live.insert(reads.begin(), reads.end());
	}

	return live;
}


bool IROptimizer::is_register_used_after(const IRFunction& func, int reg, size_t instr_idx) {
	std::vector<int> reads;
	bool crossed_control_flow = false;
	for (size_t i = instr_idx; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		reads.clear();
		ir_collect_read_registers(instr, reads);
		for (int r : reads) {
			if (r == reg) {
				return true;
			}
		}

		// A definition kills liveness only in straight-line code; past control flow
		// it may be on a path not taken.
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
	std::unordered_map<int, int> reg_map;
	int next_reg = 0;

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
	// Read roles from the metadata table, not duplicated here.
	end_idx = std::min(end_idx, func.instructions.size());

	std::vector<int> reads;
	for (size_t i = start_idx + 1; i < end_idx; i++) {
		const auto& instr = func.instructions[i];

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
	// Roles from the shared table: branches/RETURN/STORE_GLOBAL/VSET read operand 0.
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
	// Delays pure loads; drops dead (overwritten without read) and identical consecutive ones.
	if (func.instructions.empty()) {
		return;
	}

	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());

	std::unordered_map<int, size_t> pending_stores;

	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		// Non-pure ends the straight-line run (control flow, calls, stores).
		if (!ir_instruction_is_pure(instr)) {
			flush_pending(new_instructions, pending_stores, func);
			new_instructions.push_back(instr);
			continue;
		}

		if (reads_pending_store(instr, pending_stores)) {
			flush_pending(new_instructions, pending_stores, func);
		}

		if (ir_has_effect(instr.opcode, IR_SIMPLE_LOAD) && !instr.operands.empty() &&
		    instr.operands[0].type == IRValue::Type::REGISTER) {
			int dst = std::get<int>(instr.operands[0].value);

			if (pending_stores.count(dst)) {
				size_t prev_idx = pending_stores[dst];
				const auto& prev_instr = func.instructions[prev_idx];

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
					continue;
				}

				pending_stores[dst] = i;
				continue;
			}

			pending_stores[dst] = i;
			continue;
		}

		// Definition without intervening read: pending store is dead.
		const int defined = ir_destination_register(instr);
		if (defined >= 0) {
			pending_stores.erase(defined);
		}

		new_instructions.push_back(instr);
	}

	flush_pending(new_instructions, pending_stores, func);

	func.instructions = std::move(new_instructions);
}

std::vector<IROptimizer::LoopInfo> IROptimizer::identify_loops(const IRFunction& func) {
	std::vector<LoopInfo> loops;

	std::unordered_map<std::string, size_t> label_positions;
	for (size_t i = 0; i < func.instructions.size(); i++) {
		if (func.instructions[i].opcode == IROpcode::LABEL) {
			std::string label = std::get<std::string>(func.instructions[i].operands[0].value);
			label_positions[label] = i;
		}
	}

	std::unordered_map<std::string, std::string> loop_to_exit;

	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		if (instr.opcode == IROpcode::JUMP && !instr.operands.empty() &&
		    instr.operands[0].type == IRValue::Type::LABEL) {
			std::string target_label = std::get<std::string>(instr.operands[0].value);

			auto it = label_positions.find(target_label);
			if (it != label_positions.end() && it->second <= i) {
				size_t header_idx = it->second;

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

				size_t exit_idx = i + 1;
				if (!exit_label.empty()) {
					auto exit_it = label_positions.find(exit_label);
					if (exit_it != label_positions.end()) {
						exit_idx = exit_it->second;
					}
				}

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
	if (!ir_has_effect(instr.opcode, IR_SIMPLE_LOAD)) {
		return false;
	}

	if (instr.opcode == IROpcode::LOAD_IMM ||
	    instr.opcode == IROpcode::LOAD_FLOAT_IMM ||
	    instr.opcode == IROpcode::LOAD_BOOL ||
	    instr.opcode == IROpcode::LOAD_STRING) {
		return true;
	}

	if (instr.opcode == IROpcode::MOVE && instr.operands.size() >= 2 &&
	    instr.operands[1].type == IRValue::Type::REGISTER) {
		int src_reg = std::get<int>(instr.operands[1].value);
		return invariant_regs.count(src_reg) > 0;
	}

	return false;
}

bool IROptimizer::can_safely_hoist(const IRInstruction& instr, size_t instr_idx, const LoopInfo& loop, const IRFunction& func) {
	// Safe iff: (1) writes a register, (2) inputs unmodified in loop,
	// (3) dest not read before written, (4) single definition, (5) unconditional.
	// (4)+(5) found by fuzzer: two defs on different paths miscompile when hoisted.
	const int dst_reg = ir_destination_register(instr);
	if (dst_reg < 0) {
		return false;
	}

	std::vector<int> reads;
	ir_collect_read_registers(instr, reads);
	std::unordered_set<int> src_regs(reads.begin(), reads.end());

	for (size_t i = loop.header_idx; i < loop.end_idx && i < func.instructions.size(); i++) {
		const auto& loop_instr = func.instructions[i];

		if (i == instr_idx) {
			continue;
		}

		// ir_destination_register handles CALL's non-zero dest operand.
		const int modified_reg = ir_destination_register(loop_instr);
		if (modified_reg >= 0 && src_regs.count(modified_reg)) {
			return false;
		}

		// Second definition: hoisting picks one value for both paths.
		if (modified_reg == dst_reg) {
			return false;
		}

		if (i < instr_idx) {
			reads.clear();
			ir_collect_read_registers(loop_instr, reads);
			for (int reg : reads) {
				if (reg == dst_reg) {
					return false;
				}
			}
		}
	}

	return is_unconditional_in_loop(instr_idx, loop, func);
}

bool IROptimizer::is_unconditional_in_loop(size_t instr_idx, const LoopInfo& loop, const IRFunction& func) {
	// Between header and instr_idx: any LABEL (join), JUMP, or intra-loop branch
	// means instr_idx may be skipped. Branches exiting the loop don't count.
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
		return;
	}

	// Skip LICM entirely for nested loops.
	for (const auto& loop : loops) {
		for (const auto& other : loops) {
			if (&loop == &other) continue;
			if (loop.header_idx > other.header_idx && loop.header_idx < other.end_idx) {
				return;
			}
		}
	}

	for (const auto& loop : loops) {
			// Seed with registers defined before the loop.
			std::unordered_set<int> invariant_regs;

			for (size_t i = 0; i < loop.header_idx; i++) {
				const auto& instr = func.instructions[i];
				if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
					invariant_regs.insert(std::get<int>(instr.operands[0].value));
				}
			}

			std::unordered_set<size_t> invariant_instrs;
			bool loop_changed = true;

			while (loop_changed) {
				loop_changed = false;

				for (size_t i = loop.header_idx + 1; i < loop.end_idx && i < func.instructions.size(); i++) {
					if (invariant_instrs.count(i)) {
						continue;
					}

					const auto& instr = func.instructions[i];

					if (ir_is_control_flow(instr.opcode)) {
						continue;
					}

					if (is_loop_invariant(instr, loop, func, invariant_regs) &&
					    can_safely_hoist(instr, i, loop, func)) {
						invariant_instrs.insert(i);

						if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
							invariant_regs.insert(std::get<int>(instr.operands[0].value));
						}

						loop_changed = true;
					}
				}
			}

		if (!invariant_instrs.empty()) {
			std::vector<IRInstruction> hoisted;
			std::vector<IRInstruction> remaining;

			for (size_t i = 0; i < func.instructions.size(); i++) {
				if (i >= loop.header_idx && i < loop.end_idx && invariant_instrs.count(i)) {
					hoisted.push_back(func.instructions[i]);
				} else {
					remaining.push_back(func.instructions[i]);
				}
			}

			std::vector<IRInstruction> new_instructions;
			for (size_t i = 0; i < remaining.size(); i++) {
				if (remaining[i].opcode == IROpcode::LABEL && i < remaining.size() &&
				    std::get<std::string>(remaining[i].operands[0].value) == loop.header_label) {
					new_instructions.insert(new_instructions.end(), hoisted.begin(), hoisted.end());
				}
				new_instructions.push_back(remaining[i]);
			}

			func.instructions = std::move(new_instructions);
		}
	}
}

bool IROptimizer::register_unmodified_between(const IRFunction& func, int reg,
                                              size_t start_idx, size_t end_idx) {
	for (size_t i = start_idx + 1; i < end_idx && i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		if (ir_destination_register(instr) == reg) {
			return false;
		}

		if (instr.opcode == IROpcode::LABEL) {
			return false;
		}
	}

	return true;
}

void IROptimizer::enhanced_copy_propagation(IRFunction& func) {
	std::unordered_map<int, CopyInfo> copies;
	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());

	for (size_t i = 0; i < func.instructions.size(); i++) {
		auto instr = func.instructions[i];  // Make a copy we can modify

		// Deferred clear: reads happen before the effect, so propagate into this
		// instruction's operands first. Otherwise branches never see copies.
		const bool clear_after = !ir_instruction_is_pure(instr);

		// ir_reads_operand handles branches (read at 0) and CALL (write at 1).
		for (size_t j = 0; j < instr.operands.size(); j++) {
			if (!ir_reads_operand(instr, j)) {
				continue;
			}
			// INOUT: substitution would redirect the write.
			if (ir_writes_operand(instr, j)) {
				continue;
			}
			int reg = std::get<int>(instr.operands[j].value);

			if (copies.count(reg)) {
				const auto& copy_info = copies[reg];

				if (register_unmodified_between(func, copy_info.source_reg, copy_info.def_idx, i)) {
					instr.operands[j].value = copy_info.source_reg;
				}
			}
		}

		if (clear_after) {
			copies.clear();
		}

		const int written = ir_destination_register(instr);
		if (written >= 0) {
			copies.erase(written);

			for (auto it = copies.begin(); it != copies.end(); ) {
				if (it->second.source_reg == written) {
					it = copies.erase(it);
				} else {
					++it;
				}
			}
		}

		// Record after invalidation: recording first lets the kill erase it immediately.
		// Source read from propagated operand, so chains collapse in one pass.
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
