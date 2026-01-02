#pragma once
#include "ir.h"
#include <unordered_map>
#include <unordered_set>

namespace gdscript {

// IR-level optimizations to reduce stack usage and improve performance
class IROptimizer {
public:
	IROptimizer();

	// Optimize an entire IR program
	void optimize(IRProgram& program);

	// Optimize a single function
	void optimize_function(IRFunction& func);

private:
	// Optimization passes
	void constant_folding(IRFunction& func);
	void eliminate_dead_code(IRFunction& func);
	void peephole_optimization(IRFunction& func);
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
	};

	// Track known constant values for registers
	std::unordered_map<int, ConstantValue> m_constants;

	// Helper methods
	ConstantValue get_constant(const IRValue& val);
	bool try_fold_binary_op(IROpcode op, IRInstruction::TypeHint type_hint, const ConstantValue& lhs, const ConstantValue& rhs, ConstantValue& result);
	void set_register_constant(int reg, const ConstantValue& value);
	void invalidate_register(int reg);

	// Dead code elimination helpers
	std::unordered_set<int> find_live_registers(const IRFunction& func);
	bool is_register_used_after(const IRFunction& func, int reg, size_t instr_idx);

	// Peephole optimization helpers
	static bool is_arithmetic_op(IROpcode op);
	static bool is_branch_op(IROpcode op);
	static bool is_reg_used_between_exclusive(const IRFunction& func, int reg, size_t start_idx, size_t end_idx, bool conservative_at_labels = true);
	static bool is_pure_load_op(IROpcode op);

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

	// Enhanced copy propagation helpers
	struct CopyInfo {
		int source_reg;
		size_t def_idx;
	};
	bool register_unmodified_between(const IRFunction& func, int reg, size_t start_idx, size_t end_idx);
};

} // namespace gdscript
