#include "ir_interpreter.h"
#include <cmath>
#include <sstream>

namespace gdscript {

IRInterpreter::IRInterpreter(const IRProgram& program) : m_program(program) {
	// Build function map
	for (const auto& func : program.functions) {
		m_function_map[func.name] = &func;
	}
}

IRInterpreter::Value IRInterpreter::call(const std::string& function_name, const std::vector<Value>& args) {
	auto it = m_function_map.find(function_name);
	if (it == m_function_map.end()) {
		m_error = "Function not found: " + function_name;
		return int64_t(0);
	}

	const IRFunction* func = it->second;
	ExecutionContext ctx;

	// Set up parameters in registers (first N registers)
	for (size_t i = 0; i < args.size() && i < func->parameters.size(); i++) {
		ctx.registers[static_cast<int>(i)] = args[i];
	}

	// Build label map
	for (size_t i = 0; i < func->instructions.size(); i++) {
		const auto& instr = func->instructions[i];
		if (instr.opcode == IROpcode::LABEL && !instr.operands.empty()) {
			if (instr.operands[0].type == IRValue::Type::LABEL) {
				ctx.labels[std::get<std::string>(instr.operands[0].value)] = i;
			}
		}
	}

	// Execute
	execute_function(*func, ctx);

	if (ctx.returned) {
		return ctx.return_value;
	}

	return int64_t(0);
}

void IRInterpreter::execute_function(const IRFunction& func, ExecutionContext& ctx) {
	ctx.pc = 0;

	while (ctx.pc < func.instructions.size() && !ctx.returned) {
		execute_instruction(func.instructions[ctx.pc], ctx);
		if (!ctx.returned) {
			ctx.pc++;
		}
	}
}

IRInterpreter::Value IRInterpreter::get_register(ExecutionContext& ctx, int reg) {
	if (ctx.registers.find(reg) == ctx.registers.end()) {
		ctx.registers[reg] = int64_t(0);
	}
	return ctx.registers[reg];
}

void IRInterpreter::execute_instruction(const IRInstruction& instr, ExecutionContext& ctx) {
	switch (instr.opcode) {
		case IROpcode::LOAD_IMM: {
			int reg = std::get<int>(instr.operands[0].value);
			int64_t imm = std::get<int64_t>(instr.operands[1].value);
			ctx.registers[reg] = imm;
			break;
		}

		case IROpcode::MOVE: {
			int dst = std::get<int>(instr.operands[0].value);
			int src = std::get<int>(instr.operands[1].value);
			ctx.registers[dst] = get_register(ctx, src);
			break;
		}

		case IROpcode::ADD:
		case IROpcode::SUB:
		case IROpcode::MUL:
		case IROpcode::DIV:
		case IROpcode::MOD: {
			int dst = std::get<int>(instr.operands[0].value);
			int src1 = std::get<int>(instr.operands[1].value);
			int src2 = std::get<int>(instr.operands[2].value);
			ctx.registers[dst] = binary_op(get_register(ctx, src1), get_register(ctx, src2), instr.opcode);
			break;
		}

		case IROpcode::NEG: {
			int dst = std::get<int>(instr.operands[0].value);
			int src = std::get<int>(instr.operands[1].value);
			ctx.registers[dst] = unary_op(get_register(ctx, src), IROpcode::NEG);
			break;
		}

		case IROpcode::NOT: {
			int dst = std::get<int>(instr.operands[0].value);
			int src = std::get<int>(instr.operands[1].value);
			ctx.registers[dst] = unary_op(get_register(ctx, src), IROpcode::NOT);
			break;
		}

		case IROpcode::CMP_EQ:
		case IROpcode::CMP_NEQ:
		case IROpcode::CMP_LT:
		case IROpcode::CMP_LTE:
		case IROpcode::CMP_GT:
		case IROpcode::CMP_GTE: {
			int dst = std::get<int>(instr.operands[0].value);
			int src1 = std::get<int>(instr.operands[1].value);
			int src2 = std::get<int>(instr.operands[2].value);
			ctx.registers[dst] = compare_op(get_register(ctx, src1), get_register(ctx, src2), instr.opcode);
			break;
		}

		case IROpcode::AND: {
			int dst = std::get<int>(instr.operands[0].value);
			int src1 = std::get<int>(instr.operands[1].value);
			int src2 = std::get<int>(instr.operands[2].value);
			bool result = get_bool(get_register(ctx, src1)) && get_bool(get_register(ctx, src2));
			ctx.registers[dst] = result ? int64_t(1) : int64_t(0);
			break;
		}

		case IROpcode::OR: {
			int dst = std::get<int>(instr.operands[0].value);
			int src1 = std::get<int>(instr.operands[1].value);
			int src2 = std::get<int>(instr.operands[2].value);
			bool result = get_bool(get_register(ctx, src1)) || get_bool(get_register(ctx, src2));
			ctx.registers[dst] = result ? int64_t(1) : int64_t(0);
			break;
		}

		case IROpcode::LABEL:
			// No-op, handled during label resolution
			break;

		case IROpcode::JUMP: {
			std::string label = std::get<std::string>(instr.operands[0].value);
			auto it = ctx.labels.find(label);
			if (it != ctx.labels.end()) {
				ctx.pc = it->second;
			} else {
				throw std::runtime_error("Label not found: " + label);
			}
			break;
		}

		case IROpcode::BRANCH_ZERO: {
			int reg = std::get<int>(instr.operands[0].value);
			std::string label = std::get<std::string>(instr.operands[1].value);

			if (!get_bool(get_register(ctx, reg))) {
				auto it = ctx.labels.find(label);
				if (it != ctx.labels.end()) {
					ctx.pc = it->second;
				}
			}
			break;
		}

		case IROpcode::BRANCH_NOT_ZERO: {
			int reg = std::get<int>(instr.operands[0].value);
			std::string label = std::get<std::string>(instr.operands[1].value);

			if (get_bool(get_register(ctx, reg))) {
				auto it = ctx.labels.find(label);
				if (it != ctx.labels.end()) {
					ctx.pc = it->second;
				}
			}
			break;
		}

		case IROpcode::CALL: {
			// CALL format: function_name, result_reg, arg_count, arg1_reg, arg2_reg, ...
			std::string func_name = std::get<std::string>(instr.operands[0].value);
			int result_reg = std::get<int>(instr.operands[1].value);
			int arg_count = static_cast<int>(std::get<int64_t>(instr.operands[2].value));

			// Collect arguments from registers
			std::vector<Value> args;
			for (int i = 0; i < arg_count; i++) {
				int arg_reg = std::get<int>(instr.operands[3 + i].value);
				args.push_back(get_register(ctx, arg_reg));
			}

			Value result = call(func_name, args);
			ctx.registers[result_reg] = result;
			break;
		}

		case IROpcode::RETURN:
			ctx.returned = true;
			// Return value is in register 0 by convention
			if (ctx.registers.find(0) != ctx.registers.end()) {
				ctx.return_value = ctx.registers[0];
			} else {
				ctx.return_value = int64_t(0);
			}
			break;

		default:
			throw std::runtime_error("Unimplemented opcode in interpreter");
	}
}

int64_t IRInterpreter::get_int(const Value& v) const {
	if (std::holds_alternative<int64_t>(v)) {
		return std::get<int64_t>(v);
	} else if (std::holds_alternative<double>(v)) {
		return static_cast<int64_t>(std::get<double>(v));
	} else if (std::holds_alternative<bool>(v)) {
		return std::get<bool>(v) ? 1 : 0;
	}
	return 0;
}

double IRInterpreter::get_double(const Value& v) const {
	if (std::holds_alternative<double>(v)) {
		return std::get<double>(v);
	} else if (std::holds_alternative<int64_t>(v)) {
		return static_cast<double>(std::get<int64_t>(v));
	}
	return 0.0;
}

bool IRInterpreter::get_bool(const Value& v) const {
	if (std::holds_alternative<bool>(v)) {
		return std::get<bool>(v);
	} else if (std::holds_alternative<int64_t>(v)) {
		return std::get<int64_t>(v) != 0;
	} else if (std::holds_alternative<double>(v)) {
		return std::get<double>(v) != 0.0;
	}
	return false;
}

std::string IRInterpreter::get_string(const Value& v) const {
	if (std::holds_alternative<std::string>(v)) {
		return std::get<std::string>(v);
	}
	return "";
}

IRInterpreter::Value IRInterpreter::binary_op(const Value& left, const Value& right, IROpcode op) {
	// Try integer arithmetic first
	int64_t l = get_int(left);
	int64_t r = get_int(right);

	switch (op) {
		case IROpcode::ADD: return l + r;
		case IROpcode::SUB: return l - r;
		case IROpcode::MUL: return l * r;
		case IROpcode::DIV: return r != 0 ? l / r : int64_t(0);
		case IROpcode::MOD: return r != 0 ? l % r : int64_t(0);
		default: return int64_t(0);
	}
}

IRInterpreter::Value IRInterpreter::unary_op(const Value& operand, IROpcode op) {
	switch (op) {
		case IROpcode::NEG:
			return -get_int(operand);
		case IROpcode::NOT:
			return !get_bool(operand) ? int64_t(1) : int64_t(0);
		default:
			return int64_t(0);
	}
}

IRInterpreter::Value IRInterpreter::compare_op(const Value& left, const Value& right, IROpcode op) {
	int64_t l = get_int(left);
	int64_t r = get_int(right);

	bool result = false;
	switch (op) {
		case IROpcode::CMP_EQ: result = (l == r); break;
		case IROpcode::CMP_NEQ: result = (l != r); break;
		case IROpcode::CMP_LT: result = (l < r); break;
		case IROpcode::CMP_LTE: result = (l <= r); break;
		case IROpcode::CMP_GT: result = (l > r); break;
		case IROpcode::CMP_GTE: result = (l >= r); break;
		default: break;
	}

	return result ? int64_t(1) : int64_t(0);
}

} // namespace gdscript
