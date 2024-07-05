#include "riscv.hpp"
#include "syscalls.h"

namespace riscv
{
	inline RiscvEmulator& emu(machine_t& m)
	{
		return *m.get_userdata<RiscvEmulator>();
	}

	#define APICALL(func) static void func(machine_t& machine [[maybe_unused]])

	APICALL(api_print)
	{
		auto [view] = machine.sysargs<std::string_view>();

		emu(machine).print(view);
	}

}

void RiscvEmulator::initialize_syscalls()
{
	using namespace riscv;

	// Initialize the Newlib system calls.
	machine().setup_minimal_syscalls();

	// Add the Godot system calls.
	machine_t::install_syscall_handlers({
		{ECALL_PRINT, api_print},
	});



}
