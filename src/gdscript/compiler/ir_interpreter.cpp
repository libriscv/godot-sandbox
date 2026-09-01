#include "ir_interpreter.h"
#include "compiler_exception.h"
#include "globals.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>

namespace gdscript {

// Runaway detection; only test programs run here.
static constexpr size_t MAX_STEPS = 20'000'000;
static constexpr int MAX_CALL_DEPTH = 256;

IRInterpreter::IRInterpreter(const IRProgram& program) : m_program(program) {
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
				// RUNTIME is overwritten by global_init below. Unsupported container
				// initializers remain NIL in the scalar-only interpreter.
				m_globals.push_back(std::monostate{});
				break;
		}
	}

	if (m_program.has_global_init) {
		ExecutionContext ctx;
		execute_function(m_program.global_init, ctx);
	}
	if (m_program.has_member_init) {
		ExecutionContext ctx;
		execute_function(m_program.member_init, ctx);
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

	for (size_t i = 0; i < args.size() && i < func->parameters.size(); i++) {
		ctx.registers[static_cast<int>(i)] = args[i];
	}

	execute_function(*func, ctx);

	if (ctx.returned) {
		return ctx.return_value;
	}

	return std::monostate{};
}

void IRInterpreter::execute_function(const IRFunction& func, ExecutionContext& ctx) {
	if (++m_call_depth > MAX_CALL_DEPTH) {
		m_call_depth--;
		throw CompilerException(ErrorType::OPTIMIZER_ERROR,
			"IR interpreter call depth exceeded in function '" + func.name + "'");
	}

	for (size_t i = 0; i < func.instructions.size(); i++) {
		const auto& instr = func.instructions[i];
		if (instr.opcode == IROpcode::LABEL && !instr.operands.empty()) {
			if (instr.operands[0].type == IRValue::Type::LABEL) {
				ctx.labels[label_text(instr.operands[0])] = i;
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
		// Taken branches already moved pc.
		if (!ctx.returned && ctx.pc == pc) {
			ctx.pc++;
		}
	}

	m_call_depth--;
}

IRInterpreter::Value IRInterpreter::get_register(ExecutionContext& ctx, int reg) {
	if (ctx.registers.find(reg) == ctx.registers.end()) {
		ctx.registers[reg] = std::monostate{};
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
			int reg = instr.operands[0].reg_index();
			int64_t imm = instr.operands[1].immediate();
			ctx.registers[reg] = imm;
			break;
		}

		case IROpcode::SCOPE_MARK:
		case IROpcode::SCOPE_RELEASE:
			break;

		case IROpcode::LOAD_NIL: {
			int reg = instr.operands[0].reg_index();
			ctx.registers[reg] = std::monostate{};
			break;
		}

		case IROpcode::LOAD_BOOL: {
			int reg = instr.operands[0].reg_index();
			int64_t imm = instr.operands[1].immediate();
			ctx.registers[reg] = (imm != 0);
			break;
		}

		case IROpcode::LOAD_FLOAT_IMM: {
			int reg = instr.operands[0].reg_index();
			ctx.registers[reg] = instr.operands[1].float_number();
			break;
		}

		case IROpcode::LOAD_STRING: {
			int reg = instr.operands[0].reg_index();
			const int64_t index = instr.operands[1].immediate();
			if (index < 0 || static_cast<size_t>(index) >= m_program.string_constants.size()) {
				throw CompilerException(ErrorType::OPTIMIZER_ERROR, "String constant index out of range");
			}
			ctx.registers[reg] = m_program.string_constants[index];
			break;
		}

		case IROpcode::LOAD_GLOBAL: {
			int reg = instr.operands[0].reg_index();
			const int64_t index = instr.operands[1].immediate();
			if (index < 0 || static_cast<size_t>(index) >= m_globals.size()) {
				throw CompilerException(ErrorType::OPTIMIZER_ERROR, "Global index out of range");
			}
			ctx.registers[reg] = m_globals[index];
			break;
		}

		case IROpcode::STORE_GLOBAL: {
			const int64_t index = instr.operands[0].immediate();
			int reg = instr.operands[1].reg_index();
			if (index < 0 || static_cast<size_t>(index) >= m_globals.size()) {
				throw CompilerException(ErrorType::OPTIMIZER_ERROR, "Global index out of range");
			}
			m_globals[index] = get_register(ctx, reg);
			break;
		}

		case IROpcode::CONVERT: {
			int dst = instr.operands[0].reg_index();
			int src = instr.operands[1].reg_index();
			Value src_value = get_register(ctx, src);
			if (instr.type_hint == Variant::FLOAT) {
				ctx.registers[dst] = get_double(src_value);
			} else if (instr.type_hint == Variant::INT) {
				ctx.registers[dst] = get_int(src_value);
			} else {
				throw CompilerException(ErrorType::OPTIMIZER_ERROR,
					std::string("CONVERT to ") + variant_type_name(instr.type_hint) +
					" is not implemented in the interpreter");
			}
			break;
		}

		case IROpcode::COERCE: {
			int dst = instr.operands[0].reg_index();
			int src = instr.operands[1].reg_index();
			Value src_value = get_register(ctx, src);
			if (instr.type_hint == Variant::FLOAT) {
				ctx.registers[dst] = get_double(src_value);
			} else if (instr.type_hint == Variant::INT) {
				ctx.registers[dst] = get_int(src_value);
			} else if (instr.type_hint == Variant::BOOL) {
				ctx.registers[dst] = get_bool(src_value);
			} else {
				throw CompilerException(ErrorType::OPTIMIZER_ERROR,
					std::string("COERCE to ") + variant_type_name(instr.type_hint) +
					" is not implemented in the interpreter");
			}
			break;
		}

		case IROpcode::MOVE: {
			int dst = instr.operands[0].reg_index();
			int src = instr.operands[1].reg_index();
			// Copy before write to avoid iterator invalidation.
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
			int dst = instr.operands[0].reg_index();
			int src1 = instr.operands[1].reg_index();
			int src2 = instr.operands[2].reg_index();
			// Copy before write to avoid iterator invalidation.
			Value val1 = get_register(ctx, src1);
			Value val2 = get_register(ctx, src2);
			ctx.registers[dst] = binary_op(val1, val2, instr.opcode);
			break;
		}

		case IROpcode::TYPE_TEST:
		case IROpcode::TYPE_TEST_MASK: {
			int dst = instr.operands[0].reg_index();
			int src = instr.operands[1].reg_index();
			const int64_t tested = instr.operands[2].immediate();
			const Value& value = get_register(ctx, src);
			// Mirror the backend's Variant type-tag comparison.
			int64_t actual = Variant::NIL;
			if (std::holds_alternative<bool>(value)) {
				actual = Variant::BOOL;
			} else if (std::holds_alternative<int64_t>(value)) {
				actual = Variant::INT;
			} else if (std::holds_alternative<double>(value)) {
				actual = Variant::FLOAT;
			} else if (std::holds_alternative<std::string>(value)) {
				actual = Variant::STRING;
			}
			ctx.registers[dst] = instr.opcode == IROpcode::TYPE_TEST
				? (actual == tested)
				: ((tested >> actual) & 1) != 0;
			break;
		}

		case IROpcode::TYPE_OF: {
			int dst = instr.operands[0].reg_index();
			int src = instr.operands[1].reg_index();
			const Value& value = get_register(ctx, src);
			// Match the backend's Variant type-tag read.
			int64_t tag = Variant::NIL;
			if (std::holds_alternative<bool>(value)) {
				tag = Variant::BOOL;
			} else if (std::holds_alternative<int64_t>(value)) {
				tag = Variant::INT;
			} else if (std::holds_alternative<double>(value)) {
				tag = Variant::FLOAT;
			} else if (std::holds_alternative<std::string>(value)) {
				tag = Variant::STRING;
			}
			ctx.registers[dst] = tag;
			break;
		}

		case IROpcode::POW:
			// Host-defined truncation/rounding; no second definition here.
			throw CompilerException(ErrorType::OPTIMIZER_ERROR,
				"'**' is evaluated by the host through Variant::evaluate() and is not"
				" available in the IR interpreter (in function '" + func.name + "')");

		case IROpcode::IN:
			// Requires host container API.
			throw CompilerException(ErrorType::OPTIMIZER_ERROR,
				"'in' needs the host Variant API and is not available in the IR"
				" interpreter (in function '" + func.name + "')");

		case IROpcode::AWAIT:
			// Host-only; excluded from differential/invariance corpora like '**' and 'in'.
			throw CompilerException(ErrorType::OPTIMIZER_ERROR,
				"'await' suspends into the host and is not available in the IR"
				" interpreter (in function '" + func.name + "')");

		case IROpcode::CALL_HOSTED:
			throw CompilerException(ErrorType::OPTIMIZER_ERROR,
				"a call to a coroutine re-enters through the host and is not available"
				" in the IR interpreter (in function '" + func.name + "')");

		case IROpcode::NEG:
		case IROpcode::NOT:
		case IROpcode::BIT_NOT: {
			int dst = instr.operands[0].reg_index();
			int src = instr.operands[1].reg_index();
			ctx.registers[dst] = unary_op(get_register(ctx, src), instr.opcode);
			break;
		}

		case IROpcode::CMP_EQ:
		case IROpcode::CMP_NEQ:
		case IROpcode::CMP_LT:
		case IROpcode::CMP_LTE:
		case IROpcode::CMP_GT:
		case IROpcode::CMP_GTE: {
			int dst = instr.operands[0].reg_index();
			int src1 = instr.operands[1].reg_index();
			int src2 = instr.operands[2].reg_index();
			Value val1 = get_register(ctx, src1);
			Value val2 = get_register(ctx, src2);
			ctx.registers[dst] = compare_op(val1, val2, instr.opcode);
			break;
		}

		case IROpcode::AND: {
			int dst = instr.operands[0].reg_index();
			int src1 = instr.operands[1].reg_index();
			int src2 = instr.operands[2].reg_index();
			bool result = get_bool(get_register(ctx, src1)) && get_bool(get_register(ctx, src2));
			ctx.registers[dst] = result ? int64_t(1) : int64_t(0);
			break;
		}

		case IROpcode::OR: {
			int dst = instr.operands[0].reg_index();
			int src1 = instr.operands[1].reg_index();
			int src2 = instr.operands[2].reg_index();
			bool result = get_bool(get_register(ctx, src1)) || get_bool(get_register(ctx, src2));
			ctx.registers[dst] = result ? int64_t(1) : int64_t(0);
			break;
		}

		case IROpcode::LABEL:
		case IROpcode::BREAKPOINT:
				break;

		case IROpcode::JUMP:
			jump_to_label(instr, label_text(instr.operands[0]), ctx);
			break;

		case IROpcode::BRANCH_ZERO: {
			int reg = instr.operands[0].reg_index();
			if (!get_bool(get_register(ctx, reg))) {
				jump_to_label(instr, label_text(instr.operands[1]), ctx);
			}
			break;
		}

		case IROpcode::BRANCH_NOT_ZERO: {
			int reg = instr.operands[0].reg_index();
			if (get_bool(get_register(ctx, reg))) {
				jump_to_label(instr, label_text(instr.operands[1]), ctx);
			}
			break;
		}

		// Fused comparison + branch (peephole output).
		case IROpcode::BRANCH_EQ:
		case IROpcode::BRANCH_NEQ:
		case IROpcode::BRANCH_LT:
		case IROpcode::BRANCH_LTE:
		case IROpcode::BRANCH_GT:
		case IROpcode::BRANCH_GTE: {
			int lhs = instr.operands[0].reg_index();
			int rhs = instr.operands[1].reg_index();
			Value left = get_register(ctx, lhs);
			Value right = get_register(ctx, rhs);
			if (fused_branch_taken(left, right, instr.opcode)) {
				jump_to_label(instr, label_text(instr.operands[2]), ctx);
			}
			break;
		}

		case IROpcode::SWITCH: {
			// Only integers dispatch; non-integers (incl. whole-valued floats) fall through.
			const Value subject = get_register(ctx, instr.operands[0].reg_index());
			if (!std::holds_alternative<int64_t>(subject)) {
				break;
			}
			const int64_t base = instr.operands[1].immediate();
			const int64_t count = instr.operands[2].immediate();
			const int64_t value = std::get<int64_t>(subject);
			if (value < base || value >= base + count) {
				break;
			}
			const size_t entry = 3 + static_cast<size_t>(value - base);
			jump_to_label(instr, label_text(instr.operands[entry]), ctx);
			break;
		}

		case IROpcode::CALL: {
			std::string func_name = label_text(instr.operands[0]);
			int result_reg = instr.operands[1].reg_index();
			int arg_count = static_cast<int>(instr.operands[2].immediate());

			std::vector<Value> args;
			for (int i = 0; i < arg_count; i++) {
				int arg_reg = instr.operands[3 + i].reg_index();
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

		case IROpcode::GLOBAL_CALL: {
			const int result_reg = instr.operands[0].reg_index();
			const GlobalFn fn = static_cast<GlobalFn>(instr.operands[1].immediate());
			const int arg_count = static_cast<int>(instr.operands[3].immediate());

			std::vector<Value> args;
			args.reserve(arg_count);
			for (int i = 0; i < arg_count; i++) {
				args.push_back(get_register(ctx, instr.operands[4 + i].reg_index()));
			}

			const GlobalFunction* info = &global_function(fn);
			// All-String str(): plain concatenation, no host formatting needed.
			if (fn == GlobalFn::STR && !args.empty() &&
			    std::all_of(args.begin(), args.end(), [](const Value& v) {
					return std::holds_alternative<std::string>(v);
				})) {
				std::string joined;
				for (const Value& v : args) {
					joined += std::get<std::string>(v);
				}
				ctx.registers[result_reg] = std::move(joined);
				break;
			}
			if (info->kind == GlobalKind::HOST) {
				throw CompilerException(ErrorType::OPTIMIZER_ERROR,
					std::string(info->name) + "() needs the host Variant API and is not available"
					" in the IR interpreter (in function '" + func.name + "')");
			}
			if (info->impure) {
				// Impure globals (randi etc.) would diverge from the machine.
				throw CompilerException(ErrorType::OPTIMIZER_ERROR,
					std::string(info->name) + "() needs the host's random number generator"
					" and is not available in the IR interpreter (in function '" + func.name + "')");
			}

			// CAST globals. String-to-number needs host Variant::parse.
			if (info->kind == GlobalKind::CAST) {
				const Value& value = args.at(0);
				if (std::holds_alternative<std::string>(value) && fn != GlobalFn::TO_BOOL) {
					throw CompilerException(ErrorType::OPTIMIZER_ERROR,
						std::string(info->name) + "() of a String needs the host Variant API and is"
						" not available in the IR interpreter (in function '" + func.name + "')");
				}
				switch (fn) {
					case GlobalFn::TO_INT: ctx.registers[result_reg] = get_int(value); break;
					case GlobalFn::TO_FLOAT: ctx.registers[result_reg] = get_double(value); break;
						default: ctx.registers[result_reg] = get_bool(value); break;
				}
				break;
			}

			// Resolve NUMERIC form based on argument types, mirroring the backend.
			if (info->kind == GlobalKind::NUMERIC) {
				bool all_integer = true;
				for (const Value& value : args) {
					if (!std::holds_alternative<int64_t>(value)) {
						all_integer = false;
						break;
					}
				}
				info = &global_function(resolve_numeric_form(*info, all_integer));
			}

			if (info->kind == GlobalKind::INT_OP || info->kind == GlobalKind::SYSCALL_INT) {
				std::vector<int64_t> int_args;
				int_args.reserve(args.size());
				for (const Value& value : args) {
					int_args.push_back(get_int(value));
				}
				// SYSCALL_INT throws: the answer is the host's to give.
				ctx.registers[result_reg] = (info->kind == GlobalKind::INT_OP)
					? eval_global_int(info->fn, int_args.data(), int_args.size())
					: eval_global_int_syscall(info->fn, int_args.data(), int_args.size());
				break;
			}

			std::vector<double> float_args;
			float_args.reserve(args.size());
			for (const Value& value : args) {
				float_args.push_back(get_double(value));
			}
			const double result = eval_global_float(info->fn, float_args.data(), float_args.size());

			switch (info->result) {
				case GlobalResult::BOOL:
					ctx.registers[result_reg] = (result != 0.0);
					break;
				case GlobalResult::INT:
					ctx.registers[result_reg] = static_cast<int64_t>(result);
					break;
				case GlobalResult::NIL:
				case GlobalResult::FLOAT:
				case GlobalResult::STRING:
				case GlobalResult::NUMERIC:
				case GlobalResult::VARIANT:
					ctx.registers[result_reg] = result;
					break;
			}
			break;
		}

		case IROpcode::RETURN:
			ctx.returned = true;
			if (ctx.registers.find(0) != ctx.registers.end()) {
				ctx.return_value = ctx.registers[0];
			} else {
				ctx.return_value = int64_t(0);
			}
			break;

		// Host Variant API required. No default: — new opcodes must be listed explicitly.
		case IROpcode::ARRAY_APPEND:
		case IROpcode::ARRAY_GET:
		case IROpcode::ARRAY_SET:
		case IROpcode::DICT_SET:
		case IROpcode::DICT_GET_CONST:
		case IROpcode::DICT_SET_CONST:
		case IROpcode::DICT_HAS_CONST:
		case IROpcode::STRUCT_CHECK:
		case IROpcode::TRAIT_TEST:
		case IROpcode::CALL_SYSCALL:
		case IROpcode::MAKE_SCOPED:
		case IROpcode::BATCH_GET:
		case IROpcode::CODEPOINT_GET:
		case IROpcode::GET_NODE:
		case IROpcode::LOAD_RESOURCE:
		case IROpcode::LOAD_RESOURCE_VAR:
		case IROpcode::LOAD_STRING_AS:
		case IROpcode::MAKE_CALLABLE:
		case IROpcode::CONSTRUCT:
		case IROpcode::PRINT:
		case IROpcode::THROW:
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
		case IROpcode::MAKE_DICTIONARY_KEYED:
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
		// Variant::booleanize(): String is true iff non-empty.
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
	// String + String is concatenation; other ops on strings are invalid.
	if (is_string(left) || is_string(right)) {
		if (op == IROpcode::ADD && is_string(left) && is_string(right)) {
			return get_string(left) + get_string(right);
		}
		throw CompilerException(ErrorType::OPTIMIZER_ERROR,
			std::string("Operator ") + ir_opcode_name(op) + " is not defined on strings");
	}

	// Bitwise/shift: integer-only.
	switch (op) {
		case IROpcode::BIT_AND: return get_int(left) & get_int(right);
		case IROpcode::BIT_OR: return get_int(left) | get_int(right);
		case IROpcode::BIT_XOR: return get_int(left) ^ get_int(right);
		// Shift masked to 0-63, matching the RISC-V backend.
		case IROpcode::SHL:
			return static_cast<int64_t>(static_cast<uint64_t>(get_int(left)) << (get_int(right) & 63));
		case IROpcode::SHR:
			return get_int(left) >> (get_int(right) & 63);
		default:
			break;
	}

	// int op int → int; any float operand → float result.
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

	// Wrapping arithmetic (uint64_t), matching RISC-V `add` semantics.
	const uint64_t l = static_cast<uint64_t>(get_int(left));
	const uint64_t r = static_cast<uint64_t>(get_int(right));
	switch (op) {
		case IROpcode::ADD: return static_cast<int64_t>(l + r);
		case IROpcode::SUB: return static_cast<int64_t>(l - r);
		case IROpcode::MUL: return static_cast<int64_t>(l * r);
		// Godot: div/mod by zero → 0, INT64_MIN / -1 wraps.
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
			// INT64_MIN wraps, matching the machine.
			return static_cast<int64_t>(0u - static_cast<uint64_t>(get_int(operand)));
		case IROpcode::NOT:
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
	const bool left_nil = std::holds_alternative<std::monostate>(left);
	const bool right_nil = std::holds_alternative<std::monostate>(right);
	if (left_nil || right_nil) {
		if (op == IROpcode::CMP_EQ) return int64_t(left_nil && right_nil);
		if (op == IROpcode::CMP_NEQ) return int64_t(!(left_nil && right_nil));
		throw CompilerException(ErrorType::OPTIMIZER_ERROR,
			"Ordered comparison involving null");
	}

	if (is_string(left) || is_string(right)) {
		// String vs non-String: never equal, ordered comparison is invalid.
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
