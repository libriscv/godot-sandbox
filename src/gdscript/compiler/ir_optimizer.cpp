#include "ir_optimizer.h"
#include "syscall_numbers.h"
#include "globals.h"
#include "compiler_exception.h"
#include "ir_verifier.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <optional>

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

// LICM can hoist code below the mark; slide it back to the loop label.
bool IROptimizer::tighten_scope_marks(IRFunction& func) {
	bool changed = false;
	for (size_t i = 0; i + 1 < func.instructions.size(); i++) {
		if (func.instructions[i].opcode != IROpcode::SCOPE_MARK) {
			continue;
		}
		size_t label = i + 1;
		while (label < func.instructions.size() &&
			func.instructions[label].opcode != IROpcode::LABEL)
		{
				const IROpcode op = func.instructions[label].opcode;
			if (op == IROpcode::SCOPE_MARK || op == IROpcode::SCOPE_RELEASE ||
				ir_is_control_flow(op))
			{
				break;
			}
			label++;
		}
		if (label >= func.instructions.size() ||
			func.instructions[label].opcode != IROpcode::LABEL || label == i + 1)
		{
			continue;
		}
		IRInstruction mark = func.instructions[i];
		func.instructions.erase(func.instructions.begin() + i);
		func.instructions.insert(func.instructions.begin() + (label - 1), std::move(mark));
		invalidate_analysis();
		changed = true;
	}
	return changed;
}

