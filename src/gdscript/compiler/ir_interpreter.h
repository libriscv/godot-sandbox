#pragma once
#include "ir.h"
#include <unordered_map>
#include <vector>
#include <variant>
#include <stdexcept>

namespace gdscript {

// Simple IR interpreter for testing without needing full RISC-V execution
class IRInterpreter {
public:
	using Value = std::variant<int64_t, double, std::string, bool>;

	IRInterpreter(const IRProgram& program);

	// Execute a function and return result
	Value call(const std::string& function_name, const std::vector<Value>& args = {});

	// Get last error
	std::string get_error() const { return m_error; }

	// Current value of a global, by index. Exposed so a test can compare the
	// side effects of a run, not just its return value.
	const Value& global(size_t index) const { return m_globals.at(index); }
	size_t global_count() const { return m_globals.size(); }

private:
	struct ExecutionContext {
		std::unordered_map<int, Value> registers; // Virtual register -> value
		std::unordered_map<std::string, size_t> labels; // Label -> instruction index
		size_t pc = 0; // Program counter
		bool returned = false;
		Value return_value;
	};

	void execute_function(const IRFunction& func, ExecutionContext& ctx);
	void execute_instruction(const IRFunction& func, const IRInstruction& instr, ExecutionContext& ctx);
	Value get_register(ExecutionContext& ctx, int reg);

	// Initialize globals from their compile-time initializers, then run the
	// synthetic global_init function when the program has one.
	void initialize_globals();

	// Jump to `label` in the function being executed, or throw when the label
	// does not exist. A branch to a label that was deleted is a broken IR, not a
	// fallthrough.
	void jump_to_label(const IRInstruction& instr, const std::string& label, ExecutionContext& ctx);

	// Helper functions
	int64_t get_int(const Value& v) const;
	double get_double(const Value& v) const;
	bool get_bool(const Value& v) const;
	std::string get_string(const Value& v) const;

	// Whether a value participates in arithmetic as a float. Mirrors GDScript:
	// int + int stays integer, anything touching a float becomes a float.
	static bool is_float(const Value& v);
	static bool is_string(const Value& v);

	Value binary_op(const Value& left, const Value& right, IROpcode op);
	Value unary_op(const Value& operand, IROpcode op);
	Value compare_op(const Value& left, const Value& right, IROpcode op);

	// Whether a fused branch (BRANCH_EQ .. BRANCH_GTE) is taken.
	bool fused_branch_taken(const Value& left, const Value& right, IROpcode op);

	const IRProgram& m_program;
	std::unordered_map<std::string, const IRFunction*> m_function_map;
	std::vector<Value> m_globals;
	std::string m_error;
	int m_call_depth = 0;
};

} // namespace gdscript
