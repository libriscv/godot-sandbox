// -= GDScript's global functions, in RISC-V =-
//
// One IR opcode, GLOBAL_CALL, carrying a GlobalFn. globals.h's table says what
// each one takes, returns, and is performed by; this file is the emission for
// each of those shapes:
//
//   INT_OP    64-bit integer arithmetic, inline
//   FLOAT_OP  double arithmetic, inline -- and only where the operation is a
//             single exact IEEE-754 primitive, so that what the machine
//             computes is what globals.cpp's evaluator computes, to the bit
//   SYSCALL   ECALL_UTILITY: arguments in fa0-fa4, the answer in fa0
//   SYSCALL_INT the same call with the arguments in a1-a3 and the answer in
//             a0, for the ops whose arguments are integers that a double
//             would not carry back unchanged
//   NUMERIC   neither, until run time: `abs(x)` is an integer when x is one
//             and a float otherwise, and when the compiler does not know which
//             it emits the type test and both forms
//   CAST      int(), float() and bool() of something whose type is not known:
//             the host converts it, because a String is one of the things it
//             might be
//   HOST      str(), len() and String(), which need the host's Variant API
//
// -= Registers =-
//
// Every virtual register lives in a Variant on the stack, so nothing is live in
// a physical register across an IR instruction and an expansion may use the
// temporaries freely. Arguments are loaded into t3-t5 (integer forms) or
// ft0-ft2 (floating-point forms), leaving t0-t2 as scratch. t6 belongs to the
// wide-offset load and store path and is never touched here.
#include "compiler_exception.h"
#include "riscv_codegen.h"
#include "syscall_numbers.h"

namespace gdscript {

void RISCVCodeGen::emit_global_call(const IRInstruction& instr) {
	// GLOBAL_CALL result_reg, global_fn, args_are_typed, arg_count, arg_reg...
	if (instr.operands.size() < 4) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "GLOBAL_CALL requires at least 4 operands");
	}

	const int result_vreg = std::get<int>(instr.operands[0].value);
	const GlobalFn fn = static_cast<GlobalFn>(std::get<int64_t>(instr.operands[1].value));
	const bool typed = std::get<int64_t>(instr.operands[2].value) != 0;
	const int arg_count = static_cast<int>(std::get<int64_t>(instr.operands[3].value));

	if (instr.operands.size() != static_cast<size_t>(4 + arg_count)) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "GLOBAL_CALL argument count mismatch");
	}

	std::vector<int> arg_offsets;
	arg_offsets.reserve(arg_count);
	for (int i = 0; i < arg_count; i++) {
		arg_offsets.push_back(get_variant_stack_offset(std::get<int>(instr.operands[4 + i].value)));
	}
	const int result_offset = get_variant_stack_offset(result_vreg);

	const GlobalFunction& info = global_function(fn);
	if (static_cast<size_t>(arg_count) < info.min_args || static_cast<size_t>(arg_count) > info.max_args) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			std::string(info.name) + " reached the backend with " + std::to_string(arg_count) + " arguments");
	}

	// ECALL_UTILITY reads a0-a3 and a7 and writes a0. Nothing else in this
	// backend keeps a virtual register in a physical one, so this only ever has
	// work to do if that changes -- but it has to be asked before the branch
	// below, never on one side of it, because the moves it hands back must run
	// on every path.
	const std::vector<uint8_t> clobbered_regs = { REG_A0, REG_A1, REG_A2, REG_A3 };
	const auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs, m_fn.current_instr_idx);
	for (const auto& move : moves) {
		emit_mv(move.second, move.first);
	}

	if (info.kind != GlobalKind::NUMERIC) {
		emit_global_form(info, arg_offsets, result_offset, typed);
		return;
	}

	// The type-preserving globals, with the types unknown until run time.
	const std::string label_float = gen_local_label(".global_float");
	const std::string label_done = gen_local_label(".global_done");

	emit_args_all_int(REG_T2, arg_offsets);
	mark_label_use(label_float, m_code.size());
	emit_beq(REG_T2, REG_ZERO, 0);

	// Every argument is an INT Variant, so the integer form's loads need no
	// type test of their own.
	emit_global_form(global_function(info.int_form), arg_offsets, result_offset, true);
	mark_label_use(label_done, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(label_float);
	emit_global_form(global_function(info.float_form), arg_offsets, result_offset, false);
	define_label(label_done);
}

