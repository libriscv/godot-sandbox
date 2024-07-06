#include "riscv.hpp"
#include "syscalls.h"

#include <godot_cpp/variant/utility_functions.hpp>

namespace riscv
{
	inline RiscvEmulator& emu(machine_t& m)
	{
		return *m.get_userdata<RiscvEmulator>();
	}

	#define APICALL(func) static void func(machine_t& machine [[maybe_unused]])

	APICALL(api_print)
	{
		auto [view] = machine.sysargs<std::span<GuestVariant>>();

		// We really want print_internal to be a public function.
		for (const auto& var : view)
		{
			UtilityFunctions::print(var.toVariant(machine));
		}
	}

}

void RiscvEmulator::initialize_syscalls()
{
	using namespace riscv;

	// Initialize the Linux system calls.
	machine().setup_linux_syscalls(false, false);
	// TODO:
	//machine().setup_posix_threads();

	// Add the Godot system calls.
	machine_t::install_syscall_handlers({
		{ECALL_PRINT, api_print},
	});



}