const std::vector<IRPass>& IROptimizer::pipeline() {
	// Peephole appears three times: each earlier pass exposes new patterns.
	static const std::vector<IRPass> passes = {
		{ "constant-folding", &IROptimizer::constant_folding },
		// Folding known branches strands arms; unreachable-code removes them.
		{ "unreachable-code", &IROptimizer::eliminate_unreachable_code },
		{ "copy-propagation", &IROptimizer::copy_propagation },
		{ "enhanced-copy-propagation", &IROptimizer::enhanced_copy_propagation },
		{ "scalar-replacement", &IROptimizer::scalar_replace_dictionaries },
		{ "dictionary-updates", &IROptimizer::fuse_dictionary_updates },
		{ "lazy-strings", &IROptimizer::sink_lazy_strings },
		{ "licm", &IROptimizer::loop_invariant_code_motion },
		{ "peephole", &IROptimizer::peephole_optimization },
		{ "peephole", &IROptimizer::peephole_optimization },
		{ "redundant-stores", &IROptimizer::eliminate_redundant_stores },
		{ "peephole", &IROptimizer::peephole_optimization },
		{ "dead-code", &IROptimizer::eliminate_dead_code },
		{ "compact-registers", &IROptimizer::reduce_register_pressure },
		{ "scope-marks", &IROptimizer::tighten_scope_marks },
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
	m_strings = &program.strings;
	m_string_constants = &program.string_constants;
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
	invalidate_analysis();
	const auto& passes = pipeline();
	const size_t limit = std::min(m_pass_limit, passes.size());
	const bool verify = ir_verification_enabled();

	// Verify pre-pass IR so a corruption can be blamed on the right pass.
	if (verify) {
		ir_verify(func, "codegen", m_strings);
	}

	// A pass is a function of the IR: one that reported no change cannot change
	// anything until another pass does. Skips the repeat peephole runs.
	std::vector<const char*> settled;

	for (size_t i = 0; i < limit; i++) {
		if (!is_pass_enabled(passes[i].name)) {
			continue;
		}
		bool at_fixed_point = false;
		for (const char* name : settled) {
			if (std::strcmp(name, passes[i].name) == 0) {
				at_fixed_point = true;
				break;
			}
		}
		if (at_fixed_point) {
			continue;
		}
		if ((this->*passes[i].run)(func)) {
			settled.clear();
		} else {
			settled.push_back(passes[i].name);
		}
		if (verify) {
			ir_verify(func, passes[i].name, m_strings);
		}
	}

	// Bookkeeping, not an optimization: runs even with all passes disabled.
	int max_reg = 0;
	for (const auto& instr : func.instructions) {
		for (const auto& op : instr.operands) {
			if (op.type == IRValue::Type::REGISTER) {
				int reg = op.reg_index();
				max_reg = std::max(max_reg, reg);
			}
		}
	}
	// Parameters r0..N-1 occupy frame slots even when no instruction names them.
	func.max_registers = std::max(max_reg + 1, static_cast<int>(func.parameters.size()));

	// Verify after max_registers recomputation; per-pass checks ran against the old count.
	if (verify) {
		ir_verify(func, "max-registers", m_strings);
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

const IROptimizer::FunctionAnalysis& IROptimizer::analysis(const IRFunction& func) {
	if (m_analysis.valid && m_analysis.func == &func &&
		m_analysis.instruction_count == func.instructions.size()) {
		return m_analysis;
	}

	m_analysis.valid = true;
	m_analysis.func = &func;
	m_analysis.instruction_count = func.instructions.size();
	m_analysis.label_index.clear();
	m_analysis.blocks.clear();

	std::vector<Block>& blocks = m_analysis.blocks;
	const size_t count = func.instructions.size();
	if (count == 0) {
		return m_analysis;
	}

	std::unordered_map<uint32_t, size_t>& label_index = m_analysis.label_index;
	for (size_t i = 0; i < count; i++) {
		const auto& instr = func.instructions[i];
		if (ir_has_effect(instr.opcode, IR_LABEL) && !instr.operands.empty()) {
			label_index.emplace(instr.operands[0].string_id, i);
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
			Block block;
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
				auto it = label_index.find(operand.string_id);
				if (it != label_index.end()) {
					blocks[b].successors.push_back(block_of[it->second]);
				}
			}
		}
		if (!ir_has_effect(last.opcode, IR_TERMINATOR) && b + 1 < blocks.size()) {
			blocks[b].successors.push_back(b + 1);
		}
	}
	return m_analysis;
}

bool IROptimizer::replace_instructions(IRFunction& func, std::vector<IRInstruction>&& fresh) {
	bool changed = fresh.size() != func.instructions.size();
	for (size_t i = 0; !changed && i < fresh.size(); i++) {
		changed = !(fresh[i] == func.instructions[i]);
	}
	func.instructions = std::move(fresh);
	if (changed) {
		invalidate_analysis();
	}
	return changed;
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

bool IROptimizer::constant_folding(IRFunction& func) {
	const std::vector<Block>& blocks = analysis(func).blocks;
	if (blocks.empty()) {
		return false;
	}

	// Entry state parallel to the shared block shape. Uninitialized = unreachable.
	std::vector<ConstantMap> entry(blocks.size());
	std::vector<char> entry_initialized(blocks.size(), 0);

	// Forward dataflow to fixpoint; monotone by construction (entries only shrink).
	entry_initialized[0] = 1;

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

		m_constants = entry[index];
		for (size_t i = blocks[index].begin; i < blocks[index].end; i++) {
			fold_instruction(func.instructions[i], nullptr);
		}

		for (size_t successor : blocks[index].successors) {
			bool changed = false;
			if (!entry_initialized[successor]) {
				entry[successor] = m_constants;
				entry_initialized[successor] = 1;
				changed = true;
			} else {
				changed = meet_constants(entry[successor], m_constants);
			}
			if (changed) {
				worklist.push_back(successor);
			}
		}
	}

	if (!converged) {
		for (size_t b = 1; b < blocks.size(); b++) {
			entry[b].clear();
		}
	}

	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());
	for (size_t b = 0; b < blocks.size(); b++) {
		const Block& block = blocks[b];
		// Unreachable blocks: no state, leave for eliminate_unreachable_code().
		m_constants = entry_initialized[b] ? entry[b] : ConstantMap {};
		for (size_t i = block.begin; i < block.end; i++) {
			fold_instruction(func.instructions[i], &new_instructions);
		}
	}
	return replace_instructions(func, std::move(new_instructions));
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
		case IROpcode::SCOPE_MARK:
		case IROpcode::SCOPE_RELEASE:
			emit(instr);
			break;

		case IROpcode::LOAD_IMM: {
			int reg = instr.operands[0].reg_index();
			int64_t val = instr.operands[1].immediate();
			ConstantValue cv;
			cv.type = ConstantValue::Type::INT;
			cv.int_value = val;
			set_register_constant(reg, cv);
			emit(instr);
			break;
		}

		case IROpcode::LOAD_FLOAT_IMM: {
			int reg = instr.operands[0].reg_index();
			double val = instr.operands[1].float_number();
			ConstantValue cv;
			cv.type = ConstantValue::Type::FLOAT;
			cv.float_value = val;
			set_register_constant(reg, cv);
			emit(instr);
			break;
		}

		case IROpcode::LOAD_BOOL: {
			int reg = instr.operands[0].reg_index();
			int64_t val = instr.operands[1].immediate();
			ConstantValue cv;
			cv.type = ConstantValue::Type::BOOL;
			cv.bool_value = (val != 0);
			set_register_constant(reg, cv);
			emit(instr);
			break;
		}

		case IROpcode::LOAD_STRING:
		case IROpcode::LOAD_STRING_AS: {
			int reg = instr.operands[0].reg_index();
			invalidate_register(reg);
			emit(instr);
			break;
		}

		case IROpcode::MOVE: {
			int dst = instr.operands[0].reg_index();
			int src = instr.operands[1].reg_index();

			// Propagate constant value
			if (m_constants.count(src)) {
				m_constants[dst] = m_constants[src];
			} else {
				invalidate_register(dst);
			}
			emit(instr);
			break;
		}

		case IROpcode::CONVERT: {
			const int dst = instr.operands[0].reg_index();
			const int src = instr.operands[1].reg_index();
			auto found = m_constants.find(src);
			if (found != m_constants.end()) {
				ConstantValue result;
				const ConstantValue& value = found->second;
				if (instr.type_hint == Variant::FLOAT) {
					if (value.type == ConstantValue::Type::INT) {
						result.type = ConstantValue::Type::FLOAT;
						result.float_value = static_cast<double>(value.int_value);
					} else if (value.type == ConstantValue::Type::BOOL) {
						result.type = ConstantValue::Type::FLOAT;
						result.float_value = value.bool_value ? 1.0 : 0.0;
					}
				} else if (instr.type_hint == Variant::INT) {
					if (value.type == ConstantValue::Type::BOOL) {
						result.type = ConstantValue::Type::INT;
						result.int_value = value.bool_value ? 1 : 0;
					} else if (value.type == ConstantValue::Type::FLOAT &&
						std::isfinite(value.float_value) &&
						value.float_value >= static_cast<double>(INT64_MIN) &&
						value.float_value < -static_cast<double>(INT64_MIN))
					{
						result.type = ConstantValue::Type::INT;
						result.int_value = static_cast<int64_t>(value.float_value);
					}
				}

				if (result.type == ConstantValue::Type::FLOAT) {
					IRInstruction load(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(dst),
						IRValue::fimm(result.float_value));
					load.type_hint = Variant::FLOAT;
					emit(load);
					set_register_constant(dst, result);
					folded = true;
				} else if (result.type == ConstantValue::Type::INT) {
					IRInstruction load(IROpcode::LOAD_IMM, IRValue::reg(dst),
						IRValue::imm(result.int_value));
					load.type_hint = Variant::INT;
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
					int dst = instr.operands[0].reg_index();
					invalidate_register(dst);
				}
				emit(instr);
				break;
			}

			int dst = instr.operands[0].reg_index();
			int lhs_reg = instr.operands[1].reg_index();
			int rhs_reg = instr.operands[2].reg_index();

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
					int dst = instr.operands[0].reg_index();
					invalidate_register(dst);
				}
				emit(instr);
				break;
			}

			int dst = instr.operands[0].reg_index();
			int lhs_reg = instr.operands[1].reg_index();
			int rhs_reg = instr.operands[2].reg_index();

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

		case IROpcode::DECIMAL_LENGTH: {
			const int dst = instr.operands[0].reg_index();
			const int src = instr.operands[1].reg_index();
			auto known = m_constants.find(src);
			if (known != m_constants.end() && known->second.type == ConstantValue::Type::INT) {
				ConstantValue result;
				result.type = ConstantValue::Type::INT;
				result.int_value = ir_decimal_length(known->second.int_value);
				IRInstruction load(IROpcode::LOAD_IMM, IRValue::reg(dst), IRValue::imm(result.int_value));
				load.type_hint = Variant::INT;
				emit(load);
				set_register_constant(dst, result);
			} else {
				invalidate_register(dst);
				emit(instr);
			}
			break;
		}

		case IROpcode::NEG: {
			int dst = instr.operands[0].reg_index();
			int src = instr.operands[1].reg_index();

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
			int dst = instr.operands[0].reg_index();
			int src = instr.operands[1].reg_index();

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
					int dst = instr.operands[0].reg_index();
					invalidate_register(dst);
				}
				emit(instr);
				break;
			}

			int dst = instr.operands[0].reg_index();
			int lhs_reg = instr.operands[1].reg_index();
			int rhs_reg = instr.operands[2].reg_index();

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
				const int cond = instr.operands[0].reg_index();
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

		case IROpcode::GLOBAL_CALL: {
			const int dst = ir_destination_register(instr);
			if (dst >= 0 && instr.operands.size() >= 4 &&
				instr.operands[1].type == IRValue::Type::IMMEDIATE)
			{
				const GlobalFunction& info = global_function(
					static_cast<GlobalFn>(instr.operands[1].immediate()));
				const size_t count = size_t(instr.operands[3].immediate());
				if (!info.impure && count <= 3 && instr.operands.size() == 4 + count &&
					(info.kind == GlobalKind::INT_OP || info.kind == GlobalKind::FLOAT_OP))
				{
					bool known = true;
					std::array<int64_t, 3> ints {};
					std::array<double, 3> floats {};
					for (size_t i = 0; i < count; i++) {
						const int reg = instr.operands[4 + i].reg_index();
						auto value = m_constants.find(reg);
						if (value == m_constants.end()) {
							known = false;
							break;
						}
						const ConstantValue& cv = value->second;
						if (info.kind == GlobalKind::INT_OP) {
							if (cv.type == ConstantValue::Type::INT) ints[i] = cv.int_value;
							else if (cv.type == ConstantValue::Type::BOOL) ints[i] = cv.bool_value ? 1 : 0;
							else { known = false; break; }
						} else {
							if (cv.type == ConstantValue::Type::FLOAT) floats[i] = cv.float_value;
							else if (cv.type == ConstantValue::Type::INT) floats[i] = double(cv.int_value);
							else if (cv.type == ConstantValue::Type::BOOL) floats[i] = cv.bool_value ? 1.0 : 0.0;
							else { known = false; break; }
						}
					}
					if (known && info.kind == GlobalKind::INT_OP) {
						const int64_t value = eval_global_int(info.fn, ints.data(), count);
						ConstantValue result;
						if (info.result == GlobalResult::BOOL) {
							result.type = ConstantValue::Type::BOOL;
							result.bool_value = value != 0;
							emit(IRInstruction(IROpcode::LOAD_BOOL, IRValue::reg(dst),
								IRValue::imm(result.bool_value ? 1 : 0)));
						} else if (info.result == GlobalResult::INT) {
							result.type = ConstantValue::Type::INT;
							result.int_value = value;
							IRInstruction load(IROpcode::LOAD_IMM, IRValue::reg(dst), IRValue::imm(value));
							load.type_hint = Variant::INT;
							emit(load);
						}
						if (result.is_constant()) {
							set_register_constant(dst, result);
							folded = true;
						}
					} else if (known && info.kind == GlobalKind::FLOAT_OP) {
						const double value = eval_global_float(info.fn, floats.data(), count);
						ConstantValue result;
						if (info.result == GlobalResult::BOOL) {
							result.type = ConstantValue::Type::BOOL;
							result.bool_value = value != 0.0;
							emit(IRInstruction(IROpcode::LOAD_BOOL, IRValue::reg(dst),
								IRValue::imm(result.bool_value ? 1 : 0)));
						} else if (info.result == GlobalResult::FLOAT) {
							result.type = ConstantValue::Type::FLOAT;
							result.float_value = value;
							IRInstruction load(IROpcode::LOAD_FLOAT_IMM, IRValue::reg(dst), IRValue::fimm(value));
							load.type_hint = Variant::FLOAT;
							emit(load);
						}
						if (result.is_constant()) {
							set_register_constant(dst, result);
							folded = true;
						}
					}
				}
			}
			if (!folded) {
				invalidate_register(dst);
				emit(instr);
			}
			break;
		}

		// Element writes: operand 0 is the container (not a destination), keep its constant.
		case IROpcode::ARRAY_SET:
		case IROpcode::DICT_SET:
		case IROpcode::DICT_SET_CONST:
		case IROpcode::DICT_SET_CONST_STR:
		case IROpcode::DICT_OPERATE:
		case IROpcode::DICT_OPERATE_CONST:
			emit(instr);
			break;

		// Invalidate destination only; input constants survive.
		case IROpcode::ARRAY_APPEND:
		case IROpcode::ARRAY_GET:
		case IROpcode::DICT_GET_CONST:
		case IROpcode::DICT_HAS_CONST:
		case IROpcode::STRUCT_CHECK:
		case IROpcode::PRINT:
		case IROpcode::VCALL:
		case IROpcode::VGET:
		case IROpcode::VSET:
		case IROpcode::VARIANT_SET:
		case IROpcode::CALL_SYSCALL:
		case IROpcode::GET_NODE:
		case IROpcode::LOAD_RESOURCE:
		case IROpcode::LOAD_RESOURCE_VAR:
		case IROpcode::MAKE_CALLABLE:
		case IROpcode::CONSTRUCT:
		case IROpcode::AWAIT: // Host-provided result; not foldable, not a block boundary.

		case IROpcode::CALL:
		case IROpcode::CALL_HOSTED:
			if (!instr.operands.empty() && instr.operands[0].type == IRValue::Type::REGISTER) {
				invalidate_register(instr.operands[0].reg_index());
			}
			emit(instr);
			break;

		// No default: new opcodes must be listed explicitly (compile error otherwise).
		case IROpcode::BIT_NOT:
		case IROpcode::COERCE:
		case IROpcode::JUMP:
		case IROpcode::LOAD_NIL:
		case IROpcode::THROW:
		case IROpcode::LOAD_GLOBAL:
		case IROpcode::MAKE_ARRAY:
		case IROpcode::MAKE_COLOR:
		case IROpcode::MAKE_DICTIONARY:
		case IROpcode::MAKE_DICTIONARY_KEYED:
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
		case IROpcode::TYPE_TEST_MASK:
		case IROpcode::TRAIT_TEST:
		case IROpcode::TYPE_OF:
		case IROpcode::MAKE_SCOPED:
		case IROpcode::BATCH_GET:
		case IROpcode::CODEPOINT_GET:
		case IROpcode::WINDOW_OPEN:
		case IROpcode::WINDOW_GET:
		case IROpcode::WINDOW_SET:
		case IROpcode::WINDOW_FLUSH:
		case IROpcode::SWITCH:
		case IROpcode::VGET_INLINE:
		case IROpcode::VSET_INLINE:
			// Only the destination changes.  Invalidating every register operand
			// used to discard constants merely read by MAKE_VECTOR*, VGET_INLINE,
			// STORE_GLOBAL and the other catch-all instructions.
			if (const int dst = ir_destination_register(instr); dst >= 0) {
				invalidate_register(dst);
			}
			emit(instr);
			break;
	}
}