void RISCVCodeGen::emit_global_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
	int result_offset, bool typed)
{
	switch (info.kind) {
		case GlobalKind::INT_OP:
			emit_global_int_form(info, arg_offsets, result_offset, typed);
			return;
		case GlobalKind::FLOAT_OP:
			emit_global_float_form(info, arg_offsets, result_offset, typed);
			return;
		case GlobalKind::SYSCALL:
			emit_global_syscall_form(info, arg_offsets, result_offset, typed);
			return;
		case GlobalKind::SYSCALL_INT:
			emit_global_int_syscall_form(info, arg_offsets, result_offset, typed);
			return;
		// A CAST that reached the backend is one the compiler could not resolve
		// to an inline form, so the argument may be anything a Variant holds
		// and the host performs the conversion -- over the Variant itself,
		// which is the same way str() travels.
		case GlobalKind::CAST:
		case GlobalKind::HOST:
			emit_global_host_form(info, arg_offsets, result_offset);
			return;
		case GlobalKind::PRINT:
		case GlobalKind::NUMERIC:
			break;
	}
	throw CompilerException(ErrorType::RISCV_codegen_ERROR,
		std::string(info.name) + " has no concrete form to emit");
}

// -= Loading arguments =-

void RISCVCodeGen::emit_variant_to_double(uint8_t fd, int variant_offset, bool known_float) {
	if (known_float) {
		// Variant::FLOAT is a 64-bit double whatever real_t is.
		emit_fld(fd, REG_SP, variant_offset + VARIANT_DATA_OFFSET);
		return;
	}

	// The type is not known, so the conversion is. INT and BOOL widen, FLOAT
	// loads, and anything else -- a string, an array, NIL -- reads as zero,
	// which is what Variant::operator double() does for the types it does not
	// convert. Godot would raise a call error instead; the sandbox has nowhere
	// to raise it to that would not also stop the program.
	const std::string label_int = gen_local_label(".to_double_int");
	const std::string label_bool = gen_local_label(".to_double_bool");
	const std::string label_float = gen_local_label(".to_double_float");
	const std::string label_done = gen_local_label(".to_double_done");

	emit_lwu(REG_T0, REG_SP, variant_offset);

	emit_addi(REG_T1, REG_T0, -Variant::FLOAT);
	mark_label_use(label_float, m_code.size());
	emit_beq(REG_T1, REG_ZERO, 0);

	emit_addi(REG_T1, REG_T0, -Variant::INT);
	mark_label_use(label_int, m_code.size());
	emit_beq(REG_T1, REG_ZERO, 0);

	emit_addi(REG_T1, REG_T0, -Variant::BOOL);
	mark_label_use(label_bool, m_code.size());
	emit_beq(REG_T1, REG_ZERO, 0);

	// Not a number.
	emit_fcvt_d_l(fd, REG_ZERO);
	mark_label_use(label_done, m_code.size());
	emit_jal(REG_ZERO, 0);

	// A BOOL Variant is one byte; the seven above it are whatever the slot held
	// before, so reading eight makes `false` read as some other number.
	define_label(label_bool);
	emit_lbu(REG_T0, REG_SP, variant_offset + VARIANT_DATA_OFFSET);
	emit_fcvt_d_l(fd, REG_T0);
	mark_label_use(label_done, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(label_int);
	emit_ld(REG_T0, REG_SP, variant_offset + VARIANT_DATA_OFFSET);
	emit_fcvt_d_l(fd, REG_T0);
	mark_label_use(label_done, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(label_float);
	emit_fld(fd, REG_SP, variant_offset + VARIANT_DATA_OFFSET);

	define_label(label_done);
}

void RISCVCodeGen::emit_variant_to_int(uint8_t rd, int variant_offset, bool known_int) {
	if (known_int) {
		emit_ld(rd, REG_SP, variant_offset + VARIANT_DATA_OFFSET);
		return;
	}

	// Same shape as emit_variant_to_double, the other way round: a FLOAT
	// truncates toward zero, a BOOL is its low byte, and anything else is zero.
	const std::string label_int = gen_local_label(".to_int_int");
	const std::string label_bool = gen_local_label(".to_int_bool");
	const std::string label_float = gen_local_label(".to_int_float");
	const std::string label_done = gen_local_label(".to_int_done");

	emit_lwu(REG_T0, REG_SP, variant_offset);

	emit_addi(REG_T1, REG_T0, -Variant::INT);
	mark_label_use(label_int, m_code.size());
	emit_beq(REG_T1, REG_ZERO, 0);

	emit_addi(REG_T1, REG_T0, -Variant::FLOAT);
	mark_label_use(label_float, m_code.size());
	emit_beq(REG_T1, REG_ZERO, 0);

	emit_addi(REG_T1, REG_T0, -Variant::BOOL);
	mark_label_use(label_bool, m_code.size());
	emit_beq(REG_T1, REG_ZERO, 0);

	emit_li(rd, 0);
	mark_label_use(label_done, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(label_bool);
	emit_lbu(rd, REG_SP, variant_offset + VARIANT_DATA_OFFSET);
	mark_label_use(label_done, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(label_float);
	emit_fld(REG_FA0, REG_SP, variant_offset + VARIANT_DATA_OFFSET);
	emit_fcvt_l_d(rd, REG_FA0);
	mark_label_use(label_done, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(label_int);
	emit_ld(rd, REG_SP, variant_offset + VARIANT_DATA_OFFSET);

	define_label(label_done);
}

void RISCVCodeGen::emit_args_all_int(uint8_t rd, const std::vector<int>& arg_offsets) {
	emit_li(rd, 1);
	for (int offset : arg_offsets) {
		emit_lwu(REG_T0, REG_SP, offset);
		emit_addi(REG_T0, REG_T0, -Variant::INT);
		emit_seqz(REG_T0, REG_T0);
		emit_and(rd, rd, REG_T0);
	}
}

// -= Storing the result =-

void RISCVCodeGen::emit_global_double_result(int result_offset, uint8_t fs, GlobalResult result) {
	switch (result) {
		case GlobalResult::INT:
			// floori(), ceili(), roundi() and snappedi(): the host rounds, and
			// the truncation that follows only drops a fractional part that is
			// already zero.
			emit_fcvt_l_d(REG_T1, fs);
			emit_li(REG_T0, Variant::INT);
			emit_store_variant_type(REG_T0, REG_SP, result_offset);
			emit_store_variant_int(REG_T1, REG_SP, result_offset);
			return;
		case GlobalResult::BOOL:
			// The predicates answer 0.0 or 1.0.
			emit_fcvt_l_d(REG_T1, fs);
			emit_li(REG_T0, Variant::BOOL);
			emit_store_variant_type(REG_T0, REG_SP, result_offset);
			emit_store_variant_bool(REG_T1, REG_SP, result_offset);
			return;
		case GlobalResult::FLOAT:
			emit_li(REG_T0, Variant::FLOAT);
			emit_store_variant_type(REG_T0, REG_SP, result_offset);
			emit_fsd(fs, REG_SP, result_offset + VARIANT_DATA_OFFSET);
			return;
		case GlobalResult::NIL:
		case GlobalResult::STRING:
		case GlobalResult::NUMERIC:
			break;
	}
	throw CompilerException(ErrorType::RISCV_codegen_ERROR,
		"A global computing a double cannot return that result type");
}

void RISCVCodeGen::emit_global_int_result(int result_offset, uint8_t rs, GlobalResult result) {
	switch (result) {
		case GlobalResult::INT:
			emit_li(REG_T0, Variant::INT);
			emit_store_variant_type(REG_T0, REG_SP, result_offset);
			emit_store_variant_int(rs, REG_SP, result_offset);
			return;
		case GlobalResult::BOOL:
			emit_li(REG_T0, Variant::BOOL);
			emit_store_variant_type(REG_T0, REG_SP, result_offset);
			emit_store_variant_bool(rs, REG_SP, result_offset);
			return;
		case GlobalResult::NIL:
		case GlobalResult::FLOAT:
		case GlobalResult::STRING:
		case GlobalResult::NUMERIC:
			break;
	}
	throw CompilerException(ErrorType::RISCV_codegen_ERROR,
		"A global computing an integer cannot return that result type");
}

// -= Integer forms =-

void RISCVCodeGen::emit_global_int_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
	int result_offset, bool typed)
{
	// Where an integer form finds its arguments. Three is the widest one gets:
	// clampi(value, min, max) and wrapi(value, min, max).
	const uint8_t INT_ARG_REGS[3] = { REG_T3, REG_T4, REG_T5 };
	for (size_t i = 0; i < arg_offsets.size(); i++) {
		emit_variant_to_int(INT_ARG_REGS[i], arg_offsets[i], typed);
	}
	const uint8_t a = INT_ARG_REGS[0];
	const uint8_t b = INT_ARG_REGS[1];
	const uint8_t c = INT_ARG_REGS[2];

	// The answer is built in t0.
	switch (info.fn) {
		case GlobalFn::INT_IDENTITY:
			// floor(), ceil() and round() of an integer.
			emit_mv(REG_T0, a);
			break;

		case GlobalFn::ABSI: {
			// (x ^ (x >> 63)) - (x >> 63): the sign-mask form, which wraps on
			// INT64_MIN rather than being undefined the way std::abs() is.
			emit_srai(REG_T1, a, 63);
			emit_xor(REG_T0, a, REG_T1);
			emit_sub(REG_T0, REG_T0, REG_T1);
			break;
		}

		case GlobalFn::SIGNI:
			// (0 < x) - (x < 0)
			emit_slt(REG_T0, REG_ZERO, a);
			emit_slt(REG_T1, a, REG_ZERO);
			emit_sub(REG_T0, REG_T0, REG_T1);
			break;

		case GlobalFn::MINI:
		case GlobalFn::MAXI: {
			const std::string label_first = gen_local_label(".pick_first");
			const std::string label_done = gen_local_label(".pick_done");
			// min: a < b takes a. max: b < a takes a.
			mark_label_use(label_first, m_code.size());
			if (info.fn == GlobalFn::MINI) {
				emit_blt(a, b, 0);
			} else {
				emit_blt(b, a, 0);
			}
			emit_mv(REG_T0, b);
			mark_label_use(label_done, m_code.size());
			emit_jal(REG_ZERO, 0);
			define_label(label_first);
			emit_mv(REG_T0, a);
			define_label(label_done);
			break;
		}

		case GlobalFn::CLAMPI: {
			// if (x < min) min; else if (x > max) max; else x
			const std::string label_low = gen_local_label(".clamp_low");
			const std::string label_high = gen_local_label(".clamp_high");
			const std::string label_done = gen_local_label(".clamp_done");
			mark_label_use(label_low, m_code.size());
			emit_blt(a, b, 0);
			mark_label_use(label_high, m_code.size());
			emit_blt(c, a, 0);
			emit_mv(REG_T0, a);
			mark_label_use(label_done, m_code.size());
			emit_jal(REG_ZERO, 0);
			define_label(label_low);
			emit_mv(REG_T0, b);
			mark_label_use(label_done, m_code.size());
			emit_jal(REG_ZERO, 0);
			define_label(label_high);
			emit_mv(REG_T0, c);
			define_label(label_done);
			break;
		}

		case GlobalFn::POSMOD: {
			// value = x % y; if value and y have opposite signs, value += y.
			//
			// A zero divisor answers zero, the way integer division by zero
			// does here. Godot leaves it to the C++ `%`, which traps; a trap
			// inside the sandbox would stop the program.
			const std::string label_zero = gen_local_label(".posmod_zero");
			const std::string label_done = gen_local_label(".posmod_done");
			mark_label_use(label_zero, m_code.size());
			emit_beq(b, REG_ZERO, 0);

			emit_rem(REG_T0, a, b);
			// Nothing to correct when the remainder is already zero.
			mark_label_use(label_done, m_code.size());
			emit_beq(REG_T0, REG_ZERO, 0);
			// Same sign means the exclusive or is non-negative.
			emit_xor(REG_T1, REG_T0, b);
			mark_label_use(label_done, m_code.size());
			emit_bge(REG_T1, REG_ZERO, 0);
			emit_add(REG_T0, REG_T0, b);
			mark_label_use(label_done, m_code.size());
			emit_jal(REG_ZERO, 0);

			define_label(label_zero);
			emit_li(REG_T0, 0);
			define_label(label_done);
			break;
		}

		case GlobalFn::WRAPI: {
			// range = max - min
			// range == 0 ? min : min + (((value - min) % range + range) % range)
			const std::string label_empty = gen_local_label(".wrap_empty");
			const std::string label_done = gen_local_label(".wrap_done");
			emit_sub(REG_T1, c, b);
			mark_label_use(label_empty, m_code.size());
			emit_beq(REG_T1, REG_ZERO, 0);

			emit_sub(REG_T2, a, b);
			emit_rem(REG_T2, REG_T2, REG_T1);
			emit_add(REG_T2, REG_T2, REG_T1);
			emit_rem(REG_T2, REG_T2, REG_T1);
			emit_add(REG_T0, b, REG_T2);
			mark_label_use(label_done, m_code.size());
			emit_jal(REG_ZERO, 0);

			define_label(label_empty);
			emit_mv(REG_T0, b);
			define_label(label_done);
			break;
		}

		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				std::string(info.name) + " is not an integer operation");
	}

	emit_li(REG_T1, Variant::INT);
	emit_store_variant_type(REG_T1, REG_SP, result_offset);
	emit_store_variant_int(REG_T0, REG_SP, result_offset);
}

// -= Floating-point forms emitted inline =-

void RISCVCodeGen::emit_global_float_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
	int result_offset, bool typed)
{
	// ft0-ft2 hold the arguments and ft3 the answer.
	for (size_t i = 0; i < arg_offsets.size(); i++) {
		emit_variant_to_double(static_cast<uint8_t>(REG_FA0 + i), arg_offsets[i], typed);
	}
	const uint8_t a = REG_FA0;
	const uint8_t b = REG_FA1;
	const uint8_t c = REG_FA2;
	const uint8_t out = REG_FA3;

	switch (info.fn) {
		case GlobalFn::ABSF:
			emit_fabs_d(out, a);
			break;

		case GlobalFn::SQRT:
			emit_fsqrt_d(out, a);
			break;

		case GlobalFn::FLOAT_IDENTITY:
			// float() of a number: the load above is the conversion.
			emit_fmv_d(out, a);
			break;

		case GlobalFn::BOOLEANIZE: {
			// bool() of a number, which is Variant::booleanize(): true for
			// anything but zero. FEQ.D is false on a NaN, so `not (x == 0)`
			// makes bool(NAN) true the way Godot does; `x != 0` written as a
			// comparison would make it false.
			emit_fcvt_d_l(REG_FA1, REG_ZERO);
			emit_feq_d(REG_T0, a, REG_FA1);
			emit_xori(REG_T0, REG_T0, 1);
			emit_fcvt_d_l(out, REG_T0);
			break;
		}

		case GlobalFn::MINF:
		case GlobalFn::MAXF: {
			// `a < b ? a : b`, not FMIN.D: Godot's MIN() is the comparison, and
			// the two disagree about NaN.
			const std::string label_first = gen_local_label(".fpick_first");
			const std::string label_done = gen_local_label(".fpick_done");
			if (info.fn == GlobalFn::MINF) {
				emit_flt_d(REG_T0, a, b);
			} else {
				emit_flt_d(REG_T0, b, a);
			}
			mark_label_use(label_first, m_code.size());
			emit_bne(REG_T0, REG_ZERO, 0);
			emit_fmv_d(out, b);
			mark_label_use(label_done, m_code.size());
			emit_jal(REG_ZERO, 0);
			define_label(label_first);
			emit_fmv_d(out, a);
			define_label(label_done);
			break;
		}

		case GlobalFn::CLAMPF: {
			const std::string label_low = gen_local_label(".fclamp_low");
			const std::string label_high = gen_local_label(".fclamp_high");
			const std::string label_done = gen_local_label(".fclamp_done");
			emit_flt_d(REG_T0, a, b);
			mark_label_use(label_low, m_code.size());
			emit_bne(REG_T0, REG_ZERO, 0);
			emit_flt_d(REG_T0, c, a);
			mark_label_use(label_high, m_code.size());
			emit_bne(REG_T0, REG_ZERO, 0);
			emit_fmv_d(out, a);
			mark_label_use(label_done, m_code.size());
			emit_jal(REG_ZERO, 0);
			define_label(label_low);
			emit_fmv_d(out, b);
			mark_label_use(label_done, m_code.size());
			emit_jal(REG_ZERO, 0);
			define_label(label_high);
			emit_fmv_d(out, c);
			define_label(label_done);
			break;
		}

		default:
			throw CompilerException(ErrorType::RISCV_codegen_ERROR,
				std::string(info.name) + " is not an inline floating-point operation");
	}

	emit_global_double_result(result_offset, out, info.result);
}

// -= Forms the host performs =-

void RISCVCodeGen::emit_global_syscall_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
	int result_offset, bool typed)
{
	if (info.float_args > UTILITY_MAX_FLOAT_ARGS || arg_offsets.size() != info.float_args) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			std::string(info.name) + " has the wrong number of floating-point arguments");
	}

	// The arguments go straight into the ABI's fa0-fa4. Each conversion writes
	// only its own destination, so filling them in order is safe.
	for (size_t i = 0; i < arg_offsets.size(); i++) {
		emit_variant_to_double(static_cast<uint8_t>(REG_ABI_FA0 + i), arg_offsets[i], typed);
	}

	emit_li(REG_A0, info.utility_op);
	emit_li(REG_A7, ECALL_UTILITY);
	emit_ecall();

	// The host answers in fa0.
	emit_global_double_result(result_offset, REG_ABI_FA0, info.result);
}

