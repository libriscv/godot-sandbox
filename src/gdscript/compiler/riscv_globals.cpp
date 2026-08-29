// RISC-V emission for GLOBAL_CALL. See globals.h for the table.
//
// Register convention: arguments in t3-t5 (int) or ft0-ft2 (float),
// t0-t2 scratch, t6 reserved for wide-offset paths.
#include "compiler_exception.h"
#include "riscv_codegen.h"
#include "syscall_numbers.h"

namespace gdscript {

void RISCVCodeGen::emit_global_call(const IRInstruction& instr) {
	// GLOBAL_CALL result_reg, global_fn, args_are_typed, arg_count, arg_reg...
	if (instr.operands.size() < 4) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "GLOBAL_CALL requires at least 4 operands");
	}

	const int result_vreg = instr.operands[0].reg_index();
	const GlobalFn fn = static_cast<GlobalFn>(instr.operands[1].immediate());
	const bool typed = instr.operands[2].immediate() != 0;
	const int arg_count = static_cast<int>(instr.operands[3].immediate());

	if (instr.operands.size() != static_cast<size_t>(4 + arg_count)) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR, "GLOBAL_CALL argument count mismatch");
	}

	std::vector<int> arg_offsets;
	arg_offsets.reserve(arg_count);
	for (int i = 0; i < arg_count; i++) {
		arg_offsets.push_back(get_variant_stack_offset(instr.operands[4 + i].reg_index()));
	}
	const int result_offset = get_variant_stack_offset(result_vreg);

	const GlobalFunction& info = global_function(fn);
	if (static_cast<size_t>(arg_count) < info.min_args || static_cast<size_t>(arg_count) > info.max_args) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			std::string(info.name) + " reached the backend with " + std::to_string(arg_count) + " arguments");
	}

	// Inline integer/float forms never touch ABI argument registers.  Evict them
	// only when one of the possible forms can actually enter the host.
	if (global_call_may_ecall(fn)) {
		const std::vector<uint8_t> clobbered_regs = { REG_A0, REG_A1, REG_A2, REG_A3 };
		const auto moves = m_allocator.handle_syscall_clobbering(clobbered_regs,
			m_fn.current_instr_idx);
		for (const auto& move : moves) {
			emit_mv(move.second, move.first);
		}
	}

	if (info.kind != GlobalKind::NUMERIC) {
		emit_global_form(info, arg_offsets, result_offset, typed);
		return;
	}

	// NUMERIC dispatch: run-time type test.
	const std::string label_float = gen_local_label(".global_float");
	const std::string label_done = gen_local_label(".global_done");

	emit_args_all_int(REG_T2, arg_offsets);
	mark_label_use(label_float, m_code.size());
	emit_beq(REG_T2, REG_ZERO, 0);

	// All-INT path: skip type tests in the integer form.
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
		// Unresolved CAST: argument type unknown, host performs conversion.
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

