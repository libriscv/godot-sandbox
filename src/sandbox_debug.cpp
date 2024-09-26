#include "sandbox.h"

Array Sandbox::get_general_registers() const {
	Array ret;
	for (int i = 0; i < 32; i++) {
		ret.push_back(m_machine->cpu.reg(i));
	}
	return ret;
}

Array Sandbox::get_floating_point_registers() const {
	Array ret;
	for (int i = 0; i < 32; i++) {
		auto& freg = m_machine->cpu.registers().getfl(i);
		// We suspect that it's a 32-bit float if the upper 32 bits are zero
		if (freg.i32[1] == 0) {
			ret.push_back(freg.f32[0]);
		} else {
			ret.push_back(freg.f64);
		}
	}
	return ret;
}

void Sandbox::set_argument_registers(Array args) {
	if (args.size() > 8) {
		ERR_PRINT("set_argument_registers() can only set up to 8 arguments.");
		return;
	}
	for (int i = 0; i < args.size(); i++) {
		m_machine->cpu.reg(i + 10) = args[i].operator int64_t();
	}
}

String Sandbox::get_current_instruction() const {
	std::string instr = m_machine->cpu.current_instruction_to_string();
	return String(instr.c_str());
}

void Sandbox::resume(uint64_t max_instructions) {
	CurrentState &state = this->m_states[m_level];
	const bool is_reentrant_call = m_level > 1;
	state.reset(this->m_level);
	m_level++;

	// Scoped objects and owning tree node
	CurrentState *old_state = this->m_current_state;
	this->m_current_state = &state;
	// Call statistics
	this->m_calls_made++;
	Sandbox::m_global_calls_made++;

	const gaddr_t address = m_machine->cpu.pc();
	try {
		// execute guest function
		if (!is_reentrant_call) {
			// execute!
			m_machine->resume(max_instructions);
		} else if (m_level < MAX_LEVEL) {
			riscv::Registers<RISCV_ARCH> regs;
			regs = m_machine->cpu.registers();
			// execute!
			m_machine->cpu.preempt_internal(regs, true, address, max_instructions);
		} else {
			throw std::runtime_error("Recursion level exceeded");
		}

	} catch (const std::exception &e) {
		this->handle_exception(address);
	}

	// Restore the previous state
	this->m_level--;
	this->m_current_state = old_state;
}