void RISCVCodeGen::emit_global_int_syscall_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
	int result_offset, bool typed)
{
	if (arg_offsets.size() > UTILITY_MAX_INT_ARGS) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			std::string(info.name) + " has more integer arguments than a1-a3 can carry");
	}

	// a0 holds the op, so the arguments start at a1. Each conversion writes
	// only its own destination and uses t0, t1 and fa0 as scratch, so filling
	// them in order is safe.
	const uint8_t INT_ARG_REGS[UTILITY_MAX_INT_ARGS] = { REG_A1, REG_A2, REG_A3 };
	for (size_t i = 0; i < arg_offsets.size(); i++) {
		emit_variant_to_int(INT_ARG_REGS[i], arg_offsets[i], typed);
	}

	emit_li(REG_A0, info.utility_op);
	emit_li(REG_A7, ECALL_UTILITY);
	emit_ecall();

	// The host answers in a0.
	emit_global_int_result(result_offset, REG_A0, info.result);
}

void RISCVCodeGen::emit_global_host_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
	int result_offset)
{
	// str() and len() take Variants, not numbers, so this passes the arguments
	// the way print() does: copied into one contiguous run below sp, which the
	// host walks.
	const int arg_count = static_cast<int>(arg_offsets.size());
	int args_space = arg_count * variant_size();
	args_space = (args_space + 15) & ~15; // Keep sp 16-byte aligned

	emit_add_offset(REG_SP, REG_SP, -args_space);

	// Every offset below is into the frame sp just moved away from, so it reads
	// back at +args_space.
	for (int i = 0; i < arg_count; i++) {
		emit_variant_move(REG_SP, i * variant_size(), REG_SP, arg_offsets[i] + args_space, REG_T0);
	}

	emit_li(REG_A0, info.utility_op);
	emit_add_offset(REG_A1, REG_SP, result_offset + args_space); // where to write the answer
	emit_mv(REG_A2, REG_SP);                                     // the arguments
	emit_li(REG_A3, arg_count);
	emit_li(REG_A7, ECALL_UTILITY);
	emit_ecall();

	emit_add_offset(REG_SP, REG_SP, args_space);
}

} // namespace gdscript
