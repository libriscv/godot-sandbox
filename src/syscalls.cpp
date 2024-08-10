#include "syscalls.h"
#include "sandbox.h"

#include <godot_cpp/variant/utility_functions.hpp>

namespace riscv {
inline Sandbox &emu(machine_t &m) {
	return *m.get_userdata<Sandbox>();
}

#define APICALL(func) static void func(machine_t &machine [[maybe_unused]])

APICALL(api_print) {
	auto [array, len] = machine.sysargs<gaddr_t, gaddr_t>();
	auto &emu = riscv::emu(machine);

	const GuestVariant *array_ptr = emu.machine().memory.memarray<GuestVariant>(array, len);

	// We really want print_internal to be a public function.
	for (gaddr_t i = 0; i < len; i++) {
		auto &var = array_ptr[i];
		UtilityFunctions::print(var.toVariant(emu));
	}
}

APICALL(api_vcall) {
	auto [vp, method, mlen, args_ptr, args_size, vret] = machine.sysargs<GuestVariant *, std::string, unsigned, gaddr_t, gaddr_t, GuestVariant *>();
	(void)mlen;

	auto &emu = riscv::emu(machine);

	std::array<Variant, 64> vargs;
	std::array<const Variant *, 64> argptrs;
	if (args_size > vargs.size()) {
		emu.print("Too many arguments.");
		return;
	}

	const GuestVariant *args = emu.machine().memory.memarray<GuestVariant>(args_ptr, args_size);

	for (size_t i = 0; i < args_size; i++) {
		vargs[i] = args[i].toVariant(emu);
		argptrs[i] = &vargs[i];
	}

	GDExtensionCallError error;

	auto *vcall = vp->toVariantPtr(emu);
	Variant ret;
	vcall->callp("call", argptrs.data(), args_size, ret, error);
	vret->set(emu, ret);
}

APICALL(api_veval) {
	auto [op, ap, bp, retp] = machine.sysargs<int, GuestVariant *, GuestVariant *, GuestVariant *>();

	auto &emu = riscv::emu(machine);
	auto ret = retp->toVariant(emu);

	emu.print("Evaluating operator: " + std::to_string(op));

	auto valid = false;
	Variant::evaluate(static_cast<Variant::Operator>(op), ap->toVariant(emu), bp->toVariant(emu), ret, valid);

	machine.set_result(valid);
	retp->set(emu, ret);
}

} //namespace riscv

void Sandbox::initialize_syscalls() {
	using namespace riscv;

	// Initialize the Linux system calls.
	machine().setup_linux_syscalls(false, false);
	// TODO:
	//machine().setup_posix_threads();

	// Add the Godot system calls.
	machine_t::install_syscall_handlers({
			{ ECALL_PRINT, api_print },
			{ ECALL_VCALL, api_vcall },
			{ ECALL_VEVAL, api_veval },
	});
}
