#include "register_allocator.h"
#include <algorithm>

namespace gdscript {

RegisterAllocator::RegisterAllocator() {
	init_free_registers();
	m_next_stack_offset = 16;
}

void RegisterAllocator::init_free_registers() {
	m_free_registers = {
		REG_T0, REG_T1, REG_T2,
		REG_S1, REG_S2, REG_S3, REG_S4, REG_S5, REG_S6, REG_S7,
		REG_S8, REG_S9, REG_S10, REG_S11,
		REG_T3, REG_T4, REG_T5, REG_T6
	};
}

void RegisterAllocator::init(const IRFunction& func) {
	m_vreg_to_preg.clear();
	m_preg_to_vreg.clear();
	m_spilled_vregs.clear();
	m_vreg_all_uses.clear();
	init_free_registers();
	m_next_stack_offset = 16;
	compute_next_use(func);
}

void RegisterAllocator::compute_next_use(const IRFunction& func) {
	m_vreg_all_uses.clear();
	
	for (size_t i = 0; i < func.instructions.size(); i++) {
		const IRInstruction& instr = func.instructions[i];
		int instr_idx = static_cast<int>(i);
		
		for (const IRValue& operand : instr.operands) {
			if (operand.type == IRValue::Type::REGISTER) {
				int vreg = std::get<int>(operand.value);
				m_vreg_all_uses[vreg].push_back(instr_idx);
			}
		}
	}
	
	for (std::pair<const int, std::vector<int>>& pair : m_vreg_all_uses) {
		std::sort(pair.second.begin(), pair.second.end());
		pair.second.erase(
			std::unique(pair.second.begin(), pair.second.end()),
			pair.second.end()
		);
	}
}

int RegisterAllocator::allocate_register(int vreg, int current_instr_idx) {
	std::unordered_map<int, uint8_t>::iterator it = m_vreg_to_preg.find(vreg);
	if (it != m_vreg_to_preg.end()) {
		return static_cast<int>(it->second);
	}
	
	std::unordered_map<int, int>::iterator spill_it = m_spilled_vregs.find(vreg);
	if (spill_it != m_spilled_vregs.end()) {
		if (m_free_registers.empty()) {
			int spill_candidate = find_spill_candidate(current_instr_idx);
			if (spill_candidate != -1) {
				spill_register(spill_candidate);
			}
		}
		
		if (m_free_registers.empty()) {
			return -1;
		}
		
		uint8_t preg = m_free_registers.back();
		m_free_registers.pop_back();
		m_vreg_to_preg[vreg] = preg;
		m_preg_to_vreg[preg] = vreg;
		m_spilled_vregs.erase(vreg);
		
		return static_cast<int>(preg);
	}
	
	if (m_free_registers.empty()) {
		int spill_candidate = find_spill_candidate(current_instr_idx);
		if (spill_candidate != -1) {
			spill_register(spill_candidate);
		}
	}
	
	if (m_free_registers.empty()) {
		int stack_offset = m_next_stack_offset;
		m_next_stack_offset += VARIANT_SIZE;
		m_spilled_vregs[vreg] = stack_offset;
		return -1;
	}
	
	uint8_t preg = m_free_registers.back();
	m_free_registers.pop_back();
	m_vreg_to_preg[vreg] = preg;
	m_preg_to_vreg[preg] = vreg;
	
	return static_cast<int>(preg);
}

int RegisterAllocator::get_physical_register(int vreg) const {
	std::unordered_map<int, uint8_t>::const_iterator it = m_vreg_to_preg.find(vreg);
	if (it != m_vreg_to_preg.end()) {
		return static_cast<int>(it->second);
	}
	return -1;
}

int RegisterAllocator::get_stack_offset(int vreg) const {
	std::unordered_map<int, int>::const_iterator it = m_spilled_vregs.find(vreg);
	if (it != m_spilled_vregs.end()) {
		return it->second;
	}
	return -1;
}

void RegisterAllocator::spill_register(int vreg) {
	std::unordered_map<int, uint8_t>::iterator it = m_vreg_to_preg.find(vreg);
	if (it == m_vreg_to_preg.end()) {
		return;
	}
	
	uint8_t preg = it->second;
	m_free_registers.push_back(preg);
	m_preg_to_vreg.erase(preg);
	m_vreg_to_preg.erase(vreg);
	
	int stack_offset = m_next_stack_offset;
	m_next_stack_offset += VARIANT_SIZE;
	m_spilled_vregs[vreg] = stack_offset;
}

std::vector<std::pair<uint8_t, uint8_t>> RegisterAllocator::handle_syscall_clobbering(
	const std::vector<uint8_t>& clobbered_regs,
	int current_instr_idx
) {
	std::vector<std::pair<uint8_t, uint8_t>> moves;
	
	for (uint8_t clobbered_preg : clobbered_regs) {
		std::unordered_map<uint8_t, int>::iterator it = m_preg_to_vreg.find(clobbered_preg);
		if (it != m_preg_to_vreg.end()) {
			int vreg = it->second;
			
			if (!m_free_registers.empty()) {
				uint8_t new_preg = m_free_registers.back();
				m_free_registers.pop_back();
				
				m_vreg_to_preg[vreg] = new_preg;
				m_preg_to_vreg.erase(clobbered_preg);
				m_preg_to_vreg[new_preg] = vreg;
				
				moves.push_back({clobbered_preg, new_preg});
			} else {
				spill_register(vreg);
			}
		}
	}
	
	return moves;
}

void RegisterAllocator::free_register(int vreg) {
	std::unordered_map<int, uint8_t>::iterator it = m_vreg_to_preg.find(vreg);
	if (it != m_vreg_to_preg.end()) {
		uint8_t preg = it->second;
		m_free_registers.push_back(preg);
		m_preg_to_vreg.erase(preg);
		m_vreg_to_preg.erase(vreg);
	}
}

bool RegisterAllocator::is_register_available(uint8_t preg) const {
	return std::find(m_free_registers.begin(), m_free_registers.end(), preg) != m_free_registers.end();
}

int RegisterAllocator::get_next_use(int vreg, int current_instr_idx) const {
	std::unordered_map<int, std::vector<int>>::const_iterator uses_it = m_vreg_all_uses.find(vreg);
	if (uses_it == m_vreg_all_uses.end()) {
		return -1;
	}
	
	const std::vector<int>& uses = uses_it->second;
	std::vector<int>::const_iterator it = std::lower_bound(uses.begin(), uses.end(), current_instr_idx + 1);
	
	if (it != uses.end()) {
		return *it;
	}
	
	return -1;
}

int RegisterAllocator::find_spill_candidate(int current_instr_idx) const {
	if (m_vreg_to_preg.empty()) {
		return -1;
	}
	
	int best_vreg = -1;
	int best_next_use = -1;
	
	for (const std::pair<const int, uint8_t>& pair : m_vreg_to_preg) {
		int vreg = pair.first;
		int next_use = get_next_use(vreg, current_instr_idx);
		
		if (next_use == -1) {
			return vreg;
		}
		
		if (next_use > best_next_use) {
			best_next_use = next_use;
			best_vreg = vreg;
		}
	}
	
	return best_vreg;
}

} // namespace gdscript

