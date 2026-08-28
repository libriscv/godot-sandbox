#include "riscv_codegen.h"
#include "syscall_numbers.h"
#include <algorithm>

namespace gdscript {

// Shadow stack (debug builds only) and address-to-line table (every build).
// One push/pop per call, nothing per statement; outer frame lines are
// resolved from return addresses through the line table.

void RISCVCodeGen::record_line(int32_t line, bool force) {
	// Line 0 continues the previous row unless forced (function entry only).
	if (line < 0 || (line == 0 && !force)) {
		return;
	}
	const uint32_t address = uint32_t(m_code.size());
	auto& entries = m_line_table.entries;

	if (!entries.empty()) {
		// Same address: overwrite. Same line: coalesce.
		if (entries.back().address == address) {
			entries.back().line = uint32_t(line);
		} else if (entries.back().line == uint32_t(line)) {
			return;
		} else {
			entries.push_back(LineTableEntry{ address, uint32_t(line) });
		}
		// Deduplicate trailing pair.
		if (entries.size() >= 2 && entries[entries.size() - 2].line == entries.back().line) {
			entries.pop_back();
		}
		return;
	}

	entries.push_back(LineTableEntry{ address, uint32_t(line) });
}

// Uses t0-t2 (dead at entry). Runs after frame setup, before ra is spilled.
void RISCVCodeGen::emit_debug_entry() {
	const int32_t frames = DebugLayout::frames_offset();
	const std::string label_over = gen_local_label("dbg_over");

	emit_la(REG_T0, DEBUG_LABEL);
	emit_ld(REG_T1, REG_T0, DebugLayout::DEPTH_OFF);
	emit_addi(REG_T2, REG_T1, 1);
	emit_sd(REG_T2, REG_T0, DebugLayout::DEPTH_OFF);

	// Overflow: count but don't record. Unsigned catches underflow too.
	emit_li(REG_T2, DebugLayout::MAX_DEPTH);
	mark_label_use(label_over, m_code.size());
	emit_bgeu(REG_T1, REG_T2, 0);

	emit_slli(REG_T1, REG_T1, uint8_t(DebugLayout::FRAME_SHIFT));
	emit_add(REG_T1, REG_T1, REG_T0);
	emit_li(REG_T2, m_debug_index);
	emit_sd(REG_T2, REG_T1, frames + DebugLayout::FUNCTION_INDEX_OFF);
	emit_sd(REG_RA, REG_T1, frames + DebugLayout::RETURN_ADDRESS_OFF);
	emit_sd(REG_SP, REG_T1, frames + DebugLayout::FRAME_SP_OFF);
	emit_sd(REG_TP, REG_T1, frames + DebugLayout::INSTANCE_BASE_OFF);

	define_label(label_over);
}

// Saves/restores a0-a2 and a7 around ECALL_BREAKPOINT.
void RISCVCodeGen::emit_breakpoint(int32_t line, bool installed, bool user_stop,
		bool source_stop) {
	// Maintain ascending, deduplicated installed-breakpoint list.
	if (installed) {
		const auto at = std::lower_bound(m_installed_breakpoints.begin(),
			m_installed_breakpoints.end(), uint32_t(line));
		if (at == m_installed_breakpoints.end() || *at != uint32_t(line)) {
			m_installed_breakpoints.insert(at, uint32_t(line));
		}
	}

	m_emitting_breakpoint = true;
	// The debugger reads named values from their canonical frame slots.
	spill_all_registers();
	emit_addi(REG_SP, REG_SP, -32);
	emit_sd(REG_A0, REG_SP, 0);
	emit_sd(REG_A1, REG_SP, 8);
	emit_sd(REG_A2, REG_SP, 16);
	emit_sd(REG_A7, REG_SP, 24);

	emit_li(REG_A0, line);
	emit_li(REG_A1, user_stop ? 1 : 0);
	emit_li(REG_A2, source_stop ? 1 : 0);
	emit_li(REG_A7, ECALL_BREAKPOINT);
	emit_ecall();

	emit_ld(REG_A0, REG_SP, 0);
	emit_ld(REG_A1, REG_SP, 8);
	emit_ld(REG_A2, REG_SP, 16);
	emit_ld(REG_A7, REG_SP, 24);
	emit_addi(REG_SP, REG_SP, 32);
	m_emitting_breakpoint = false;
}

// The frame is left where it is; only the depth moves. Nothing reads a frame
// above the depth, and the next call overwrites it.
void RISCVCodeGen::emit_debug_exit() {
	emit_la(REG_T0, DEBUG_LABEL);
	emit_ld(REG_T1, REG_T0, DebugLayout::DEPTH_OFF);
	emit_addi(REG_T1, REG_T1, -1);
	emit_sd(REG_T1, REG_T0, DebugLayout::DEPTH_OFF);
}

} // namespace gdscript
