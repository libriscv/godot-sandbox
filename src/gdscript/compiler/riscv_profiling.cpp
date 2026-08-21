#include "riscv_codegen.h"

namespace gdscript {

// Frameless: shadow stack is in the profiling area, addressed absolutely.
//
// Per call: entry pushes {stamp, child=0}; exit computes elapsed,
// record.total += elapsed, record.self += elapsed - child, caller.child += elapsed.
// Instrumentation overhead is charged to the caller through caller.child.

void RISCVCodeGen::emit_csrr(uint8_t rd, uint32_t csr) {
	// csrrs rd, csr, x0
	emit_word(0x73 | (uint32_t(rd) << 7) | (0x2 << 12) | ((csr & 0xFFF) << 20));
}

void RISCVCodeGen::emit_profiling_entry() {
	const uint32_t csr = m_profiling_clock == ProfilingClock::INSTRUCTIONS ? 0xC00 : 0xC01;
	const int32_t shadow = ProfilingLayout::shadow_offset(uint32_t(m_profiling_count));
	const std::string label_over = gen_local_label("prof_over");
	const std::string label_done = gen_local_label("prof_done");

	emit_la(REG_T0, PROFILING_LABEL);
	emit_ld(REG_T1, REG_T0, ProfilingLayout::DEPTH_OFF);
	emit_addi(REG_T2, REG_T1, 1);
	emit_sd(REG_T2, REG_T0, ProfilingLayout::DEPTH_OFF);

	emit_li(REG_T2, ProfilingLayout::MAX_DEPTH);
	mark_label_use(label_over, m_code.size());
	emit_bgeu(REG_T1, REG_T2, 0);

	emit_slli(REG_T1, REG_T1, uint8_t(ProfilingLayout::FRAME_SHIFT));
	emit_add(REG_T1, REG_T1, REG_T0);
	emit_csrr(REG_T2, csr);
	emit_sd(REG_T2, REG_T1, shadow + ProfilingLayout::ENTRY_STAMP_OFF);
	emit_sd(REG_ZERO, REG_T1, shadow + ProfilingLayout::CHILD_OFF);
	mark_label_use(label_done, m_code.size());
	emit_jal(REG_ZERO, 0);

	define_label(label_over);
	emit_ld(REG_T1, REG_T0, ProfilingLayout::OVERFLOW_OFF);
	emit_addi(REG_T1, REG_T1, 1);
	emit_sd(REG_T1, REG_T0, ProfilingLayout::OVERFLOW_OFF);

	define_label(label_done);
}

void RISCVCodeGen::emit_profiling_exit() {
	const uint32_t csr = m_profiling_clock == ProfilingClock::INSTRUCTIONS ? 0xC00 : 0xC01;
	const int32_t shadow = ProfilingLayout::shadow_offset(uint32_t(m_profiling_count));
	const int32_t record = ProfilingLayout::record_offset(uint32_t(m_profiling_index));
	const std::string label_done = gen_local_label("prof_done");

	emit_la(REG_T0, PROFILING_LABEL);
	emit_ld(REG_T1, REG_T0, ProfilingLayout::DEPTH_OFF);
	emit_addi(REG_T1, REG_T1, -1);
	emit_sd(REG_T1, REG_T0, ProfilingLayout::DEPTH_OFF);

	// Unsigned: underflow from exception unwind wraps high and skips like overflow.
	emit_li(REG_T2, ProfilingLayout::MAX_DEPTH);
	mark_label_use(label_done, m_code.size());
	emit_bgeu(REG_T1, REG_T2, 0);

	emit_slli(REG_T2, REG_T1, uint8_t(ProfilingLayout::FRAME_SHIFT));
	emit_add(REG_T2, REG_T2, REG_T0);
	emit_csrr(REG_T3, csr);
	emit_ld(REG_T4, REG_T2, shadow + ProfilingLayout::ENTRY_STAMP_OFF);
	emit_sub(REG_T3, REG_T3, REG_T4); // elapsed
	emit_ld(REG_T4, REG_T2, shadow + ProfilingLayout::CHILD_OFF);
	emit_sub(REG_T4, REG_T3, REG_T4); // self = elapsed - children

	emit_ld(REG_T5, REG_T0, record + ProfilingLayout::CALL_COUNT_OFF);
	emit_addi(REG_T5, REG_T5, 1);
	emit_sd(REG_T5, REG_T0, record + ProfilingLayout::CALL_COUNT_OFF);

	emit_ld(REG_T5, REG_T0, record + ProfilingLayout::TOTAL_OFF);
	emit_add(REG_T5, REG_T5, REG_T3);
	emit_sd(REG_T5, REG_T0, record + ProfilingLayout::TOTAL_OFF);

	emit_ld(REG_T5, REG_T0, record + ProfilingLayout::SELF_OFF);
	emit_add(REG_T5, REG_T5, REG_T4);
	emit_sd(REG_T5, REG_T0, record + ProfilingLayout::SELF_OFF);

	mark_label_use(label_done, m_code.size());
	emit_beq(REG_T1, REG_ZERO, 0);

	const int32_t caller_child =
		shadow + ProfilingLayout::CHILD_OFF - ProfilingLayout::FRAME_SIZE;
	emit_ld(REG_T5, REG_T2, caller_child);
	emit_add(REG_T5, REG_T5, REG_T3);
	emit_sd(REG_T5, REG_T2, caller_child);

	define_label(label_done);
}

} // namespace gdscript
