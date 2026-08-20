#include "ir_interpreter.h"
#include "compiler_exception.h"
#include <cmath>
#include <cstdint>
#include <sstream>

namespace gdscript {

// A runaway program is a compiler bug, not a hang. The interpreter is only ever
// pointed at test programs, so any run this long is a lost loop.
static constexpr size_t MAX_STEPS = 20'000'000;
static constexpr int MAX_CALL_DEPTH = 256;

IRInterpreter::IRInterpreter(const IRProgram& program) : m_program(program) {
	// Build function map
	for (const auto& func : program.functions) {
		m_function_map[func.name] = &func;
	}
	initialize_globals();
}

void IRInterpreter::initialize_globals() {
	m_globals.reserve(m_program.globals.size());
	for (const auto& global : m_program.globals) {
		switch (global.init_type) {
			case IRGlobalVar::InitType::INT:
				m_globals.push_back(std::get<int64_t>(global.init_value));
				break;
			case IRGlobalVar::InitType::FLOAT:
				m_globals.push_back(std::get<double>(global.init_value));
				break;
			case IRGlobalVar::InitType::STRING:
				m_globals.push_back(std::get<std::string>(global.init_value));
				break;
			case IRGlobalVar::InitType::BOOL:
				m_globals.push_back(std::get<bool>(global.init_value));
				break;
			case IRGlobalVar::InitType::NONE:
			case IRGlobalVar::InitType::NULL_VAL:
			case IRGlobalVar::InitType::EMPTY_ARRAY:
			case IRGlobalVar::InitType::EMPTY_DICT:
			case IRGlobalVar::InitType::RUNTIME:
				// NIL, containers and startup-evaluated initializers have no
				// representation here. RUNTIME globals are overwritten by
				// global_init below; the rest stay as an integer zero, which is
				// what a NIL Variant booleanizes and compares to.
				m_globals.push_back(int64_t(0));
				break;
		}
	}

	if (m_program.has_global_init) {
		ExecutionContext ctx;
		execute_function(m_program.global_init, ctx);
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

	// Execute
	execute_function(*func, ctx);

	if (ctx.returned) {
		return ctx.return_value;
	}

	return int64_t(0);
}

void IRInterpreter::execute_function(const IRFunction& func, ExecutionContext& ctx) {
	if (++m_call_depth > MAX_CALL_DEPTH) {
		m_call_depth--;
		throw CompilerException(ErrorType::OPTIMIZER_ERROR,
			"IR interpreter call depth exceeded in function '" + func.name + "'");
	}

	// Build label map
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];
		if (instr.opcode == IROpcode::LABEL && !instr.operands.empty()) {
			if (instr.operands[0].type == IRValue::Type::LABEL) {
				ctx.labels[std::get<std::string>(instr.operands[0].value)] = i;
			}
		}
	}

	ctx.pc = 0;

	size_t steps = 0;
	while (ctx.pc < func.instructions.size() && !ctx.returned) {
		if (++steps > MAX_STEPS) {
			m_call_depth--;
			throw CompilerException(ErrorType::OPTIMIZER_ERROR,
				"IR interpreter step limit exceeded in function '" + func.name + "'");
		}
		const size_t pc = ctx.pc;
		execute_instruction(func, func.instructions[pc], ctx);
		// A branch that was taken already moved the pc; anything else advances.
		if (!ctx.returned && ctx.pc == pc) {
			ctx.pc++;
		}
	}

	m_call_depth--;
}

IRInterpreter::Value IRInterpreter::get_register(ExecutionContext& ctx, int reg) {
	if (ctx.registers.find(reg) == ctx.registers.end()) {
		ctx.registers[reg] = int64_t(0);
	}
	return ctx.registers[reg];
}

void IRInterpreter::jump_to_label(const IRInstruction& instr, const std::string& label, ExecutionContext& ctx) {
	auto it = ctx.labels.find(label);
	if (it == ctx.labels.end()) {
		throw CompilerException(ErrorType::OPTIMIZER_ERROR,
			std::string(ir_opcode_name(instr.opcode)) + " targets a label that does not exist: " + label);
	}
	ctx.pc = it->second;
}

