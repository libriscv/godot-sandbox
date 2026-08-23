#pragma once
#include "ir.h"
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace gdscript {

// Greedy register allocator with furthest-next-use spill heuristic.
// Pool: t0-t5, s1-s11 (17 regs). t6 reserved for wide-offset scratch.
class RegisterAllocator {
public:
	RegisterAllocator();

	// Reset state and precompute use positions for func.
	void init(const IRFunction& func);

	// Map vreg to a physical register, spilling by furthest-next-use if needed.
	// Returns physical register (0-31), or -1 if no register available.
	int allocate_register(int vreg, int current_instr_idx);

	// Current physical register for vreg, or -1.
	int get_physical_register(int vreg) const;

	// Stub; stack offsets are managed by codegen, not the allocator.
	int get_stack_offset(int vreg) const;

	// Free the physical register held by vreg (does not emit a store).
	void spill_register(int vreg);

	// Move live values out of clobbered_regs into free registers.
	// Returns (src_preg, dst_preg) pairs the caller must emit moves for.
	std::vector<std::pair<uint8_t, uint8_t>> handle_syscall_clobbering(
		const std::vector<uint8_t>& clobbered_regs,
		int current_instr_idx
	);

	// Release the physical register held by vreg.
	void free_register(int vreg);

	// Drop the vreg→preg mapping. Needed after a stack-side Variant update
	// that makes the physical register's value stale.
	void invalidate_register(int vreg);

	// Bypass allocation: bind vreg to preg unconditionally.
	void force_register_mapping(int vreg, uint8_t preg);

	// Reverse lookup: vreg occupying preg, or -1.
	int get_vreg_for_preg(uint8_t preg) const;

	// All vregs in physical registers. AWAIT requires full spill.
	std::vector<int> mapped_vregs() const;

	// Whether preg is in the free pool.
	bool is_register_available(uint8_t preg) const;

	const std::vector<uint8_t>& get_available_registers() const { return m_free_registers; }

	// First use of vreg strictly after current_instr_idx, or -1 (dead).
	int get_next_use(int vreg, int current_instr_idx) const;

	// Build sorted use lists for all vregs in func.
	void compute_next_use(const IRFunction& func);

private:
	static constexpr uint8_t REG_T0 = 5;
	static constexpr uint8_t REG_T1 = 6;
	static constexpr uint8_t REG_T2 = 7;
	static constexpr uint8_t REG_S0 = 8;  // frame pointer, excluded from pool
	static constexpr uint8_t REG_S1 = 9;
	static constexpr uint8_t REG_S2 = 18;
	static constexpr uint8_t REG_S3 = 19;
	static constexpr uint8_t REG_S4 = 20;
	static constexpr uint8_t REG_S5 = 21;
	static constexpr uint8_t REG_S6 = 22;
	static constexpr uint8_t REG_S7 = 23;
	static constexpr uint8_t REG_S8 = 24;
	static constexpr uint8_t REG_S9 = 25;
	static constexpr uint8_t REG_S10 = 26;
	static constexpr uint8_t REG_S11 = 27;
	static constexpr uint8_t REG_T3 = 28;
	static constexpr uint8_t REG_T4 = 29;
	static constexpr uint8_t REG_T5 = 30;
	static constexpr uint8_t REG_T6 = 31;

	std::unordered_map<int, uint8_t> m_vreg_to_preg;
	std::unordered_map<uint8_t, int> m_preg_to_vreg;
	std::vector<uint8_t> m_free_registers;
	// vreg → sorted instruction indices where it appears (for binary-search next-use).
	std::unordered_map<int, std::vector<int>> m_vreg_all_uses;

	// Pick the vreg with the furthest (or no) next use. Returns -1 if nothing allocated.
	int find_spill_candidate(int current_instr_idx) const;

	void init_free_registers();
};

} // namespace gdscript
