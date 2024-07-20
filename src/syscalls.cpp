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

	APICALL(api_vcall)
	{
		auto [vp, method, mlen, args, vret] = machine.sysargs<GuestVariant*, std::string, unsigned, std::span<GuestVariant>, GuestVariant*>();
		(void)mlen;

		auto& emu = riscv::emu(machine);

		emu.print("Calling method: " + std::string(method));

		std::array<Variant, 64> vargs;
		std::array<const Variant*, 64> argptrs;
		if (args.size() > vargs.size())
		{
			emu.print("Too many arguments.");
			return;
		}
		printf("args.size() = %zu\n", args.size());

		for (size_t i = 0; i < args.size(); i++)
		{
			printf("args[%zu] = %p\n", i, &args[i]);
			vargs[i] = args[i].toVariant(machine);
			argptrs[i] = &vargs[i];
		}

		auto vcall = vp->toVariant(machine);
		Variant ret;
		GDExtensionCallError error;
		vcall.callp(method.c_str(), argptrs.data(), args.size(), ret, error);

		vret->set(machine, ret);
	}

	APICALL(api_veval)
	{
		auto [op, ap, bp, retp] = machine.sysargs<int, GuestVariant*, GuestVariant*, GuestVariant*>();

		auto& emu = riscv::emu(machine);
		auto ret = retp->toVariant(machine);

		emu.print("Evaluating operator: " + std::to_string(op));

		auto valid = false;
		Variant::evaluate(static_cast<Variant::Operator>(op), ap->toVariant(machine), bp->toVariant(machine), ret, valid);

		machine.set_result(valid);
		retp->set(machine, ret);
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
		{ECALL_VCALL, api_vcall},
		{ECALL_VEVAL, api_veval},
	});



}
