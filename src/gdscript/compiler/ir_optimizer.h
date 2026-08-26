#pragma once
#include "ir.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gdscript {

class IROptimizer;

// Listed so test_opt_invariance can run a prefix to bisect a miscompile.
struct IRPass {
	const char* name;
	void (IROptimizer::*run)(IRFunction&);
};

class IROptimizer {
public:
	IROptimizer();

	void optimize(IRProgram& program);
	void optimize_function(IRFunction& func);

	// A pass name may appear more than once (peephole runs three times).
	static const std::vector<IRPass>& pipeline();

	void set_pass_limit(size_t count) { m_pass_limit = count; }
	void set_enabled_passes(const std::vector<std::string>& names);
	// max_registers recomputation still runs even with all passes disabled.
	void disable_all_passes() { set_enabled_passes({"none"}); }

private:
	void constant_folding(IRFunction& func);
	void eliminate_unreachable_code(IRFunction& func);
	void eliminate_dead_code(IRFunction& func);
	void tighten_scope_marks(IRFunction& func);
	void peephole_optimization(IRFunction& func);

	// On match: appends replacement, advances i, returns true. On miss: no side effects.
	using PeepholePattern = bool (IROptimizer::*)(const IRFunction& func, size_t& i,
		std::vector<IRInstruction>& new_instructions);

	bool try_fuse_compare_and_branch(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions);
	bool try_eliminate_moves_around_op(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions);
	bool try_eliminate_move_pair(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions);
	bool try_fold_move_after_op(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions);
	bool try_remove_branch_to_next(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions);
	void copy_propagation(IRFunction& func);
	void eliminate_redundant_stores(IRFunction& func);
	void reduce_register_pressure(IRFunction& func);
	void loop_invariant_code_motion(IRFunction& func);
	void enhanced_copy_propagation(IRFunction& func);

	struct ConstantValue {
		enum class Type { NONE, INT, FLOAT, BOOL, STRING };
		Type type = Type::NONE;
		int64_t int_value = 0;
		double float_value = 0.0;
		bool bool_value = false;
		std::string string_value;

		bool is_constant() const { return type != Type::NONE; }

		// Bitwise compare for doubles: NaN == NaN, -0.0 != 0.0. Prevents misfolding at joins.
		bool same_as(const ConstantValue& other) const;

		// Variant::booleanize() for tracked types. Returns false when undecidable.
		bool truthiness(bool& truth) const;
	};

	using ConstantMap = std::unordered_map<int, ConstantValue>;
	ConstantMap m_constants;

	// Forward dataflow block. Clearing state at every LABEL kills constants across any `if`.
	struct ConstBlock {
		size_t begin = 0;
		size_t end = 0;
		std::vector<size_t> successors;
		ConstantMap entry;
		bool entry_initialized = false;   // false: unreachable
	};

	static std::vector<ConstBlock> build_blocks(const IRFunction& func);

	// Intersect: keeps only registers both agree on. Returns true if anything was dropped.
	static bool meet_constants(ConstantMap& into, const ConstantMap& from);

	// Shared by analysis (out==nullptr) and rewrite (out!=nullptr) to prevent drift.
	void fold_instruction(const IRInstruction& instr, std::vector<IRInstruction>* out);

	ConstantValue get_constant(const IRValue& val);
	bool try_fold_binary_op(IROpcode op, IRInstruction::TypeHint type_hint, const ConstantValue& lhs, const ConstantValue& rhs, ConstantValue& result);
	void set_register_constant(int reg, const ConstantValue& value);
	void invalidate_register(int reg);

	std::unordered_set<int> find_live_registers(const IRFunction& func);
	bool is_register_used_after(const IRFunction& func, int reg, size_t instr_idx);

	// Opcode classification comes from ir_opcodes.def, not a list here.
	static bool is_reg_used_between_exclusive(const IRFunction& func, int reg, size_t start_idx, size_t end_idx, bool conservative_at_labels = true);

	// Whole-function liveness, not windowed: MOVE-folding deletes definitions,
	// so the temp must be dead everywhere, not just in the pattern's window.
	static bool is_reg_read_outside(const IRFunction& func, int reg, size_t first, size_t last);

	struct LoopInfo {
		size_t header_idx;
		size_t end_idx;
		std::string header_label;
		std::string end_label;
		std::vector<size_t> back_edges;
	};
	std::vector<LoopInfo> identify_loops(const IRFunction& func);
	bool is_loop_invariant(const IRInstruction& instr, const LoopInfo& loop,
	                       const IRFunction& func, const std::unordered_set<int>& invariant_regs,
	                       bool loop_reads_only);
	bool can_safely_hoist(const IRInstruction& instr, size_t instr_idx, const LoopInfo& loop, const IRFunction& func);
	// True when no instruction in the loop can change what a container query
	// answers. Aliasing is not tracked, so any host write at all disqualifies it.
	static bool loop_only_reads_host(const LoopInfo& loop, const IRFunction& func);
	// False if the instruction sits inside an `if` within the loop body.
	static bool is_unconditional_in_loop(size_t instr_idx, const LoopInfo& loop, const IRFunction& func);

	struct CopyInfo {
		int source_reg;
		size_t def_idx;
	};
	bool register_unmodified_between(const IRFunction& func, int reg, size_t start_idx, size_t end_idx);

	// Empty m_enabled_passes means "every pass".
	size_t m_pass_limit = SIZE_MAX;
	std::unordered_set<std::string> m_enabled_passes;
	bool is_pass_enabled(const char* name) const;
};

} // namespace gdscript