void IRInterpreter::execute_instruction(const IRFunction& func, const IRInstruction& instr, ExecutionContext& ctx) {
	switch (instr.opcode) {
		case IROpcode::LOAD_IMM: {
			int reg = std::get<int>(instr.operands[0].value);
			int64_t imm = std::get<int64_t>(instr.operands[1].value);
			ctx.registers[reg] = imm;
			break;
		}

		case IROpcode::LOAD_BOOL: {
			int reg = std::get<int>(instr.operands[0].value);
			int64_t imm = std::get<int64_t>(instr.operands[1].value);
			ctx.registers[reg] = (imm != 0);
			break;
		}

		case IROpcode::LOAD_FLOAT_IMM: {
			int reg = std::get<int>(instr.operands[0].value);
			ctx.registers[reg] = std::get<double>(instr.operands[1].value);
			break;
		}

		case IROpcode::LOAD_STRING: {
			int reg = std::get<int>(instr.operands[0].value);
			const int64_t index = std::get<int64_t>(instr.operands[1].value);
			if (index < 0 || static_cast<size_t>(index) >= m_program.string_constants.size()) {
				throw CompilerException(ErrorType::OPTIMIZER_ERROR, "String constant index out of range");
			}
			ctx.registers[reg] = m_program.string_constants[index];
			break;
		}

		case IROpcode::LOAD_GLOBAL: {
			int reg = std::get<int>(instr.operands[0].value);
			const int64_t index = std::get<int64_t>(instr.operands[1].value);
			if (index < 0 || static_cast<size_t>(index) >= m_globals.size()) {
				throw CompilerException(ErrorType::OPTIMIZER_ERROR, "Global index out of range");
			}
			ctx.registers[reg] = m_globals[index];
			break;
		}

		case IROpcode::STORE_GLOBAL: {
			const int64_t index = std::get<int64_t>(instr.operands[0].value);
			int reg = std::get<int>(instr.operands[1].value);
			if (index < 0 || static_cast<size_t>(index) >= m_globals.size()) {
				throw CompilerException(ErrorType::OPTIMIZER_ERROR, "Global index out of range");
			}
			m_globals[index] = get_register(ctx, reg);
			break;
		}

		case IROpcode::CONVERT: {
			int dst = std::get<int>(instr.operands[0].value);
			int src = std::get<int>(instr.operands[1].value);
			Value src_value = get_register(ctx, src);
			if (instr.type_hint != Variant::FLOAT) {
				throw CompilerException(ErrorType::OPTIMIZER_ERROR,
					std::string("CONVERT to ") + variant_type_name(instr.type_hint) +
					" is not implemented in the interpreter");
			}
			ctx.registers[dst] = get_double(src_value);
			break;
		}

		case IROpcode::MOVE: {
			int dst = std::get<int>(instr.operands[0].value);
			int src = std::get<int>(instr.operands[1].value);
			// Get src value first to avoid map iterator invalidation
			Value src_value = get_register(ctx, src);
			ctx.registers[dst] = src_value;
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
			int dst = std::get<int>(instr.operands[0].value);
			int src1 = std::get<int>(instr.operands[1].value);
			int src2 = std::get<int>(instr.operands[2].value);
			// Get operands first to avoid map iterator invalidation
			Value val1 = get_register(ctx, src1);
			Value val2 = get_register(ctx, src2);
			ctx.registers[dst] = binary_op(val1, val2, instr.opcode);
			break;
		}

		case IROpcode::NEG:
		case IROpcode::NOT:
		case IROpcode::BIT_NOT: {
			int dst = std::get<int>(instr.operands[0].value);
			int src = std::get<int>(instr.operands[1].value);
			ctx.registers[dst] = unary_op(get_register(ctx, src), instr.opcode);
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
			Value val1 = get_register(ctx, src1);
			Value val2 = get_register(ctx, src2);
			ctx.registers[dst] = compare_op(val1, val2, instr.opcode);
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

		case IROpcode::JUMP:
			jump_to_label(instr, std::get<std::string>(instr.operands[0].value), ctx);
			break;

		case IROpcode::BRANCH_ZERO: {
			int reg = std::get<int>(instr.operands[0].value);
			if (!get_bool(get_register(ctx, reg))) {
				jump_to_label(instr, std::get<std::string>(instr.operands[1].value), ctx);
			}
			break;
		}

		case IROpcode::BRANCH_NOT_ZERO: {
			int reg = std::get<int>(instr.operands[0].value);
			if (get_bool(get_register(ctx, reg))) {
				jump_to_label(instr, std::get<std::string>(instr.operands[1].value), ctx);
			}
			break;
		}

		// Fused comparison + branch, produced by the peephole pass:
		// "BRANCH_<cmp> lhs, rhs, @label".
		case IROpcode::BRANCH_EQ:
		case IROpcode::BRANCH_NEQ:
		case IROpcode::BRANCH_LT:
		case IROpcode::BRANCH_LTE:
		case IROpcode::BRANCH_GT:
		case IROpcode::BRANCH_GTE: {
			int lhs = std::get<int>(instr.operands[0].value);
			int rhs = std::get<int>(instr.operands[1].value);
			Value left = get_register(ctx, lhs);
			Value right = get_register(ctx, rhs);
			if (fused_branch_taken(left, right, instr.opcode)) {
				jump_to_label(instr, std::get<std::string>(instr.operands[2].value), ctx);
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

			if (m_function_map.find(func_name) == m_function_map.end()) {
				throw CompilerException(ErrorType::OPTIMIZER_ERROR,
					"Call to a function the interpreter does not know: " + func_name);
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

		// Everything that needs the host Variant API. There is no `default:`
		// here on purpose: an opcode added to ir_opcodes.def has to be either
		// implemented above or listed here, so it cannot slip through as a
		// silently unsupported instruction.
		case IROpcode::CALL_SYSCALL:
		case IROpcode::PRINT:
		case IROpcode::VCALL:
		case IROpcode::VGET:
		case IROpcode::VSET:
		case IROpcode::VGET_INLINE:
		case IROpcode::VSET_INLINE:
		case IROpcode::MAKE_VECTOR2:
		case IROpcode::MAKE_VECTOR3:
		case IROpcode::MAKE_VECTOR4:
		case IROpcode::MAKE_VECTOR2I:
		case IROpcode::MAKE_VECTOR3I:
		case IROpcode::MAKE_VECTOR4I:
		case IROpcode::MAKE_COLOR:
		case IROpcode::MAKE_RECT2:
		case IROpcode::MAKE_RECT2I:
		case IROpcode::MAKE_PLANE:
		case IROpcode::MAKE_ARRAY:
		case IROpcode::MAKE_DICTIONARY:
		case IROpcode::MAKE_PACKED_BYTE_ARRAY:
		case IROpcode::MAKE_PACKED_INT32_ARRAY:
		case IROpcode::MAKE_PACKED_INT64_ARRAY:
		case IROpcode::MAKE_PACKED_FLOAT32_ARRAY:
		case IROpcode::MAKE_PACKED_FLOAT64_ARRAY:
		case IROpcode::MAKE_PACKED_STRING_ARRAY:
		case IROpcode::MAKE_PACKED_VECTOR2_ARRAY:
		case IROpcode::MAKE_PACKED_VECTOR3_ARRAY:
		case IROpcode::MAKE_PACKED_COLOR_ARRAY:
		case IROpcode::MAKE_PACKED_VECTOR4_ARRAY:
			throw CompilerException(ErrorType::OPTIMIZER_ERROR,
				std::string("Opcode ") + ir_opcode_name(instr.opcode) +
				" needs the host Variant API and is not available in the IR interpreter"
				" (in function '" + func.name + "')");
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
	} else if (std::holds_alternative<bool>(v)) {
		return std::get<bool>(v) ? 1.0 : 0.0;
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
	} else if (std::holds_alternative<std::string>(v)) {
		// Godot's Variant::booleanize() on a String is "not empty".
		return !std::get<std::string>(v).empty();
	}
	return false;
}

std::string IRInterpreter::get_string(const Value& v) const {
	if (std::holds_alternative<std::string>(v)) {
		return std::get<std::string>(v);
	}
	return "";
}

bool IRInterpreter::is_float(const Value& v) {
	return std::holds_alternative<double>(v);
}

bool IRInterpreter::is_string(const Value& v) {
	return std::holds_alternative<std::string>(v);
}

IRInterpreter::Value IRInterpreter::binary_op(const Value& left, const Value& right, IROpcode op) {
	// String concatenation is the one non-numeric binary operation GDScript has
	// on these types.
	if (is_string(left) || is_string(right)) {
		if (op == IROpcode::ADD && is_string(left) && is_string(right)) {
			return get_string(left) + get_string(right);
		}
		throw CompilerException(ErrorType::OPTIMIZER_ERROR,
			std::string("Operator ") + ir_opcode_name(op) + " is not defined on strings");
	}

	// Bitwise operations and shifts are integer-only in GDScript.
	switch (op) {
		case IROpcode::BIT_AND: return get_int(left) & get_int(right);
		case IROpcode::BIT_OR: return get_int(left) | get_int(right);
		case IROpcode::BIT_XOR: return get_int(left) ^ get_int(right);
		// Shift counts are masked to 0-63, matching the RISC-V backend
		case IROpcode::SHL:
			return static_cast<int64_t>(static_cast<uint64_t>(get_int(left)) << (get_int(right) & 63));
		case IROpcode::SHR:
			return get_int(left) >> (get_int(right) & 63);
		default:
			break;
	}

	// int op int stays integer; anything involving a float becomes a float.
	if (is_float(left) || is_float(right)) {
		const double l = get_double(left);
		const double r = get_double(right);
		switch (op) {
			case IROpcode::ADD: return l + r;
			case IROpcode::SUB: return l - r;
			case IROpcode::MUL: return l * r;
			case IROpcode::DIV: return l / r;
			case IROpcode::MOD: return std::fmod(l, r);
			default: break;
		}
		throw CompilerException(ErrorType::OPTIMIZER_ERROR,
			std::string("Operator ") + ir_opcode_name(op) + " is not a float binary operation");
	}

	// Integer arithmetic wraps, because that is what the machine the backend
	// targets does: a RISC-V `add` is modulo 2^64. Computing it as signed
	// int64_t would be undefined behaviour on overflow and, worse, would let
	// the interpreter and the generated code disagree about a program that
	// overflows.
	const uint64_t l = static_cast<uint64_t>(get_int(left));
	const uint64_t r = static_cast<uint64_t>(get_int(right));
	switch (op) {
		case IROpcode::ADD: return static_cast<int64_t>(l + r);
		case IROpcode::SUB: return static_cast<int64_t>(l - r);
		case IROpcode::MUL: return static_cast<int64_t>(l * r);
		// Godot returns 0 for integer division and modulo by zero (after an
		// error), and INT64_MIN / -1 overflows rather than trapping.
		case IROpcode::DIV:
			if (r == 0) return int64_t(0);
			if (r == UINT64_MAX && l == static_cast<uint64_t>(INT64_MIN)) return INT64_MIN;
			return static_cast<int64_t>(get_int(left) / get_int(right));
		case IROpcode::MOD:
			if (r == 0) return int64_t(0);
			if (r == UINT64_MAX && l == static_cast<uint64_t>(INT64_MIN)) return int64_t(0);
			return static_cast<int64_t>(get_int(left) % get_int(right));
		default: break;
	}
	throw CompilerException(ErrorType::OPTIMIZER_ERROR,
		std::string("Operator ") + ir_opcode_name(op) + " is not an integer binary operation");
}

IRInterpreter::Value IRInterpreter::unary_op(const Value& operand, IROpcode op) {
	switch (op) {
		case IROpcode::NEG:
			if (is_float(operand)) {
				return -get_double(operand);
			}
			// Negating INT64_MIN wraps, as it does on the machine.
			return static_cast<int64_t>(0u - static_cast<uint64_t>(get_int(operand)));
		case IROpcode::NOT:
			// GDScript's 'not' produces a bool.
			return !get_bool(operand);
		case IROpcode::BIT_NOT:
			return ~get_int(operand);
		default:
			throw CompilerException(ErrorType::OPTIMIZER_ERROR,
				std::string("Operator ") + ir_opcode_name(op) + " is not a unary operation");
	}
}

IRInterpreter::Value IRInterpreter::compare_op(const Value& left, const Value& right, IROpcode op) {
	bool result = false;

	if (is_string(left) || is_string(right)) {
		// Comparing a String against a non-String is only ever equal/unequal in
		// Godot, and never equal.
		if (!is_string(left) || !is_string(right)) {
			if (op == IROpcode::CMP_EQ) return int64_t(0);
			if (op == IROpcode::CMP_NEQ) return int64_t(1);
			throw CompilerException(ErrorType::OPTIMIZER_ERROR,
				"Ordered comparison between a String and a non-String");
		}
		const std::string l = get_string(left);
		const std::string r = get_string(right);
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

	if (is_float(left) || is_float(right)) {
		const double l = get_double(left);
		const double r = get_double(right);
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

	const int64_t l = get_int(left);
	const int64_t r = get_int(right);
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

bool IRInterpreter::fused_branch_taken(const Value& left, const Value& right, IROpcode op) {
	IROpcode cmp;
	switch (op) {
		case IROpcode::BRANCH_EQ: cmp = IROpcode::CMP_EQ; break;
		case IROpcode::BRANCH_NEQ: cmp = IROpcode::CMP_NEQ; break;
		case IROpcode::BRANCH_LT: cmp = IROpcode::CMP_LT; break;
		case IROpcode::BRANCH_LTE: cmp = IROpcode::CMP_LTE; break;
		case IROpcode::BRANCH_GT: cmp = IROpcode::CMP_GT; break;
		case IROpcode::BRANCH_GTE: cmp = IROpcode::CMP_GTE; break;
		default:
			throw CompilerException(ErrorType::OPTIMIZER_ERROR,
				std::string(ir_opcode_name(op)) + " is not a fused branch");
	}
	return get_bool(compare_op(left, right, cmp));
}

} // namespace gdscript
