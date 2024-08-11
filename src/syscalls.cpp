#include "syscalls.h"
#include "sandbox.h"

#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

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

APICALL(api_node2d) {
	// Node2D operation, Node2D object name, and the variant to get/set the value.
	auto [op, name, var] = machine.sysargs<int, std::string_view, GuestVariant*>();
	// We can't pass a string_view directly to Godot, but we can pass a C-string.
	// We need to make sure the string is null-terminated:
	if (name.back() != 0)
		throw std::runtime_error("node2d: Name string must end with an inclusive null-terminator");

	auto &emu = riscv::emu(machine);
	// Get the Node2D object by its name from the current scene.
	godot::Node2D *node2d = emu.get_node<Node2D>(name.begin());
	if (!node2d) {
		ERR_PRINT(("Node not found: " + std::string(name)).c_str());
		throw std::runtime_error("Node not found: " + std::string(name));
	}

	switch (Node2D_Op(op)) {
		case Node2D_Op::GET_POSITION:
			var->set(emu, node2d->get_position());
			break;
		case Node2D_Op::SET_POSITION:
			node2d->set_position(var->toVariant(emu));
			break;
		case Node2D_Op::GET_ROTATION:
			var->set(emu, node2d->get_rotation());
			break;
		case Node2D_Op::SET_ROTATION:
			node2d->set_rotation(var->toVariant(emu));
			break;
		case Node2D_Op::GET_SCALE:
			var->set(emu, node2d->get_scale());
			break;
		case Node2D_Op::SET_SCALE:
			node2d->set_scale(var->toVariant(emu));
			break;
		case Node2D_Op::GET_SKEW:
			var->set(emu, node2d->get_skew());
			break;
		case Node2D_Op::SET_SKEW:
			node2d->set_skew(var->toVariant(emu));
			break;
		default:
			ERR_PRINT("Invalid Node2D operation");
			throw std::runtime_error("Invalid Node2D operation");
	}
}

} //namespace riscv

void Sandbox::initialize_syscalls() {
	using namespace riscv;

	// Initialize common Linux system calls
	machine().setup_linux_syscalls(false, false);
	// Initialize POSIX threads
	machine().setup_posix_threads();

	machine().on_unhandled_syscall = [](machine_t &machine, size_t syscall) {
		auto &emu = riscv::emu(machine);
		emu.print("Unhandled system call: " + std::to_string(syscall));
		machine.penalize(100'000); // Add to the instruction counter due to I/O.
	};

	// Add the Godot system calls.
	machine_t::install_syscall_handlers({
			{ ECALL_PRINT, api_print },
			{ ECALL_VCALL, api_vcall },
			{ ECALL_VEVAL, api_veval },
			{ ECALL_NODE2D, api_node2d },
	});
}
