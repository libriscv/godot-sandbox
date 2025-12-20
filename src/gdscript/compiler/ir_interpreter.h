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

private:
	struct ExecutionContext {
		std::unordered_map<int, Value> registers; // Virtual register -> value
		std::unordered_map<std::string, size_t> labels; // Label -> instruction index
		size_t pc = 0; // Program counter
		bool returned = false;
		Value return_value;
	};

	void execute_function(const IRFunction& func, ExecutionContext& ctx);
	void execute_instruction(const IRInstruction& instr, ExecutionContext& ctx);
	Value get_register(ExecutionContext& ctx, int reg);

	// Helper functions
	int64_t get_int(const Value& v) const;
	double get_double(const Value& v) const;
	bool get_bool(const Value& v) const;
	std::string get_string(const Value& v) const;

	Value binary_op(const Value& left, const Value& right, IROpcode op);
	Value unary_op(const Value& operand, IROpcode op);
	Value compare_op(const Value& left, const Value& right, IROpcode op);

	const IRProgram& m_program;
	std::unordered_map<std::string, const IRFunction*> m_function_map;
	std::string m_error;
};

} // namespace gdscript
