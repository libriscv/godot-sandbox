#pragma once
#include "ir.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gdscript {

class IROptimizer;

// One step of the optimizer pipeline. The pipeline is a list rather than a
// straight-line function body so that a test can run a prefix of it: when an
// optimized program computes something different from the unoptimized one, the
// shortest prefix that reproduces the difference names the guilty pass.
struct IRPass {
	const char* name;
	void (IROptimizer::*run)(IRFunction&);
};

// IR-level optimizations to reduce stack usage and improve performance
class IROptimizer {
public:
	IROptimizer();

	// Optimize an entire IR program
	void optimize(IRProgram& program);

	// Optimize a single function
	void optimize_function(IRFunction& func);

	// The pipeline, in the order optimize_function() runs it. A pass name may
	// appear more than once: peephole runs three times, at different points.
	static const std::vector<IRPass>& pipeline();

	// Run only the first `count` pipeline steps. The default runs all of them.
	void set_pass_limit(size_t count) { m_pass_limit = count; }

	// Run only the named passes. An empty list (the default) runs every pass.
	// Names are pipeline step names; naming a pass that runs several times
	// enables all of its occurrences.
	void set_enabled_passes(const std::vector<std::string>& names);

	// Disable every pass. optimize_function() still recomputes max_registers,
	// which is bookkeeping rather than an optimization.
	void disable_all_passes() { set_enabled_passes({"none"}); }

private:
	// Optimization passes
	void constant_folding(IRFunction& func);
	void eliminate_unreachable_code(IRFunction& func);
	void eliminate_dead_code(IRFunction& func);
	void peephole_optimization(IRFunction& func);

	// One peephole pattern. Looks at func.instructions[i]; on a match it
	// appends the replacement to new_instructions, advances i past what it
	// consumed, and returns true. Returning false must leave both untouched.
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

	// Helper for constant folding
	struct ConstantValue {
		enum class Type { NONE, INT, FLOAT, BOOL, STRING };
		Type type = Type::NONE;
		int64_t int_value = 0;
		double float_value = 0.0;
		bool bool_value = false;
		std::string string_value;

		bool is_constant() const { return type != Type::NONE; }

		// Two states agree only when they name the same value. Doubles compare
		// by their bits so that NaN equals itself and -0.0 does not equal 0.0:
		// a join must not claim to know a value it would fold differently.
		bool same_as(const ConstantValue& other) const;

		// Godot's Variant::booleanize() for the kinds this pass tracks, which
		// is what BRANCH_ZERO and BRANCH_NOT_ZERO test. False when the value
		// is not one this pass can decide.
		bool truthiness(bool& truth) const;
	};

	// Track known constant values for registers
	using ConstantMap = std::unordered_map<int, ConstantValue>;
	ConstantMap m_constants;

	// One basic block of a function, with the constant state control arrives
	// with. Constant folding is a forward dataflow over these rather than a
	// linear scan: a LABEL is a join point, and clearing the state at one --
	// which is what this pass used to do -- throws away every constant that no
	// path between its definition and the label touches. In a real program that
	// ended constant folding at the first `if`.
	struct ConstBlock {
		size_t begin = 0;
		size_t end = 0;
		std::vector<size_t> successors;
		ConstantMap entry;
		bool entry_initialized = false;   // false means: nothing reaches it
	};

	static std::vector<ConstBlock> build_blocks(const IRFunction& func);

	// Intersect `into` with `from`, keeping only registers both agree on.
	// Returns whether anything was dropped.
	static bool meet_constants(ConstantMap& into, const ConstantMap& from);

	// The transfer function, shared by the analysis and the rewrite so the two
	// cannot drift: it always updates m_constants, and appends the instruction
	// -- folded, if it could be -- only when `out` is non-null.
	void fold_instruction(const IRInstruction& instr, std::vector<IRInstruction>* out);

	// Helper methods
	ConstantValue get_constant(const IRValue& val);
	bool try_fold_binary_op(IROpcode op, IRInstruction::TypeHint type_hint, const ConstantValue& lhs, const ConstantValue& rhs, ConstantValue& result);
	void set_register_constant(int reg, const ConstantValue& value);
	void invalidate_register(int reg);

	// Dead code elimination helpers
	std::unordered_set<int> find_live_registers(const IRFunction& func);
	bool is_register_used_after(const IRFunction& func, int reg, size_t instr_idx);

	// Peephole optimization helpers. What an opcode is -- arithmetic, a branch,
	// a plain load -- comes from the metadata table in ir_opcodes.def, not from
	// a list maintained here.
	static bool is_reg_used_between_exclusive(const IRFunction& func, int reg, size_t start_idx, size_t end_idx, bool conservative_at_labels = true);

	// Whether any instruction outside [first, last] reads `reg`.
	//
	// The patterns that collapse a MOVE into the instruction around it delete
	// the definitions of the temporaries they fold away, so those registers
	// have to be dead in the whole function -- not just in the four
	// instructions the pattern covers. Checking only the window is how a
	// definition something later still reads gets deleted.
	static bool is_reg_read_outside(const IRFunction& func, int reg, size_t first, size_t last);

	// LICM helpers
	struct LoopInfo {
		size_t header_idx;      // Index of loop header LABEL
		size_t end_idx;         // Index of loop end LABEL
		std::string header_label;
		std::string end_label;
		std::vector<size_t> back_edges;  // Indices of JUMP instructions to header
	};
	std::vector<LoopInfo> identify_loops(const IRFunction& func);
	bool is_loop_invariant(const IRInstruction& instr, const LoopInfo& loop,
	                       const IRFunction& func, const std::unordered_set<int>& invariant_regs);
	bool can_safely_hoist(const IRInstruction& instr, size_t instr_idx, const LoopInfo& loop, const IRFunction& func);
	// Whether every iteration that enters the loop body reaches this
	// instruction, rather than it sitting inside an `if` within the body.
	static bool is_unconditional_in_loop(size_t instr_idx, const LoopInfo& loop, const IRFunction& func);

	// Enhanced copy propagation helpers
	struct CopyInfo {
		int source_reg;
		size_t def_idx;
	};
	bool register_unmodified_between(const IRFunction& func, int reg, size_t start_idx, size_t end_idx);

	// Pass selection. m_enabled_passes empty means "every pass".
	size_t m_pass_limit = SIZE_MAX;
	std::unordered_set<std::string> m_enabled_passes;
	bool is_pass_enabled(const char* name) const;
};

} // namespace gdscript