void RISCVCodeGen::emit_variant_to_double(uint8_t fd, int variant_offset, bool known_float) {
	if (known_float) {
		emit_fld(fd, REG_SP, variant_offset + VARIANT_DATA_OFFSET);
		return;
	}

	// Untyped: INT/BOOL widen, FLOAT loads, everything else → 0.0
	// (matches Variant::operator double()).
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

	emit_fcvt_d_l(fd, REG_ZERO);
	mark_label_use(label_done, m_code.size());
	emit_jal(REG_ZERO, 0);

	// BOOL payload is 1 byte; lbu to avoid reading stale upper bytes.
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

	// Untyped: FLOAT truncates toward zero, BOOL is low byte, else 0.
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

void RISCVCodeGen::emit_global_double_result(int result_offset, uint8_t fs, GlobalResult result) {
	switch (result) {
		case GlobalResult::INT:
			emit_fcvt_l_d(REG_T1, fs);
			emit_li(REG_T0, Variant::INT);
			emit_store_variant_type(REG_T0, REG_SP, result_offset);
			emit_store_variant_int(REG_T1, REG_SP, result_offset);
			return;
		case GlobalResult::BOOL:
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
		case GlobalResult::VARIANT:
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
		case GlobalResult::VARIANT:
			break;
	}
	throw CompilerException(ErrorType::RISCV_codegen_ERROR,
		"A global computing an integer cannot return that result type");
}

void RISCVCodeGen::emit_global_int_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
	int result_offset, bool typed)
{
	// Up to 3 args: clampi/wrapi are the widest.
	const uint8_t INT_ARG_REGS[3] = { REG_T3, REG_T4, REG_T5 };
	for (size_t i = 0; i < arg_offsets.size(); i++) {
		emit_variant_to_int(INT_ARG_REGS[i], arg_offsets[i], typed);
	}
	const uint8_t a = INT_ARG_REGS[0];
	const uint8_t b = INT_ARG_REGS[1];
	const uint8_t c = INT_ARG_REGS[2];

	// Result in t0.
	switch (info.fn) {
		case GlobalFn::INT_IDENTITY:
			emit_mv(REG_T0, a);
			break;

		case GlobalFn::ABSI: {
			// Sign-mask: wraps on INT64_MIN (std::abs is UB there).
			emit_srai(REG_T1, a, 63);
			emit_xor(REG_T0, a, REG_T1);
			emit_sub(REG_T0, REG_T0, REG_T1);
			break;
		}

		case GlobalFn::SIGNI:
			emit_slt(REG_T0, REG_ZERO, a);
			emit_slt(REG_T1, a, REG_ZERO);
			emit_sub(REG_T0, REG_T0, REG_T1);
			break;

		case GlobalFn::MINI:
		case GlobalFn::MAXI: {
			const std::string label_first = gen_local_label(".pick_first");
			const std::string label_done = gen_local_label(".pick_done");
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
			// Zero divisor → 0 (C++ % traps; sandbox cannot trap usefully).
			const std::string label_zero = gen_local_label(".posmod_zero");
			const std::string label_done = gen_local_label(".posmod_done");
			mark_label_use(label_zero, m_code.size());
			emit_beq(b, REG_ZERO, 0);

			emit_rem(REG_T0, a, b);
			mark_label_use(label_done, m_code.size());
			emit_beq(REG_T0, REG_ZERO, 0);
			// Opposite signs: adjust remainder.
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

void RISCVCodeGen::emit_global_float_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
	int result_offset, bool typed)
{
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
			emit_fmv_d(out, a);
			break;

		case GlobalFn::BOOLEANIZE: {
			// !(x == 0.0): FEQ.D is false for NaN, so bool(NaN) == true.
			emit_fcvt_d_l(REG_FA1, REG_ZERO);
			emit_feq_d(REG_T0, a, REG_FA1);
			emit_xori(REG_T0, REG_T0, 1);
			emit_fcvt_d_l(out, REG_T0);
			break;
		}

		case GlobalFn::MINF:
		case GlobalFn::MAXF: {
			// Comparison, not FMIN.D — Godot's MIN/MAX and FMIN.D disagree on NaN.
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

void RISCVCodeGen::emit_global_syscall_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
	int result_offset, bool typed)
{
	if (info.float_args > UTILITY_MAX_FLOAT_ARGS || arg_offsets.size() != info.float_args) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			std::string(info.name) + " has the wrong number of floating-point arguments");
	}

	for (size_t i = 0; i < arg_offsets.size(); i++) {
		emit_variant_to_double(static_cast<uint8_t>(REG_ABI_FA0 + i), arg_offsets[i], typed);
	}

	emit_li(REG_A0, info.utility_op);
	emit_li(REG_A7, ECALL_UTILITY);
	emit_ecall();

	emit_global_double_result(result_offset, REG_ABI_FA0, info.result);
}

void RISCVCodeGen::emit_global_int_syscall_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
	int result_offset, bool typed)
{
	if (arg_offsets.size() > UTILITY_MAX_INT_ARGS) {
		throw CompilerException(ErrorType::RISCV_codegen_ERROR,
			std::string(info.name) + " has more integer arguments than a1-a3 can carry");
	}

	// a0 = op, args in a1-a3.
	const uint8_t INT_ARG_REGS[UTILITY_MAX_INT_ARGS] = { REG_A1, REG_A2, REG_A3 };
	for (size_t i = 0; i < arg_offsets.size(); i++) {
		emit_variant_to_int(INT_ARG_REGS[i], arg_offsets[i], typed);
	}

	emit_li(REG_A0, info.utility_op);
	emit_li(REG_A7, ECALL_UTILITY);
	emit_ecall();

	emit_global_int_result(result_offset, REG_A0, info.result);
}

void RISCVCodeGen::emit_global_host_form(const GlobalFunction& info, const std::vector<int>& arg_offsets,
	int result_offset)
{
	// Variant args copied contiguously below sp, same as print().
	const int arg_count = static_cast<int>(arg_offsets.size());
	int args_space = arg_count * variant_size();
	args_space = (args_space + 15) & ~15;

	emit_add_offset(REG_SP, REG_SP, -args_space);

	// Original frame offsets shifted by args_space.
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
