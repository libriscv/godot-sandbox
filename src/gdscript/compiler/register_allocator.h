#pragma once
#include "ir.h"
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace gdscript {

/**
 * @brief Simple Greedy Register Allocator with Furthest Next Use heuristic
 * 
 * Maintains a pool of 18 free physical registers (t0-t6, s1-s11, excluding s0/fp)
 * and maps virtual registers to physical RISC-V registers using hash maps.
 * 
 * Algorithm:
 * - When allocating a register for a virtual register, checks if already allocated
 *   (return it), spilled to stack (load it), or new (take from free pool).
 * - If the free pool is empty, uses the "Furthest Next Use" heuristic to select
 *   a spill candidate: picks the virtual register whose next use is furthest away
 *   (or unknown/never used again).
 * - This keeps values needed soon in registers and spills values not needed soon,
 *   improving code quality with minimal complexity.
 * 
 * The allocator tracks all uses of each virtual register in sorted lists, allowing
 * efficient binary search to find the next use after any given instruction.
 * 
 * @note Spills to stack only when necessary (no free registers available)
 */
class RegisterAllocator {
public:
	/**
	 * @brief Construct a new Register Allocator
	 * 
	 * Initializes the free register pool with 18 available physical registers.
	 */
	RegisterAllocator();
	
	/**
	 * @brief Initialize for a new function
	 * 
	 * Clears all state and computes next use positions for all virtual registers
	 * in the function. Must be called before allocating registers for a function.
	 * 
	 * @param func The IR function to analyze for register allocation
	 */
	void init(const IRFunction& func);
	
	/**
	 * @brief Get physical register for virtual register
	 * 
	 * Implements the Simple Greedy algorithm:
	 * 1. If already allocated to a physical register, return it
	 * 2. If spilled to stack, load it into a register (spill another if needed)
	 * 3. If new, allocate from free pool (spill another if needed)
	 * 
	 * Uses Furthest Next Use heuristic when spilling: picks the vreg with
	 * furthest next use (or unknown/never used again) as the spill candidate.
	 * 
	 * @param vreg Virtual register number to allocate
	 * @param current_instr_idx Current instruction index (used for next use computation)
	 * @return Physical register number (0-31), or -1 if spilled to stack
	 */
	int allocate_register(int vreg, int current_instr_idx);
	
	/**
	 * @brief Get current physical register for vreg
	 * 
	 * @param vreg Virtual register number
	 * @return Physical register number (0-31), or -1 if spilled or not allocated
	 */
	int get_physical_register(int vreg) const;
	
	/**
	 * @brief Get stack offset for spilled vreg
	 * 
	 * @param vreg Virtual register number
	 * @return Stack offset in bytes, or -1 if in register or not allocated
	 */
	int get_stack_offset(int vreg) const;
	
	/**
	 * @brief Spill a virtual register to stack, freeing its physical register
	 * 
	 * Called by allocate_register() when the free pool is empty.
	 * The vreg to spill is selected by find_spill_candidate() using
	 * the Furthest Next Use heuristic.
	 * 
	 * @param vreg Virtual register to spill
	 */
	void spill_register(int vreg);
	
	/**
	 * @brief Handle syscall register clobbering
	 * 
	 * Moves live values from clobbered registers (e.g., a0-a3 for VEVAL,
	 * a0-a5 for VCALL) to other available registers. Only spills to stack
	 * if no other registers are available.
	 * 
	 * @param clobbered_regs List of physical registers that will be clobbered
	 * @param current_instr_idx Current instruction index (unused, for future use)
	 * @return List of register moves needed: (src_preg, dst_preg) pairs
	 *         The caller should emit move instructions for these pairs
	 */
	std::vector<std::pair<uint8_t, uint8_t>> handle_syscall_clobbering(
		const std::vector<uint8_t>& clobbered_regs, 
		int current_instr_idx
	);
	
	/**
	 * @brief Free a register when vreg dies
	 * 
	 * Optional optimization: can be called when a virtual register is no longer
	 * needed to free its physical register earlier. Currently not used but
	 * available for future optimizations.
	 * 
	 * @param vreg Virtual register to free
	 */
	void free_register(int vreg);
	
	/**
	 * @brief Check if a physical register is available
	 * 
	 * @param preg Physical register number (0-31)
	 * @return true if the register is in the free pool, false otherwise
	 */
	bool is_register_available(uint8_t preg) const;
	
	/**
	 * @brief Get all available physical registers
	 * 
	 * @return Reference to the vector of free physical register numbers
	 */
	const std::vector<uint8_t>& get_available_registers() const { return m_free_registers; }
	
	/**
	 * @brief Get next use position for a vreg from current instruction
	 * 
	 * Part of the Furthest Next Use heuristic. Uses binary search on the
	 * sorted use list to efficiently find the next instruction where the
	 * virtual register is used.
	 * 
	 * @param vreg Virtual register number
	 * @param current_instr_idx Current instruction index
	 * @return Next instruction index where vreg is used, or -1 if no next use
	 *         (vreg is never used again - best candidate for spilling)
	 */
	int get_next_use(int vreg, int current_instr_idx) const;
	
	/**
	 * @brief Compute next use positions for all vregs in function
	 * 
	 * Scans all instructions to collect all uses of each virtual register,
	 * then sorts the use lists for efficient binary search. This builds
	 * m_vreg_all_uses which is used by get_next_use() to find the next
	 * use after any given instruction.
	 * 
	 * @param func The IR function to analyze
	 */
	void compute_next_use(const IRFunction& func);

private:
	/**
	 * @brief Available physical registers (t0-t6, s1-s11, excluding s0/fp)
	 * 
	 * Total: 18 registers available for allocation
	 * Note: REG_S0 (x8) is excluded as it's used as frame pointer
	 */
	static constexpr uint8_t REG_T0 = 5;
	static constexpr uint8_t REG_T1 = 6;
	static constexpr uint8_t REG_T2 = 7;
	static constexpr uint8_t REG_S0 = 8;  // x8 (fp) - excluded, used as frame pointer
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
	
	/**
	 * @brief Maps virtual register → physical register
	 * 
	 * Tracks which physical register each virtual register is currently in.
	 */
	std::unordered_map<int, uint8_t> m_vreg_to_preg;
	
	/**
	 * @brief Reverse map: physical register → virtual register
	 * 
	 * Allows quick lookup of which virtual register is in a given physical register.
	 */
	std::unordered_map<uint8_t, int> m_preg_to_vreg;
	
	/**
	 * @brief Available physical registers (free pool)
	 * 
	 * Vector of physical register numbers that are currently free and available
	 * for allocation. Initially contains 18 registers (t0-t6, s1-s11).
	 */
	std::vector<uint8_t> m_free_registers;
	
	/**
	 * @brief All uses of each virtual register (for computing next use dynamically)
	 * 
	 * Maps vreg → sorted list of instruction indices where it's used.
	 * Used by get_next_use() to efficiently find the next use after a given
	 * instruction using binary search.
	 */
	std::unordered_map<int, std::vector<int>> m_vreg_all_uses;
	
	/**
	 * @brief Spilled virtual registers: vreg → stack offset
	 * 
	 * Tracks virtual registers that have been spilled to the stack.
	 * Maps virtual register number to its stack offset in bytes.
	 */
	std::unordered_map<int, int> m_spilled_vregs;
	
	/**
	 * @brief Stack offset counter (for spilled registers)
	 * 
	 * Tracks the next available stack offset for spilling registers.
	 * Starts at 16 (after saved registers: ra, fp).
	 */
	int m_next_stack_offset;
	
	/**
	 * @brief Size of Variant struct in bytes
	 * 
	 * Used to calculate stack offsets when spilling registers.
	 */
	static constexpr int VARIANT_SIZE = 24;
	
	/**
	 * @brief Find spill candidate register using Furthest Next Use heuristic
	 * 
	 * Selects the virtual register with the furthest next use (or unknown/never
	 * used again) from the current instruction. This is the core of the
	 * "Furthest Next Use" improvement over simple "first allocated = first spilled".
	 * 
	 * @param current_instr_idx Current instruction index
	 * @return Virtual register number to spill, or -1 if no registers allocated
	 */
	int find_spill_candidate(int current_instr_idx) const;
	
	/**
	 * @brief Initialize free register pool
	 * 
	 * Populates m_free_registers with the 18 available physical registers
	 * (t0-t6, s1-s11, excluding s0/fp).
	 */
	void init_free_registers();
};

} // namespace gdscript

