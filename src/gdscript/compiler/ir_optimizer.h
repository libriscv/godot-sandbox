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
	static bool is_reg_used_between_exclusive(const IRFunction& func, int reg, size_t start_idx, size_t end_idx);
	static bool is_pure_load_op(IROpcode op);
};

} // namespace gdscript
