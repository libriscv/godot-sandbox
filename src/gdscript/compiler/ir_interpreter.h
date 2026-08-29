#pragma once
#include "ir.h"
#include <unordered_map>
#include <vector>
#include <variant>
#include <stdexcept>

namespace gdscript {

class IRInterpreter {
public:
	// Keep NIL distinct from integer zero. This matters for nullable type tests
	// and mirrors the Variant tag observed by the machine backend.
	using Value = std::variant<std::monostate, int64_t, double, std::string, bool>;

	IRInterpreter(const IRProgram& program);

	Value call(const std::string& function_name, const std::vector<Value>& args = {});

	std::string get_error() const { return m_error; }

	// Exposed for test assertions on side effects.
	const Value& global(size_t index) const { return m_globals.at(index); }
	size_t global_count() const { return m_globals.size(); }

private:
	struct ExecutionContext {
		std::unordered_map<int, Value> registers;
		std::unordered_map<std::string, size_t> labels;
		size_t pc = 0;
		bool returned = false;
		Value return_value;
	};

	void execute_function(const IRFunction& func, ExecutionContext& ctx);
	void execute_instruction(const IRFunction& func, const IRInstruction& instr, ExecutionContext& ctx);
	Value get_register(ExecutionContext& ctx, int reg);

	void initialize_globals();

	// Throws on missing label (broken IR, not a fallthrough).
	void jump_to_label(const IRInstruction& instr, const std::string& label, ExecutionContext& ctx);
	// Resolves an operand's interned name.
	const std::string& label_text(const IRValue& value) const { return m_program.strings[value.string_id]; }

	int64_t get_int(const Value& v) const;
	double get_double(const Value& v) const;
	bool get_bool(const Value& v) const;
	std::string get_string(const Value& v) const;

	static bool is_float(const Value& v);
	static bool is_string(const Value& v);

	Value binary_op(const Value& left, const Value& right, IROpcode op);
	Value unary_op(const Value& operand, IROpcode op);
	Value compare_op(const Value& left, const Value& right, IROpcode op);

	bool fused_branch_taken(const Value& left, const Value& right, IROpcode op);

	const IRProgram& m_program;
	std::unordered_map<std::string, const IRFunction*> m_function_map;
	std::vector<Value> m_globals;
	std::string m_error;
	int m_call_depth = 0;
};

} // namespace gdscript
