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
	// Answers whether it changed func; drives the repeat-pass skip.
	bool (IROptimizer::*run)(IRFunction&);
};

class IROptimizer {
public:
	IROptimizer();

	void optimize(IRProgram& program);
	void optimize_function(IRFunction& func);

	// Verifier diagnostics; unset when a test optimizes a bare IRFunction.
	void set_string_table(const IRStringTable* strings) { m_strings = strings; }

	// A pass name may appear more than once (peephole runs three times).
	static const std::vector<IRPass>& pipeline();

	const IRStringTable* m_strings = nullptr;

	void set_pass_limit(size_t count) { m_pass_limit = count; }
	void set_enabled_passes(const std::vector<std::string>& names);
	// max_registers recomputation still runs even with all passes disabled.
	void disable_all_passes() { set_enabled_passes({"none"}); }

private:
	bool constant_folding(IRFunction& func);
	bool eliminate_unreachable_code(IRFunction& func);
	bool eliminate_dead_code(IRFunction& func);
	bool tighten_scope_marks(IRFunction& func);
	bool peephole_optimization(IRFunction& func);

	// On match: appends replacement, advances i, returns true. On miss: no side effects.
	using PeepholePattern = bool (IROptimizer::*)(const IRFunction& func, size_t& i,
		std::vector<IRInstruction>& new_instructions);

	bool try_fuse_compare_and_branch(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions);
	bool try_eliminate_moves_around_op(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions);
	bool try_eliminate_move_pair(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions);
	bool try_fold_move_after_op(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions);
	bool try_remove_branch_to_next(const IRFunction& func, size_t& i, std::vector<IRInstruction>& new_instructions);
	bool copy_propagation(IRFunction& func);
	bool eliminate_redundant_stores(IRFunction& func);
	bool scalar_replace_structs(IRFunction& func);
	bool reduce_register_pressure(IRFunction& func);
	bool loop_invariant_code_motion(IRFunction& func);
	bool enhanced_copy_propagation(IRFunction& func);

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

	// Forward dataflow block; state cleared at every LABEL, so constants do not
	// cross an `if`. Shape only: per-pass state (constants in, reachability)
	// rides in the pass's own parallel array, so the shape is shareable.
	struct Block {
		size_t begin = 0;
		size_t end = 0;
		std::vector<size_t> successors;
	};

	// Control flow shared by the passes that read it. Invalidated by any
	// instruction move; label names are unique by construction (gen_label
	// counter, enforced by the verifier), so first- and last-seen index agree.
	struct FunctionAnalysis {
		bool valid = false;
		const IRFunction* func = nullptr;
		size_t instruction_count = 0;
		std::unordered_map<uint32_t, size_t> label_index;
		std::vector<Block> blocks;
	};
	FunctionAnalysis m_analysis;

	const FunctionAnalysis& analysis(const IRFunction& func);
	void invalidate_analysis() { m_analysis.valid = false; }

	// Assigns; answers whether the result differs. Invalidates on difference.
	bool replace_instructions(IRFunction& func, std::vector<IRInstruction>&& fresh);

	// Intersect: keeps only registers both agree on. Returns true if anything was dropped.
	static bool meet_constants(ConstantMap& into, const ConstantMap& from);

	// Shared by analysis (out==nullptr) and rewrite (out!=nullptr) to prevent drift.
	void fold_instruction(const IRInstruction& instr, std::vector<IRInstruction>* out);

	ConstantValue get_constant(const IRValue& val);
	bool try_fold_binary_op(IROpcode op, IRInstruction::TypeHint type_hint, const ConstantValue& lhs, const ConstantValue& rhs, ConstantValue& result);
	void set_register_constant(int reg, const ConstantValue& value);
	void invalidate_register(int reg);

	bool is_register_used_after(const IRFunction& func, int reg, size_t instr_idx);

	// Opcode classification comes from ir_opcodes.def, not a list here.
	static bool is_reg_used_between_exclusive(const IRFunction& func, int reg, size_t start_idx, size_t end_idx, bool conservative_at_labels = true);

	// Whole-function liveness, not windowed: MOVE-folding deletes definitions,
	// so the temp must be dead everywhere, not just in the pattern's window.
	static bool is_reg_read_outside(const IRFunction& func, int reg, size_t first, size_t last);

	struct LoopInfo {
		size_t header_idx;
		size_t end_idx;
		uint32_t header_label = IRStringTable::INVALID_ID;
		uint32_t end_label = IRStringTable::INVALID_ID;
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
	bool is_unconditional_in_loop(size_t instr_idx, const LoopInfo& loop, const IRFunction& func);

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
