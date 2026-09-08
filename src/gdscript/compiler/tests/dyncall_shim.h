#pragma once
#include "syscall_abi.h"
#include <libriscv/machine.hpp>
#include <libriscv/rv32i_instr.hpp>

// Bare-machine compiler tests install their own syscall handlers. Give them
// the same counted-instruction dispatch as the Sandbox before loading ELFs.
inline void sgd_install_test_dyncalls() {
	using namespace riscv;
	static const Instruction<RISCV64> instruction{
		[](CPU<RISCV64>& cpu, rv32i_instruction word) {
			if (!gdscript::valid_counted_syscall(word.whole, int64_t(cpu.reg(REG_ARG0))))
				cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION, word.whole);
			Machine<RISCV64>::syscall_handlers[word.whole >> 20](cpu.machine());
		},
		[](char* buffer, size_t size, const CPU<RISCV64>&, rv32i_instruction word) {
			return snprintf(buffer, size, "DYNCALL %u", word.whole >> 20);
		}
	};
	CPU<RISCV64>::on_unimplemented_instruction = [](rv32i_instruction word) -> const Instruction<RISCV64>& {
		if (gdscript::valid_counted_syscall_encoding(word.whole) &&
			(word.whole >> 20) < Machine<RISCV64>::syscall_handlers.size()) return instruction;
		return CPU<RISCV64>::get_unimplemented_instruction();
	};
}