// CFG reachability, not linear scan: folded branches strand labelled blocks.
// Distinct from DCE, which removes instructions defining unread registers.
bool IROptimizer::eliminate_unreachable_code(IRFunction& func) {
	const std::vector<Block>& blocks = analysis(func).blocks;
	if (blocks.empty()) {
		return false;
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
	return replace_instructions(func, std::move(new_instructions));
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
		int cmp_dst = cmp_instr.operands[0].reg_index();
		int branch_reg = branch_instr.operands[0].reg_index();

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
	const uint32_t name = target->string_id;

	for (size_t j = i + 1; j < func.instructions.size(); j++) {
		const auto& next = func.instructions[j];
		if (!ir_has_effect(next.opcode, IR_LABEL)) {
			return false;
		}
		if (!next.operands.empty() && next.operands[0].string_id == name) {
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
	int dst = instr.operands[0].reg_index();
	int src = instr.operands[1].reg_index();
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

			int move1_dst = move1.operands[0].reg_index();
			int move1_src = move1.operands[1].reg_index();
			int move2_dst = move2.operands[0].reg_index();
			int move2_src = move2.operands[1].reg_index();
			int op_dst = op.operands[0].reg_index();
			int move3_dst = move3.operands[0].reg_index();
			int move3_src = move3.operands[1].reg_index();

			if (op.operands[1].type == IRValue::Type::REGISTER &&
			    op.operands[2].type == IRValue::Type::REGISTER) {
				int op_lhs = op.operands[1].reg_index();
				int op_rhs = op.operands[2].reg_index();

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

			int move1_dst = move1.operands[0].reg_index();
			int move1_src = move1.operands[1].reg_index();
			int op_dst = op.operands[0].reg_index();
			int move2_dst = move2.operands[0].reg_index();
			int move2_src = move2.operands[1].reg_index();

			if (op.operands[1].type == IRValue::Type::REGISTER) {
				int op_lhs = op.operands[1].reg_index();

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
				int op_rhs = op.operands[2].reg_index();

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

			int move1_dst = move1.operands[0].reg_index();
			int move1_src = move1.operands[1].reg_index();
			int load_dst = load.operands[0].reg_index();
			int op_dst = op.operands[0].reg_index();
			int move2_dst = move2.operands[0].reg_index();
			int move2_src = move2.operands[1].reg_index();

			if (op.operands[1].type == IRValue::Type::REGISTER &&
			    op.operands[2].type == IRValue::Type::REGISTER) {
				int op_lhs = op.operands[1].reg_index();
				int op_rhs = op.operands[2].reg_index();

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

		int move1_dst = move1.operands[0].reg_index();
		int move1_src = move1.operands[1].reg_index();
		int move2_dst = move2.operands[0].reg_index();
		int move2_src = move2.operands[1].reg_index();

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
	const int first_dst_index = ir_destination_operand_index(func.instructions[i].opcode);
	if (first_dst_index >= 0 &&
		!ir_reads_operand(func.instructions[i], size_t(first_dst_index)) &&
		func.instructions[i + 1].opcode == IROpcode::MOVE) {

		const auto& op = func.instructions[i];
		const auto& move = func.instructions[i + 1];

		const int dst_index = ir_destination_operand_index(op.opcode);
		if (dst_index >= 0 && static_cast<size_t>(dst_index) < op.operands.size() &&
			op.operands[size_t(dst_index)].type == IRValue::Type::REGISTER &&
			move.operands.size() >= 2) {
			int op_dst = op.operands[size_t(dst_index)].reg_index();
			int move_dst = move.operands[0].reg_index();
			int move_src = move.operands[1].reg_index();
			std::vector<int> op_reads;
			ir_collect_read_registers(op, op_reads);
			const bool destination_is_input =
				std::find(op_reads.begin(), op_reads.end(), move_dst) != op_reads.end();

			if (!destination_is_input && move_src == op_dst &&
			    !is_reg_read_outside(func, op_dst, i, i + 1)) {

				IRInstruction new_op = op;
				new_op.operands[size_t(dst_index)] = IRValue::reg(move_dst);
				new_instructions.push_back(new_op);

				i += 2;
				return true;
			}
		}
	}
	return false;
}

bool IROptimizer::peephole_optimization(IRFunction& func) {
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

	return replace_instructions(func, std::move(new_instructions));
}

bool IROptimizer::copy_propagation(IRFunction& func) {
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
				int dst = instr.operands[0].reg_index();
				constant_regs[dst] = {instr.opcode, instr.operands[1], instr.type_hint};
			}
		}

		if (instr.opcode == IROpcode::MOVE) {
			int dst = instr.operands[0].reg_index();
			int src = instr.operands[1].reg_index();

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

	return replace_instructions(func, std::move(new_instructions));
}

bool IROptimizer::eliminate_dead_code(IRFunction& func) {
	// Deletes pure instructions whose destination nothing reads. A deletion can
	// drop the last read of one of its inputs, so those inputs' definitions go
	// back on the worklist: same fixpoint as the per-round scan-and-rebuild,
	// without the rebuild.
	const size_t count = func.instructions.size();
	if (count == 0) {
		return false;
	}

	std::vector<int> reads;
	int max_reg = -1;
	for (const auto& instr : func.instructions) {
		max_reg = std::max(max_reg, ir_destination_register(instr));
		reads.clear();
		ir_collect_read_registers(instr, reads);
		for (int reg : reads) {
			max_reg = std::max(max_reg, reg);
		}
	}
	if (max_reg < 0) {
		return false;
	}

	const size_t nregs = size_t(max_reg) + 1;
	// Read count per register, and its definitions as a newest-first chain --
	// two flat arrays instead of a vector per register.
	std::vector<uint32_t> use_count(nregs, 0);
	std::vector<size_t> def_head(nregs, count);
	std::vector<size_t> def_next(count, count);

	for (size_t i = 0; i < count; i++) {
		const IRInstruction& instr = func.instructions[i];
		reads.clear();
		ir_collect_read_registers(instr, reads);
		for (int reg : reads) {
			if (reg >= 0) {
				use_count[size_t(reg)]++;
			}
		}
		const int dst = ir_destination_register(instr);
		if (dst >= 0) {
			def_next[i] = def_head[size_t(dst)];
			def_head[size_t(dst)] = i;
		}
	}

	std::vector<bool> deleted(count, false);
	std::vector<size_t> worklist;
	worklist.reserve(count);
	for (size_t i = count; i-- > 0;) {
		worklist.push_back(i);
	}

	size_t deletions = 0;
	while (!worklist.empty()) {
		const size_t i = worklist.back();
		worklist.pop_back();
		if (deleted[i]) {
			continue;
		}
		const IRInstruction& instr = func.instructions[i];
		const int dst = ir_destination_register(instr);
		if (dst < 0 || use_count[size_t(dst)] != 0 || !ir_instruction_is_pure(instr)) {
			continue;
		}
		deleted[i] = true;
		deletions++;

		reads.clear();
		ir_collect_read_registers(instr, reads);
		for (int reg : reads) {
			if (reg < 0 || --use_count[size_t(reg)] != 0) {
				continue;
			}
			for (size_t def = def_head[size_t(reg)]; def != count; def = def_next[def]) {
				if (!deleted[def]) {
					worklist.push_back(def);
				}
			}
		}
	}

	if (deletions == 0) {
		return false;
	}

	std::vector<IRInstruction> survivors;
	survivors.reserve(count - deletions);
	for (size_t i = 0; i < count; i++) {
		if (!deleted[i]) {
			survivors.push_back(func.instructions[i]);
		}
	}
	func.instructions = std::move(survivors);
	invalidate_analysis();
	return true;
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

bool IROptimizer::reduce_register_pressure(IRFunction& func) {
	std::unordered_map<int, int> reg_map;
	// r0 is the return slot, and parameters occupy r0..rN-1 by ABI.  Pin all of
	// them; only compiler temporaries may be renumbered into holes above that
	// prefix.
	reg_map[IRFunction::RETURN_REGISTER] = IRFunction::RETURN_REGISTER;
	for (size_t i = 0; i < func.parameters.size(); i++) {
		reg_map[int(i)] = int(i);
	}
	int next_reg = std::max(1, int(func.parameters.size()));

	for (const auto& instr : func.instructions) {
		for (const auto& op : instr.operands) {
			if (op.type == IRValue::Type::REGISTER) {
				int old_reg = op.reg_index();
				if (reg_map.find(old_reg) == reg_map.end()) {
					reg_map[old_reg] = next_reg++;
				}
			}
		}
	}
	bool changed = false;

	for (auto& instr : func.instructions) {
		for (auto& op : instr.operands) {
			if (op.type == IRValue::Type::REGISTER) {
				int old_reg = op.reg_index();
				changed = changed || reg_map[old_reg] != old_reg;
				op.reg_value = reg_map[old_reg];
			}
		}
	}
	for (IRFunction::DebugLocal& local : func.debug_locals) {
		auto mapped = reg_map.find(local.register_num);
		if (mapped != reg_map.end()) {
			changed = changed || local.register_num != mapped->second;
			local.register_num = mapped->second;
		}
	}
	if (changed) invalidate_analysis();
	return changed;
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
		cv.int_value = val.immediate();
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

// Replace a non-escaping Dictionary (struct or constant-keyed literal) with
// one register per key. Two shapes: flow-wide (single-def aliases, no use
// above the MAKE) and straight-line (no branches below, aliases end on
// overwrite). Keys may only be added before the first branch after the MAKE.
// Escaping use (return, call, erase, computed key) disqualifies.
bool IROptimizer::scalar_replace_dictionaries(IRFunction& func) {
	using Key = std::string; // "s" + String text, or "i" + decimal int
	bool changed = false;
	for (;;) {
		const std::vector<IRInstruction>& code = func.instructions;
		const int params = int(func.parameters.size());
		int nregs = std::max(func.max_registers, params);
		for (const IRInstruction& instr : code) {
			for (const IRValue& operand : instr.operands) {
				if (operand.type == IRValue::Type::REGISTER) {
					nregs = std::max(nregs, operand.reg_index() + 1);
				}
			}
		}
		std::vector<std::vector<size_t>> defs(static_cast<size_t>(nregs));
		for (size_t i = 0; i < code.size(); i++) {
			for (size_t operand = 0; operand < code[i].operands.size(); operand++) {
				if (ir_writes_operand(code[i], operand)) {
					defs[size_t(code[i].operands[operand].reg_index())].push_back(i);
				}
			}
		}
		const auto def_count = [&](int reg) {
			return defs[size_t(reg)].size() + (reg < params ? 1 : 0);
		};
		const auto string_key = [&](const IRValue& index) -> std::optional<Key> {
			if (m_string_constants == nullptr || index.type != IRValue::Type::IMMEDIATE ||
				index.immediate() < 0 || size_t(index.immediate()) >= m_string_constants->size())
			{
				return std::nullopt;
			}
			return "s" + (*m_string_constants)[size_t(index.immediate())];
		};
		// Bool/float literals hash differently from the int they equal.
		const auto register_key = [&](const IRValue& value) -> std::optional<Key> {
			if (value.type != IRValue::Type::REGISTER || def_count(value.reg_index()) != 1 ||
				value.reg_index() < params)
			{
				return std::nullopt;
			}
			const IRInstruction& def = code[defs[size_t(value.reg_index())][0]];
			if (def.opcode == IROpcode::LOAD_STRING) {
				return string_key(def.operands[1]);
			}
			if (def.opcode == IROpcode::LOAD_IMM && def.type_hint == Variant::INT) {
				return "i" + std::to_string(def.operands[1].immediate());
			}
			return std::nullopt;
		};

		bool replaced = false;
		for (size_t make_at = 0; make_at < code.size() && !replaced; make_at++) {
			const IRInstruction& make = code[make_at];
			const bool keyed = make.opcode == IROpcode::MAKE_DICTIONARY_KEYED;
			if ((!keyed && make.opcode != IROpcode::MAKE_DICTIONARY) || make.operands.size() < 2) {
				continue;
			}
			const int64_t count = make.operands[1].immediate();
			if (count < 0 || make.operands.size() != size_t(2 + count * 2)) {
				continue;
			}
			const int root = make.operands[0].reg_index();
			std::vector<std::pair<Key, IRValue>> initial;
			std::unordered_set<Key> distinct;
			bool constant_keys = true;
			for (int64_t i = 0; i < count && constant_keys; i++) {
				const IRValue& key_operand = make.operands[size_t(2 + i * 2)];
				const std::optional<Key> key = keyed ? string_key(key_operand) : register_key(key_operand);
				constant_keys = key.has_value() && distinct.insert(*key).second;
				if (constant_keys) {
					initial.emplace_back(*key, make.operands[size_t(3 + i * 2)]);
				}
			}
			if (!constant_keys) {
				continue;
			}

			const auto rewrite = [&](bool flow_wide, std::vector<IRInstruction>& fresh) {
				std::unordered_set<int> aliases{root};
				std::vector<int> reads;
				if (flow_wide && root == IRFunction::RETURN_REGISTER) {
					return false;
				}
				if (flow_wide) {
					for (bool grew = true; grew;) {
						grew = false;
						for (const IRInstruction& instr : code) {
							if (instr.opcode == IROpcode::MOVE && instr.operands.size() == 2 &&
								aliases.count(instr.operands[1].reg_index()) != 0)
							{
								grew |= aliases.insert(instr.operands[0].reg_index()).second;
							}
						}
					}
					for (int alias : aliases) {
						if (alias == IRFunction::RETURN_REGISTER || def_count(alias) != 1) {
							return false;
						}
					}
					for (size_t i = 0; i < make_at; i++) {
						reads.clear();
						ir_collect_read_registers(code[i], reads);
						for (int reg : reads) {
							if (aliases.count(reg) != 0) {
								return false;
							}
						}
					}
				}

				int next_register = nregs;
				std::unordered_map<Key, int> fields;
				fresh.clear();
				fresh.reserve(code.size() + initial.size());
				fresh.insert(fresh.end(), code.begin(), code.begin() + std::ptrdiff_t(make_at));
				for (const auto& [key, value] : initial) {
					const int field = next_register++;
					fields[key] = field;
					IRInstruction move(IROpcode::MOVE, IRValue::reg(field), value);
					move.line = make.line;
					fresh.push_back(std::move(move));
				}

				bool in_prefix = true;
				for (size_t i = make_at + 1; i < code.size(); i++) {
					const IRInstruction& instr = code[i];
					const bool control_flow = ir_is_control_flow(instr.opcode);
					in_prefix &= !control_flow;
					reads.clear();
					ir_collect_read_registers(instr, reads);
					const size_t alias_reads = size_t(std::count_if(reads.begin(), reads.end(),
						[&](int reg) { return aliases.count(reg) != 0; }));
					if (!flow_wide) {
						if (control_flow && !(instr.opcode == IROpcode::RETURN && alias_reads == 0)) {
							return false;
						}
					}
					if (alias_reads == 0) {
						for (size_t operand = 0; operand < instr.operands.size(); operand++) {
							if (ir_writes_operand(instr, operand)) {
								aliases.erase(instr.operands[operand].reg_index());
							}
						}
						fresh.push_back(instr);
						continue;
					}
					if (alias_reads != 1) {
						return false;
					}

					const auto alias_at = [&](size_t operand) {
						return operand < instr.operands.size() &&
							instr.operands[operand].type == IRValue::Type::REGISTER &&
							aliases.count(instr.operands[operand].reg_index()) != 0;
					};
					const auto emit = [&](IRInstruction replacement) {
						replacement.line = instr.line;
						fresh.push_back(std::move(replacement));
					};
					const auto read_field = [&](const std::optional<Key>& key) {
						const auto field = key ? fields.find(*key) : fields.end();
						if (field == fields.end()) {
							return false;
						}
						IRInstruction move(IROpcode::MOVE, instr.operands[0], IRValue::reg(field->second));
						move.type_hint = instr.opcode == IROpcode::DICT_GET_CONST
							? instr.type_hint : IRInstruction::TypeHint_NONE;
						emit(std::move(move));
						return true;
					};
					const auto write_field = [&](const std::optional<Key>& key, const IRValue& value) {
						if (!key) {
							return false;
						}
						auto field = fields.find(*key);
						if (field == fields.end()) {
							if (!in_prefix) {
								return false;
							}
							field = fields.emplace(*key, next_register++).first;
						}
						emit(IRInstruction(IROpcode::MOVE, IRValue::reg(field->second), value));
						return true;
					};
					const auto answer_has = [&](const std::optional<Key>& key) {
						if (!key) {
							return false;
						}
						IRInstruction load(IROpcode::LOAD_BOOL, instr.operands[0],
							IRValue::imm(fields.count(*key) != 0 ? 1 : 0));
						load.type_hint = Variant::BOOL;
						emit(std::move(load));
						return true;
					};

					bool handled = false;
					switch (instr.opcode) {
						case IROpcode::MOVE:
							if (alias_at(1)) {
								const int copy = instr.operands[0].reg_index();
								if (copy == IRFunction::RETURN_REGISTER) {
									return false;
								}
								aliases.insert(copy);
								handled = true;
							}
							break;
						case IROpcode::DICT_GET_CONST:
							handled = alias_at(1) && read_field(string_key(instr.operands[2]));
							break;
						case IROpcode::DICT_SET_CONST:
						case IROpcode::DICT_SET_CONST_STR:
							handled = alias_at(0) && write_field(string_key(instr.operands[1]), instr.operands[2]);
							break;
						case IROpcode::DICT_SET:
							handled = alias_at(0) && write_field(register_key(instr.operands[1]), instr.operands[2]);
							break;
						case IROpcode::DICT_HAS_CONST:
							handled = alias_at(1) && answer_has(string_key(instr.operands[2]));
							break;
						case IROpcode::IN:
							handled = alias_at(2) && answer_has(register_key(instr.operands[1]));
							break;
						case IROpcode::CALL_SYSCALL: {
							if (instr.operands.size() < 4 || !alias_at(3) ||
								instr.operands[1].immediate() != ECALL_DICTIONARY_OPS)
							{
								break;
							}
							const int64_t op = instr.operands[2].immediate();
							if (op == dictionary_op(Dictionary_Op::GET) && instr.operands.size() == 5) {
								handled = read_field(register_key(instr.operands[4]));
							} else if (op == dictionary_op(Dictionary_Op::HAS) && instr.operands.size() == 5) {
								handled = answer_has(register_key(instr.operands[4]));
							} else if (op == dictionary_op(Dictionary_Op::GET_SIZE) && instr.operands.size() == 4) {
								IRInstruction load(IROpcode::LOAD_IMM, instr.operands[0],
									IRValue::imm(int64_t(fields.size())));
								load.type_hint = Variant::INT;
								emit(std::move(load));
								handled = true;
							} else if (op == dictionary_op(Dictionary_Op::GET_OR_DEFAULT) && instr.operands.size() == 6) {
								const std::optional<Key> key = register_key(instr.operands[4]);
								if (key && fields.count(*key) == 0) {
									emit(IRInstruction(IROpcode::MOVE, instr.operands[0], instr.operands[5]));
									handled = true;
								} else {
									handled = read_field(key);
								}
							}
							break;
						}
						default:
							break;
					}
					if (!handled) {
						return false;
					}
					if (instr.opcode != IROpcode::MOVE) {
						for (size_t operand = 0; operand < instr.operands.size(); operand++) {
							if (ir_writes_operand(instr, operand)) {
								aliases.erase(instr.operands[operand].reg_index());
							}
						}
					}
				}
				func.max_registers = std::max(func.max_registers, next_register);
				return true;
			};

			std::vector<IRInstruction> fresh;
			for (bool flow_wide : { true, false }) {
				if (rewrite(flow_wide, fresh)) {
					replace_instructions(func, std::move(fresh));
					changed = replaced = true;
					break;
				}
			}
		}
		if (!replaced) {
			break;
		}
	}
	return changed;
}

bool IROptimizer::eliminate_redundant_stores(IRFunction& func) {
	// Delays pure loads; drops dead (overwritten without read) and identical consecutive ones.
	if (func.instructions.empty()) {
		return false;
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
			int dst = instr.operands[0].reg_index();

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
						is_identical = (curr_val.immediate() ==
						                prev_val.immediate());
					} else if (curr_val.type == IRValue::Type::FLOAT) {
						is_identical = (curr_val.float_number() ==
						                prev_val.float_number());
					} else if (curr_val.type == IRValue::Type::REGISTER) {
						// For MOVE: compare source register
						is_identical = (curr_val.reg_index() ==
						                prev_val.reg_index());
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

	return replace_instructions(func, std::move(new_instructions));
}

std::vector<IROptimizer::LoopInfo> IROptimizer::identify_loops(const IRFunction& func) {
	std::vector<LoopInfo> loops;

	const std::unordered_map<uint32_t, size_t>& label_positions = analysis(func).label_index;

	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];

		if (!ir_is_control_flow(instr.opcode)) continue;
		for (size_t operand = 0; operand < instr.operands.size(); operand++) {
			if (instr.operands[operand].type != IRValue::Type::LABEL) continue;
			const uint32_t target_label = instr.operands[operand].string_id;

			auto it = label_positions.find(target_label);
			if (it != label_positions.end() && it->second <= i) {
				size_t header_idx = it->second;

				uint32_t exit_label = IRStringTable::INVALID_ID;
				for (size_t j = header_idx; j < i; j++) {
					const auto& loop_instr = func.instructions[j];
					if (ir_has_effect(loop_instr.opcode, IR_BRANCH)) {
						// Find the label operand (last operand for all branch types)
						size_t label_idx = loop_instr.operands.size() - 1;
						if (label_idx < loop_instr.operands.size() &&
						    loop_instr.operands[label_idx].type == IRValue::Type::LABEL) {
							exit_label = loop_instr.operands[label_idx].string_id;
							break;
						}
					}
				}

				size_t exit_idx = i + 1;
				if (exit_label != IRStringTable::INVALID_ID) {
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

// A syscall that only reads host state. It cannot change what another query in
// the loop answers, so its presence in the body does not block a hoist.
// Deliberately narrow: ECALL_ARRAY_AT is absent because a negative index turns
// it into a store.
static bool syscall_only_reads(const IRInstruction& instr) {
	if (instr.opcode != IROpcode::CALL_SYSCALL || instr.operands.size() < 2 ||
		instr.operands[1].type != IRValue::Type::IMMEDIATE) {
		return false;
	}
	switch (instr.operands[1].immediate()) {
		case ECALL_ARRAY_SIZE:
		case ECALL_STRING_SIZE:
			return true;
		case ECALL_DICTIONARY_OPS: {
			if (instr.operands.size() < 3 || instr.operands[2].type != IRValue::Type::IMMEDIATE) {
				return false;
			}
			const int64_t op = instr.operands[2].immediate();
			return op == dictionary_op(Dictionary_Op::GET_SIZE) ||
				op == dictionary_op(Dictionary_Op::HAS) ||
				op == dictionary_op(Dictionary_Op::GET) ||
				op == dictionary_op(Dictionary_Op::GET_KEYS) ||
				op == dictionary_op(Dictionary_Op::GET_VALUES);
		}
		default:
			return false;
	}
}

// The subset of the above whose answer arrives in a register. A query that
// creates a scoped variant may not be hoisted: the loop's SCOPE_RELEASE would
// free it out from under the register that now holds it, since the hoist lands
// below the SCOPE_MARK and not above it.
static bool syscall_is_hoistable_query(const IRInstruction& instr) {
	if (!syscall_only_reads(instr)) {
		return false;
	}
	switch (instr.operands[1].immediate()) {
		case ECALL_ARRAY_SIZE:
		case ECALL_STRING_SIZE:
			return true;
		case ECALL_DICTIONARY_OPS: {
			const int64_t op = instr.operands[2].immediate();
			return op == dictionary_op(Dictionary_Op::GET_SIZE) ||
				op == dictionary_op(Dictionary_Op::HAS);
		}
		default:
			return false;
	}
}

bool IROptimizer::loop_only_reads_host(const LoopInfo& loop, const IRFunction& func) {
	for (size_t i = loop.header_idx; i < loop.end_idx && i < func.instructions.size(); i++) {
		const IRInstruction& instr = func.instructions[i];
		// Control flow and scope bookkeeping reach the host but touch no container.
		if (ir_is_control_flow(instr.opcode) || instr.opcode == IROpcode::SCOPE_MARK ||
			instr.opcode == IROpcode::SCOPE_RELEASE || syscall_only_reads(instr)) {
			continue;
		}
		if (!ir_instruction_is_pure(instr)) {
			return false;
		}
	}
	return true;
}

bool IROptimizer::is_loop_invariant(const IRInstruction& instr, const LoopInfo& loop,
                                    const IRFunction& func, const std::unordered_set<int>& invariant_regs,
                                    bool loop_reads_only) {
	// `while i < a.size():` and the size read a container walk makes per pass
	// are the same instruction, and neither has to repeat when the loop cannot
	// reach the container.
	if (loop_reads_only && syscall_is_hoistable_query(instr)) {
		for (size_t i = 2; i < instr.operands.size(); i++) {
			if (instr.operands[i].type != IRValue::Type::REGISTER) {
				continue;
			}
			if (invariant_regs.count(instr.operands[i].reg_index()) == 0) {
				return false;
			}
		}
		return true;
	}

	if (instr.opcode == IROpcode::LOAD_IMM ||
	    instr.opcode == IROpcode::LOAD_FLOAT_IMM ||
	    instr.opcode == IROpcode::LOAD_BOOL ||
	    instr.opcode == IROpcode::LOAD_STRING) {
		return true;
	}

	if (instr.opcode == IROpcode::LOAD_GLOBAL && instr.operands.size() >= 2 &&
		instr.operands[1].type == IRValue::Type::IMMEDIATE)
	{
		const int64_t global = instr.operands[1].immediate();
		for (size_t i = loop.header_idx; i < loop.end_idx && i < func.instructions.size(); i++) {
			const IRInstruction& body = func.instructions[i];
			if (ir_has_effect(body.opcode, IR_CALL)) {
				return false;
			}
			if (body.opcode == IROpcode::STORE_GLOBAL && body.operands.size() >= 1 &&
				body.operands[0].type == IRValue::Type::IMMEDIATE &&
				body.operands[0].immediate() == global)
			{
				return false;
			}
		}
		return true;
	}

	if (instr.opcode == IROpcode::MOVE && instr.operands.size() >= 2 &&
	    instr.operands[1].type == IRValue::Type::REGISTER) {
		int src_reg = instr.operands[1].reg_index();
		return invariant_regs.count(src_reg) > 0;
	}

	const bool builds_from_inputs = instr.opcode == IROpcode::VGET_INLINE ||
		instr.opcode == IROpcode::MAKE_VECTOR2 || instr.opcode == IROpcode::MAKE_VECTOR3 ||
		instr.opcode == IROpcode::MAKE_VECTOR4 || instr.opcode == IROpcode::MAKE_VECTOR2I ||
		instr.opcode == IROpcode::MAKE_VECTOR3I || instr.opcode == IROpcode::MAKE_VECTOR4I;
	if (builds_from_inputs) {
		for (size_t i = 0; i < instr.operands.size(); i++) {
			if (!ir_reads_operand(instr, i)) {
				continue;
			}
			if (invariant_regs.count(instr.operands[i].reg_index()) == 0) {
				return false;
			}
		}
		return true;
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
	const std::unordered_map<uint32_t, size_t>& label_positions = analysis(func).label_index;

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
		auto it = label_positions.find(target.string_id);
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

bool IROptimizer::loop_invariant_code_motion(IRFunction& func) {
	bool changed = false;
	auto loops = identify_loops(func);

	if (loops.empty()) {
		return false;
	}

	for (const auto& loop : loops) {
		// The inner loop has its own preheader and control flow.  Leave that loop
		// alone, but do not disable LICM for every other loop in the function.
		bool nested = false;
		for (const auto& other : loops) {
			if (&loop != &other && loop.header_idx > other.header_idx &&
				loop.header_idx < other.end_idx)
			{
				nested = true;
				break;
			}
		}
		if (nested) {
			continue;
		}
		const bool loop_reads_only = loop_only_reads_host(loop, func);

		// Seed with registers defined before the loop. Parameters arrive in
		// r0..rN-1 with no instruction defining them, so they are seeded too;
		// one reassigned inside the loop is caught by can_safely_hoist().
		std::unordered_set<int> invariant_regs;
		for (size_t i = 0; i < func.parameters.size(); i++) {
			invariant_regs.insert(int(i));
		}

		for (size_t i = 0; i < loop.header_idx; i++) {
			const int dst = ir_destination_register(func.instructions[i]);
			if (dst >= 0) {
				invariant_regs.insert(dst);
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

				if (is_loop_invariant(instr, loop, func, invariant_regs, loop_reads_only) &&
				    can_safely_hoist(instr, i, loop, func)) {
					invariant_instrs.insert(i);

					const int dst = ir_destination_register(instr);
					if (dst >= 0) invariant_regs.insert(dst);

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
				    remaining[i].operands[0].string_id == loop.header_label) {
					new_instructions.insert(new_instructions.end(), hoisted.begin(), hoisted.end());
				}
				new_instructions.push_back(remaining[i]);
			}

			changed = replace_instructions(func, std::move(new_instructions)) || changed;
		}
	}
	return changed;
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

bool IROptimizer::enhanced_copy_propagation(IRFunction& func) {
	std::unordered_map<int, CopyInfo> copies;
	std::vector<IRInstruction> new_instructions;
	new_instructions.reserve(func.instructions.size());

	for (size_t i = 0; i < func.instructions.size(); i++) {
		auto instr = func.instructions[i];  // Make a copy we can modify

		// Deferred clear: reads happen before the effect, so propagate into this
		// instruction's operands first. Otherwise branches never see copies.
		// A host call cannot mutate an IR vreg other than its explicit
		// destination.  Keep copies across syscalls and ordinary side effects;
		// only operations with implicit writes (or a host re-entry) invalidate
		// the local value relation.
		const bool clear_after = instr.opcode == IROpcode::CALL ||
			instr.opcode == IROpcode::AWAIT || instr.opcode == IROpcode::SCOPE_RELEASE;

		// ir_reads_operand handles branches (read at 0) and CALL (write at 1).
		for (size_t j = 0; j < instr.operands.size(); j++) {
			if (!ir_reads_operand(instr, j)) {
				continue;
			}
			// INOUT: substitution would redirect the write.
			if (ir_writes_operand(instr, j)) {
				continue;
			}
			int reg = instr.operands[j].reg_index();

			if (copies.count(reg)) {
				const auto& copy_info = copies[reg];

				if (register_unmodified_between(func, copy_info.source_reg, copy_info.def_idx, i)) {
					instr.operands[j].reg_value = copy_info.source_reg;
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
			const int dst = instr.operands[0].reg_index();
			const int src = instr.operands[1].reg_index();
			if (dst != src) {
				copies[dst] = {src, i};
			}
		}

		new_instructions.push_back(instr);
	}

	return replace_instructions(func, std::move(new_instructions));
}


// Fuse d[k] op= v (GET + operator + SET) into one DICT_OPERATE when both
// intermediates are single-use and the window between is host-quiet.
// Division and shifts excluded: guest-side rules differ from Variant::evaluate.
bool IROptimizer::fuse_dictionary_updates(IRFunction& func) {
	std::vector<IRInstruction>& code = func.instructions;
	const int params = int(func.parameters.size());
	int nregs = std::max(func.max_registers, params);
	for (const IRInstruction& instr : code) {
		for (const IRValue& operand : instr.operands) {
			if (operand.type == IRValue::Type::REGISTER) {
				nregs = std::max(nregs, operand.reg_index() + 1);
			}
		}
	}
	std::vector<int> def_count(size_t(nregs), 0);
	std::vector<int> read_count(size_t(nregs), 0);
	std::vector<int> reads;
	for (int reg = 0; reg < params; reg++) {
		def_count[size_t(reg)] = 1;
	}
	for (size_t i = 0; i < code.size(); i++) {
		for (size_t operand = 0; operand < code[i].operands.size(); operand++) {
			if (ir_writes_operand(code[i], operand)) {
				const int reg = code[i].operands[operand].reg_index();
				def_count[size_t(reg)]++;
			}
		}
		reads.clear();
		ir_collect_read_registers(code[i], reads);
		for (int reg : reads) {
			read_count[size_t(reg)]++;
		}
	}
	const auto written_between = [&](int reg, size_t first, size_t last) {
		for (size_t i = first; i <= last && i < code.size(); i++) {
			for (size_t operand = 0; operand < code[i].operands.size(); operand++) {
				if (ir_writes_operand(code[i], operand) && code[i].operands[operand].reg_index() == reg) {
					return true;
				}
			}
		}
		return false;
	};
	const auto string_of = [&](const IRValue& index) -> const std::string* {
		if (m_string_constants == nullptr || index.type != IRValue::Type::IMMEDIATE ||
			index.immediate() < 0 || size_t(index.immediate()) >= m_string_constants->size())
		{
			return nullptr;
		}
		return &(*m_string_constants)[size_t(index.immediate())];
	};
	// Nothing between the read and write may reach the Dictionary.
	const auto quiet = [](const IRInstruction& instr) {
		const auto numeric = [](IRInstruction::TypeHint type) {
			return type == Variant::INT || type == Variant::FLOAT;
		};
		switch (instr.opcode) {
			case IROpcode::GLOBAL_CALL:
			case IROpcode::CONSTRUCT:
			case IROpcode::CONVERT:
			case IROpcode::IN:
				return false;
			case IROpcode::MOD:
				// String % Object calls the object's _to_string().
				return numeric(instr.lhs_type_hint) && numeric(instr.rhs_type_hint);
			default:
				return !ir_is_control_flow(instr.opcode) && ir_instruction_is_pure(instr);
		}
	};

	size_t moving_read = SIZE_MAX;
	const auto local_def = [&](int reg, size_t at) -> int {
		for (size_t i = at; i-- > 0;) {
			for (size_t operand = 0; operand < code[i].operands.size(); operand++) {
				if (ir_writes_operand(code[i], operand) && code[i].operands[operand].reg_index() == reg) {
					return int(i);
				}
			}
			if (ir_is_control_flow(code[i].opcode)) {
				return -1;
			}
		}
		return -1;
	};
	// Matches the read's key against the write's, which the parser may have
	// cloned as a second computation of the same pure expression.
	const std::function<bool(int, size_t, int, size_t, int)> same_value =
		[&](int a, size_t at_a, int b, size_t at_b, int depth) -> bool {
		if (a == b && !written_between(a, std::min(at_a, at_b), std::max(at_a, at_b) - 1)) {
			return true;
		}
		const int def_a = local_def(a, at_a);
		const int def_b = local_def(b, at_b);
		if (def_a < 0 || def_b < 0) {
			return false;
		}
		const IRInstruction& da = code[size_t(def_a)];
		const IRInstruction& db = code[size_t(def_b)];
		if (da.opcode != db.opcode || da.type_hint != db.type_hint ||
			da.lhs_type_hint != db.lhs_type_hint || da.rhs_type_hint != db.rhs_type_hint ||
			da.operands.size() != db.operands.size())
		{
			return false;
		}
		switch (da.opcode) {
			case IROpcode::LOAD_IMM:
				return da.operands[1].immediate() == db.operands[1].immediate();
			case IROpcode::LOAD_STRING: {
				const std::string* sa = string_of(da.operands[1]);
				const std::string* sb = string_of(db.operands[1]);
				return sa != nullptr && sb != nullptr && *sa == *sb;
			}
			case IROpcode::LOAD_GLOBAL: {
				if (da.operands[1].immediate() != db.operands[1].immediate()) {
					return false;
				}
				// Nothing between the two loads may store to the member.
				const size_t first = size_t(std::min(def_a, def_b));
				const size_t last = size_t(std::max(def_a, def_b));
				for (size_t i = first + 1; i < last; i++) {
					if (i != moving_read && !quiet(code[i])) {
						return false;
					}
				}
				return true;
			}
			default:
				break;
		}
		if (depth == 0 || !ir_has_effect(da.opcode, IR_ARITHMETIC) || !quiet(da)) {
			return false;
		}
		for (size_t operand = 1; operand < da.operands.size(); operand++) {
			const IRValue& x = da.operands[operand];
			const IRValue& y = db.operands[operand];
			if (x.type != y.type) {
				return false;
			}
			if (x.type == IRValue::Type::REGISTER) {
				if (!same_value(x.reg_index(), size_t(def_a), y.reg_index(), size_t(def_b), depth - 1)) {
					return false;
				}
			} else if (x.type != IRValue::Type::IMMEDIATE || x.immediate() != y.immediate()) {
				return false;
			}
		}
		return true;
	};

	// Fusions are collected and applied together. Each removes reads and defs
	// only of its own single-use registers, so the counts above stay valid
	// (or conservative) for every later candidate.
	std::vector<bool> dropped(code.size(), false);
	std::vector<std::pair<size_t, IRInstruction>> fusions;
	for (size_t get_at = 0; get_at < code.size(); get_at++) {
		const IRInstruction& get = code[get_at];
		moving_read = get_at;
		const bool get_const = get.opcode == IROpcode::DICT_GET_CONST && get.operands.size() == 3;
		const bool get_keyed = get.opcode == IROpcode::CALL_SYSCALL && get.operands.size() == 5 &&
			get.operands[1].immediate() == ECALL_DICTIONARY_OPS &&
			get.operands[2].immediate() == dictionary_op(Dictionary_Op::GET);
		if (!get_const && !get_keyed) {
			continue;
		}
		const int old_value = get.operands[0].reg_index();
		const int dict = get.operands[get_const ? 1 : 3].reg_index();
		if (old_value < params || def_count[size_t(old_value)] != 1 ||
			read_count[size_t(old_value)] != 1 || old_value == dict)
		{
			continue;
		}

		// Straight-line from the read to the write, touching nothing on the host.
		size_t op_at = 0;
		size_t set_at = 0;
		for (size_t i = get_at + 1; i < code.size(); i++) {
			const IRInstruction& instr = code[i];
			if (op_at == 0) {
				reads.clear();
				ir_collect_read_registers(instr, reads);
				if (std::find(reads.begin(), reads.end(), old_value) != reads.end()) {
					op_at = i;
					continue;
				}
			} else if ((instr.opcode == IROpcode::DICT_SET || instr.opcode == IROpcode::DICT_SET_CONST ||
				instr.opcode == IROpcode::DICT_SET_CONST_STR) &&
				instr.operands[2].type == IRValue::Type::REGISTER &&
				instr.operands[2].reg_index() == code[op_at].operands[0].reg_index())
			{
				set_at = i;
				break;
			}
			if (!quiet(instr)) {
				break;
			}
		}
		if (op_at == 0 || set_at == 0) {
			continue;
		}

		const IRInstruction& op = code[op_at];
		const int variant_op = ir_dictionary_operate_operator(op.opcode);
		if (variant_op < 0 || op.operands.size() != 3 ||
			op.operands[1].type != IRValue::Type::REGISTER || op.operands[1].reg_index() != old_value ||
			op.operands[2].type != IRValue::Type::REGISTER)
		{
			continue;
		}
		const int answer = op.operands[0].reg_index();
		const int operand = op.operands[2].reg_index();
		if (answer < params || def_count[size_t(answer)] != 1 || read_count[size_t(answer)] != 1 ||
			operand == old_value || operand == answer || written_between(operand, op_at + 1, set_at))
		{
			continue;
		}

		const IRInstruction& set = code[set_at];
		if (set.operands[0].type != IRValue::Type::REGISTER ||
			!same_value(dict, get_at, set.operands[0].reg_index(), set_at, 1))
		{
			continue;
		}
		const bool keyed_write = set.opcode == IROpcode::DICT_SET;
		if (keyed_write) {
			if (!get_keyed || set.operands[1].type != IRValue::Type::REGISTER ||
				!same_value(get.operands[4].reg_index(), get_at, set.operands[1].reg_index(),
					set_at, 3))
			{
				continue;
			}
		} else {
			const std::string* read_key = get_const ? string_of(get.operands[2]) : nullptr;
			const std::string* write_key = string_of(set.operands[1]);
			if (read_key == nullptr || write_key == nullptr || *read_key != *write_key) {
				continue;
			}
		}
		IRInstruction fused(keyed_write ? IROpcode::DICT_OPERATE : IROpcode::DICT_OPERATE_CONST);
		fused.operands.push_back(set.operands[0]);
		fused.operands.push_back(set.operands[1]);
		fused.operands.push_back(IRValue::reg(operand));
		fused.operands.push_back(IRValue::imm(variant_op));
		if (!keyed_write) {
			fused.operands.push_back(IRValue::imm(set.opcode == IROpcode::DICT_SET_CONST_STR ? 1 : 0));
		}
		fused.line = set.line;

		dropped[get_at] = true;
		dropped[op_at] = true;
		fusions.emplace_back(set_at, std::move(fused));
		// The window is consumed; anything inside it waits for the next run.
		get_at = set_at;
	}
	if (fusions.empty()) {
		return false;
	}
	std::vector<IRInstruction> fresh;
	fresh.reserve(code.size() - 2 * fusions.size());
	size_t next_fusion = 0;
	for (size_t i = 0; i < code.size(); i++) {
		if (dropped[i]) {
			continue;
		}
		if (next_fusion < fusions.size() && fusions[next_fusion].first == i) {
			fresh.push_back(std::move(fusions[next_fusion++].second));
		} else {
			fresh.push_back(std::move(code[i]));
		}
	}
	return replace_instructions(func, std::move(fresh));
}

// Lazy strings: a str() whose result never escapes is described by its
// pieces. length() is answered from those pieces in the guest (literal
// lengths fold, int pieces use DECIMAL_LENGTH, String pieces ask the host).
// A lone escape past a branch sinks the str() call there; multiple escapes
// keep it in place. The call is never moved into a loop it was not in.
namespace {

enum class PieceKind : uint8_t { UNKNOWN, INT, STRING, LITERAL };

struct LazyPiece {
	PieceKind kind = PieceKind::UNKNOWN;
	int reg = -1;
	int64_t literal_index = -1; // LITERAL: IRProgram::string_constants index
	int64_t literal_length = 0;
};

// Type hints here describe the result; a comparison's describes its operands.
PieceKind defined_kind(const IRInstruction& instr) {
	switch (instr.opcode) {
		case IROpcode::LOAD_IMM:
		case IROpcode::TYPE_OF:
		case IROpcode::DECIMAL_LENGTH:
			return PieceKind::INT;
		case IROpcode::LOAD_STRING:
			return PieceKind::STRING;
		case IROpcode::COERCE:
		case IROpcode::CONVERT:
			if (instr.type_hint == Variant::INT) return PieceKind::INT;
			if (instr.type_hint == Variant::STRING) return PieceKind::STRING;
			return PieceKind::UNKNOWN;
		case IROpcode::ADD:
			// String + String concatenates.
			if (instr.type_hint == Variant::STRING) return PieceKind::STRING;
			[[fallthrough]];
		case IROpcode::SUB:
		case IROpcode::MUL:
		case IROpcode::DIV:
		case IROpcode::MOD:
		case IROpcode::NEG:
		case IROpcode::BIT_AND:
		case IROpcode::BIT_OR:
		case IROpcode::BIT_XOR:
		case IROpcode::BIT_NOT:
		case IROpcode::SHL:
		case IROpcode::SHR:
			return instr.type_hint == Variant::INT ? PieceKind::INT : PieceKind::UNKNOWN;
		case IROpcode::CALL_SYSCALL:
			if (instr.operands.size() == 3 && instr.type_hint == Variant::INT &&
				instr.operands[1].type == IRValue::Type::IMMEDIATE &&
				(instr.operands[1].immediate() == ECALL_STRING_SIZE ||
				 instr.operands[1].immediate() == ECALL_ARRAY_SIZE))
			{
				return PieceKind::INT;
			}
			return PieceKind::UNKNOWN;
		case IROpcode::GLOBAL_CALL:
			return instr.operands.size() >= 2 &&
				instr.operands[1].immediate() == static_cast<int64_t>(GlobalFn::STR)
				? PieceKind::STRING : PieceKind::UNKNOWN;
		default:
			return PieceKind::UNKNOWN;
	}
}

bool is_str_call(const IRInstruction& instr) {
	if (instr.opcode != IROpcode::GLOBAL_CALL || instr.operands.size() < 5 ||
		instr.operands[1].type != IRValue::Type::IMMEDIATE ||
		instr.operands[1].immediate() != static_cast<int64_t>(GlobalFn::STR) ||
		instr.operands[3].type != IRValue::Type::IMMEDIATE ||
		instr.operands.size() != 4 + size_t(instr.operands[3].immediate()))
	{
		return false;
	}
	for (size_t i = 4; i < instr.operands.size(); i++) {
		if (instr.operands[i].type != IRValue::Type::REGISTER) return false;
	}
	return true;
}

bool is_string_size_of(const IRInstruction& instr, int reg) {
	return instr.opcode == IROpcode::CALL_SYSCALL && instr.operands.size() == 3 &&
		instr.operands[1].type == IRValue::Type::IMMEDIATE &&
		instr.operands[1].immediate() == ECALL_STRING_SIZE &&
		instr.operands[2].type == IRValue::Type::REGISTER &&
		instr.operands[2].reg_index() == reg;
}

} // namespace

bool IROptimizer::sink_lazy_strings(IRFunction& func) {
	// A coroutine resumes in the middle of its body; the reachability below
	// only knows the one entry.
	if (func.is_coroutine) {
		return false;
	}
	// One str() per round: rewriting one changes what the next can see.
	bool changed = false;
	for (int round = 0; round < 256 && sink_one_lazy_string(func); round++) {
		changed = true;
	}
	if (changed) {
		invalidate_analysis();
	}
	return changed;
}

bool IROptimizer::sink_one_lazy_string(IRFunction& func) {
	std::vector<IRInstruction>& code = func.instructions;
	const size_t count = code.size();
	if (count == 0) {
		return false;
	}

	const size_t params = func.parameters.size();
	int max_reg = int(params) - 1;
	for (const IRInstruction& instr : code) {
		for (const IRValue& operand : instr.operands) {
			if (operand.type == IRValue::Type::REGISTER) {
				max_reg = std::max(max_reg, operand.reg_index());
			}
		}
	}
	if (max_reg < 0) {
		return false;
	}
	const size_t nregs = size_t(max_reg) + 1;

	std::vector<std::vector<size_t>> defs(nregs);
	std::vector<std::vector<size_t>> uses(nregs);
	std::vector<int> reads;
	for (size_t i = 0; i < count; i++) {
		const IRInstruction& instr = code[i];
		for (size_t operand = 0; operand < instr.operands.size(); operand++) {
			if (instr.operands[operand].type == IRValue::Type::REGISTER &&
				ir_writes_operand(instr, operand))
			{
				defs[size_t(instr.operands[operand].reg_index())].push_back(i);
			}
		}
		reads.clear();
		ir_collect_read_registers(instr, reads);
		std::sort(reads.begin(), reads.end());
		reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
		for (int reg : reads) {
			if (reg >= 0 && size_t(reg) < nregs) uses[size_t(reg)].push_back(i);
		}
	}
	const auto single_def = [&](int reg) {
		return reg >= int(params) && defs[size_t(reg)].size() == 1;
	};

	// Every definition reaching `reg` through MOVEs has to agree. Not memoized:
	// a cycle of MOVEs answers optimistically while it is being walked.
	const auto value_kind = [&](int root) {
		std::vector<bool> seen(nregs, false);
		std::vector<int> work { root };
		PieceKind kind = PieceKind::UNKNOWN;
		bool any = false;
		while (!work.empty()) {
			const int reg = work.back();
			work.pop_back();
			if (seen[size_t(reg)]) continue;
			seen[size_t(reg)] = true;
			if (reg < int(params) || defs[size_t(reg)].empty()) {
				return PieceKind::UNKNOWN;
			}
			for (size_t at : defs[size_t(reg)]) {
				const IRInstruction& def = code[at];
				if (def.opcode == IROpcode::MOVE && def.operands[1].type == IRValue::Type::REGISTER) {
					work.push_back(def.operands[1].reg_index());
					continue;
				}
				const PieceKind here = defined_kind(def);
				if (here == PieceKind::UNKNOWN || (any && here != kind)) {
					return PieceKind::UNKNOWN;
				}
				kind = here;
				any = true;
			}
		}
		return any ? kind : PieceKind::UNKNOWN;
	};

	const auto& labels = analysis(func).label_index;
	const auto successors = [&](size_t i, std::vector<size_t>& out) {
		out.clear();
		const IRInstruction& instr = code[i];
		const bool branch = ir_has_effect(instr.opcode, IR_BRANCH);
		if (branch || ir_has_effect(instr.opcode, IR_TERMINATOR)) {
			for (const IRValue& operand : instr.operands) {
				if (operand.type != IRValue::Type::LABEL) continue;
				auto it = labels.find(operand.string_id);
				if (it != labels.end()) out.push_back(it->second);
			}
		}
		if (!ir_has_effect(instr.opcode, IR_TERMINATOR) && i + 1 < count) {
			out.push_back(i + 1);
		}
	};
	// Instructions reachable from the entry without passing `blocker`.
	const auto reachable_avoiding = [&](size_t blocker) {
		std::vector<bool> seen(count, false);
		std::vector<size_t> work { 0 };
		std::vector<size_t> next;
		while (!work.empty()) {
			const size_t i = work.back();
			work.pop_back();
			if (i == blocker || seen[i]) continue;
			seen[i] = true;
			successors(i, next);
			work.insert(work.end(), next.begin(), next.end());
		}
		return seen;
	};

	for (size_t k = 0; k < count; k++) {
		const IRInstruction& call = code[k];
		if (!is_str_call(call)) continue;
		const int result = call.operands[0].reg_index();
		if (!single_def(result)) continue;

		std::vector<LazyPiece> pieces;
		bool known = true;
		for (size_t i = 4; i < call.operands.size() && known; i++) {
			LazyPiece piece;
			piece.reg = call.operands[i].reg_index();
			if (single_def(piece.reg) && m_string_constants != nullptr) {
				const IRInstruction& def = code[defs[size_t(piece.reg)][0]];
				if (def.opcode == IROpcode::LOAD_STRING &&
					size_t(def.operands[1].immediate()) < m_string_constants->size())
				{
					const std::string& text = (*m_string_constants)[size_t(def.operands[1].immediate())];
					// ASCII: one byte, one character. Anything else asks the host.
					if (std::all_of(text.begin(), text.end(),
						[](char c) { return static_cast<unsigned char>(c) < 0x80; }))
					{
						piece.kind = PieceKind::LITERAL;
						piece.literal_index = def.operands[1].immediate();
						piece.literal_length = int64_t(text.size());
					}
				}
			}
			if (piece.kind == PieceKind::UNKNOWN) {
				piece.kind = value_kind(piece.reg);
			}
			known = piece.kind != PieceKind::UNKNOWN;
			pieces.push_back(piece);
		}
		if (!known) continue;

		// The String's names: its register and every single-definition copy.
		std::vector<int> names { result };
		std::vector<size_t> alias_moves;
		for (size_t n = 0; n < names.size(); n++) {
			for (size_t at : uses[size_t(names[n])]) {
				const IRInstruction& use = code[at];
				if (use.opcode != IROpcode::MOVE || use.operands[1].reg_index() != names[n]) continue;
				const int copy = use.operands[0].reg_index();
				if (copy != names[n] && single_def(copy)) {
					names.push_back(copy);
					alias_moves.push_back(at);
				}
			}
		}

		struct Use { size_t at; int reg; };
		std::vector<Use> lengths;
		std::vector<Use> escapes;
		for (int name : names) {
			for (size_t at : uses[size_t(name)]) {
				if (std::find(alias_moves.begin(), alias_moves.end(), at) != alias_moves.end() &&
					code[at].operands[1].reg_index() == name)
				{
					continue;
				}
				(is_string_size_of(code[at], name) ? lengths : escapes).push_back({ at, name });
			}
		}
		if (lengths.empty() && escapes.empty()) continue; // dead-code's to delete

		// Every use must be dominated by the call.
		const std::vector<bool> without_call = reachable_avoiding(k);
		bool dominated = true;
		for (const auto* list : { &lengths, &escapes }) {
			for (const Use& use : *list) dominated = dominated && !without_call[use.at];
		}
		for (size_t at : alias_moves) dominated = dominated && !without_call[at];
		if (!dominated) continue;

		bool sink = false;
		if (escapes.size() == 1 && escapes[0].at > k) {
			const size_t at = escapes[0].at;
			bool only_ints = true;
			for (const LazyPiece& piece : pieces) {
				// Copying a String piece extends its scoped lifetime.
				only_ints = only_ints && piece.kind != PieceKind::STRING;
			}
			bool past_branch = false;
			for (size_t i = k + 1; i < at && !past_branch; i++) {
				past_branch = ir_is_control_flow(code[i].opcode);
			}
			bool same_loops = true;
			for (const LoopInfo& loop : identify_loops(func)) {
				size_t last = loop.header_idx;
				for (size_t edge : loop.back_edges) last = std::max(last, edge);
				const bool has_use = at >= loop.header_idx && at <= last;
				const bool has_call = k >= loop.header_idx && k <= last;
				same_loops = same_loops && (!has_use || has_call);
			}
			sink = only_ints && past_branch && same_loops;
		}
		const bool keep_call = !escapes.empty() && !sink;
		if (keep_call && lengths.empty()) continue;

		int next_reg = std::max(int(nregs), func.max_registers);
		const auto fresh = [&next_reg]() { return next_reg++; };
		const auto made = [&call](IRInstruction instr) {
			instr.line = call.line;
			instr.debug_order = 0;
			return instr;
		};

		// length(), computed where the String was made.
		std::vector<IRInstruction> at_call;
		int length_reg = -1;
		if (!lengths.empty()) {
			int64_t literal_total = 0;
			std::vector<int> parts;
			for (const LazyPiece& piece : pieces) {
				if (piece.kind == PieceKind::LITERAL) {
					literal_total += piece.literal_length;
					continue;
				}
				const int part = fresh();
				IRInstruction instr = piece.kind == PieceKind::INT
					? IRInstruction(IROpcode::DECIMAL_LENGTH, IRValue::reg(part), IRValue::reg(piece.reg))
					: IRInstruction(IROpcode::CALL_SYSCALL, IRValue::reg(part),
						IRValue::imm(ECALL_STRING_SIZE), IRValue::reg(piece.reg));
				instr.type_hint = Variant::INT;
				at_call.push_back(made(std::move(instr)));
				parts.push_back(part);
			}
			size_t first = 0;
			if (parts.empty() || literal_total != 0) {
				length_reg = fresh();
				IRInstruction load(IROpcode::LOAD_IMM, IRValue::reg(length_reg), IRValue::imm(literal_total));
				load.type_hint = Variant::INT;
				at_call.push_back(made(std::move(load)));
			} else {
				length_reg = parts[0];
				first = 1;
			}
			for (size_t i = first; i < parts.size(); i++) {
				const int sum = fresh();
				IRInstruction add(IROpcode::ADD, IRValue::reg(sum), IRValue::reg(length_reg), IRValue::reg(parts[i]));
				add.type_hint = Variant::INT;
				at_call.push_back(made(std::move(add)));
				length_reg = sum;
			}
		}

		// The int pieces as they were at the call, for a str() made later.
		std::vector<IRInstruction> at_escape;
		if (sink) {
			std::vector<IRValue> arguments;
			for (const LazyPiece& piece : pieces) {
				const int copy = fresh();
				if (piece.kind == PieceKind::LITERAL) {
					IRInstruction load(IROpcode::LOAD_STRING, IRValue::reg(copy), IRValue::imm(piece.literal_index));
					load.type_hint = Variant::STRING;
					load.line = code[escapes[0].at].line;
					at_escape.push_back(std::move(load));
				} else {
					at_call.push_back(made(IRInstruction(IROpcode::MOVE, IRValue::reg(copy), IRValue::reg(piece.reg))));
				}
				arguments.push_back(IRValue::reg(copy));
			}
			IRInstruction remade = call;
			remade.operands[0] = IRValue::reg(escapes[0].reg);
			for (size_t i = 0; i < arguments.size(); i++) {
				remade.operands[4 + i] = arguments[i];
			}
			remade.line = code[escapes[0].at].line;
			remade.debug_order = 0;
			at_escape.push_back(std::move(remade));
		}

		std::vector<IRInstruction> rewritten;
		rewritten.reserve(count + at_call.size() + at_escape.size());
		for (size_t i = 0; i < count; i++) {
			if (i == k) {
				rewritten.insert(rewritten.end(), at_call.begin(), at_call.end());
				if (keep_call) rewritten.push_back(code[i]);
				continue;
			}
			if (!keep_call && std::find(alias_moves.begin(), alias_moves.end(), i) != alias_moves.end()) {
				continue;
			}
			if (sink && i == escapes[0].at) {
				rewritten.insert(rewritten.end(), at_escape.begin(), at_escape.end());
			}
			const auto length = std::find_if(lengths.begin(), lengths.end(),
				[i](const Use& use) { return use.at == i; });
			if (length != lengths.end()) {
				IRInstruction move(IROpcode::MOVE, code[i].operands[0], IRValue::reg(length_reg));
				move.line = code[i].line;
				move.debug_order = code[i].debug_order;
				rewritten.push_back(std::move(move));
				continue;
			}
			rewritten.push_back(code[i]);
		}
		func.instructions = std::move(rewritten);
		func.max_registers = std::max(func.max_registers, next_reg);
		invalidate_analysis();
		return true;
	}
	return false;
}

} // namespace gdscript
