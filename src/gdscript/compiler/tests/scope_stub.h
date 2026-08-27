#pragma once
#include "../syscall_numbers.h"

template <typename Machine>
inline void install_scope_stub() {
	Machine::install_syscall_handler(ECALL_VSCOPE, [](Machine &machine) {
		machine.set_result(0);
	});
	Machine::install_syscall_handler(ECALL_SANDBOX_ADD, [](Machine &machine) {
		machine.set_result(0);
	});
}
