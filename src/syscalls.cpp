#include "syscalls.h"
#include "sandbox.h"

#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
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

	if (args_size > 8) {
		emu.print("Too many arguments.");
		return;
	}

	const GuestVariant *args = emu.machine().memory.memarray<GuestVariant>(args_ptr, args_size);

	GDExtensionCallError error;

	if (vp->type == Variant::CALLABLE) {
		std::array<Variant, 8> vargs;
		std::array<const Variant *, 8> argptrs;
		for (size_t i = 0; i < args_size; i++) {
			vargs[i] = args[i].toVariant(emu);
			argptrs[i] = &vargs[i];
		}

		auto *vcall = vp->toVariantPtr(emu);
		Variant ret;
		vcall->callp(String::utf8(method.data(), method.size()), argptrs.data(), args_size, ret, error);
		vret->set(emu, ret);
	} else if (vp->type == Variant::OBJECT) {
		auto *obj = reinterpret_cast<godot::Object *>(uintptr_t(vp->v.i));
		if (!emu.is_scoped_object(obj)) {
			ERR_PRINT("Object is not scoped");
			throw std::runtime_error("Object is not scoped");
		}

		Array vargs;
		vargs.resize(args_size);
		for (unsigned i = 0; i < args_size; i++) {
			vargs[i] = args[i].toVariant(emu);
		}
		Variant ret = obj->callv(String::utf8(method.data(), method.size()), vargs);
		vret->set(emu, ret);
	} else {
		ERR_PRINT("Invalid Variant type for Object::call");
	}
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

APICALL(api_vfree) {
	auto [vp] = machine.sysargs<GuestVariant *>();
	auto &emu = riscv::emu(machine);

	// Free the Variant object depending on its type.
	vp->free(emu);
}

APICALL(api_obj_callp) {
	auto [addr, method, deferred, vret, args_addr, args_size] = machine.sysargs<uint64_t, std::string_view, bool, GuestVariant *, gaddr_t, unsigned>();

	auto &emu = riscv::emu(machine);
	auto *obj = reinterpret_cast<godot::Object *>(uintptr_t(addr));
	if (!emu.is_scoped_object(obj)) {
		ERR_PRINT("Object is not scoped");
		throw std::runtime_error("Object is not scoped");
	}
	if (args_size > 8) {
		ERR_PRINT("Too many arguments.");
		throw std::runtime_error("Too many arguments.");
	}
	const GuestVariant *g_args = emu.machine().memory.memarray<GuestVariant>(args_addr, args_size);

	if (!deferred) {
		Array vargs;
		vargs.resize(args_size);
		for (unsigned i = 0; i < args_size; i++) {
			vargs[i] = g_args[i].toVariant(emu);
		}
		Variant ret = obj->callv(String::utf8(method.data(), method.size()), vargs);
		vret->set(emu, ret);
	} else {
		// Call deferred unfortunately takes a parameter pack, so we have to manually
		// check the number of arguments, and call the correct function.
		if (args_size == 0) {
			obj->call_deferred(String::utf8(method.data(), method.size()));
		} else if (args_size == 1) {
			obj->call_deferred(String::utf8(method.data(), method.size()), g_args[0].toVariant(emu));
		} else if (args_size == 2) {
			obj->call_deferred(String::utf8(method.data(), method.size()), g_args[0].toVariant(emu), g_args[1].toVariant(emu));
		} else if (args_size == 3) {
			obj->call_deferred(String::utf8(method.data(), method.size()), g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu));
		} else if (args_size == 4) {
			obj->call_deferred(String::utf8(method.data(), method.size()), g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu), g_args[3].toVariant(emu));
		} else if (args_size == 5) {
			obj->call_deferred(String::utf8(method.data(), method.size()), g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu), g_args[3].toVariant(emu), g_args[4].toVariant(emu));
		} else if (args_size == 6) {
			obj->call_deferred(String::utf8(method.data(), method.size()), g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu), g_args[3].toVariant(emu), g_args[4].toVariant(emu), g_args[5].toVariant(emu));
		} else if (args_size == 7) {
			obj->call_deferred(String::utf8(method.data(), method.size()), g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu), g_args[3].toVariant(emu), g_args[4].toVariant(emu), g_args[5].toVariant(emu), g_args[6].toVariant(emu));
		} else if (args_size == 8) {
			obj->call_deferred(String::utf8(method.data(), method.size()), g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu), g_args[3].toVariant(emu), g_args[4].toVariant(emu), g_args[5].toVariant(emu), g_args[6].toVariant(emu), g_args[7].toVariant(emu));
		}
	}
}

APICALL(api_get_node) {
	auto [addr, name] = machine.sysargs<uint64_t, std::string>();
	auto &emu = riscv::emu(machine);
	Node *node = nullptr;

	if (addr == 0) {
		auto *owner_node = emu.get_tree_base();
		if (owner_node == nullptr) {
			ERR_PRINT("Sandbox has no parent Node");
			machine.set_result(0);
			return;
		}
		node = owner_node->get_node<Node>(NodePath(name.c_str()));
	} else {
		Node *base_node = reinterpret_cast<Node *>(uintptr_t(addr));
		if (!emu.is_scoped_object(base_node)) {
			ERR_PRINT("Node object is not scoped");
			machine.set_result(0);
			return;
		}
		node = base_node->get_node<Node>(NodePath(name.c_str()));
	}
	if (node == nullptr) {
		ERR_PRINT(("Node not found: " + name).c_str());
		machine.set_result(0);
		return;
	}

	emu.add_scoped_object(node);
	machine.set_result(reinterpret_cast<uint64_t>(node));
}

APICALL(api_node) {
	auto [op, addr, gvar] = machine.sysargs<int, uint64_t, gaddr_t>();
	machine.penalize(250'000); // Costly Node operations.

	auto &emu = riscv::emu(machine);
	// Get the Node object by its name from the current scene.
	godot::Node *node = (godot::Node *)uintptr_t(addr);
	if (!emu.is_scoped_object(node)) {
		ERR_PRINT("Node object is not scoped");
		throw std::runtime_error("Node object is not scoped");
	}

	switch (Node_Op(op)) {
		case Node_Op::QUEUE_FREE:
			if (node == &emu) {
				ERR_PRINT("Cannot queue free the sandbox");
				throw std::runtime_error("Cannot queue free the sandbox");
			}
			//emu.rem_scoped_object(node);
			node->queue_free();
			break;
		case Node_Op::DUPLICATE: {
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			auto *new_node = node->duplicate();
			emu.add_scoped_object(new_node);
			var->set(emu, new_node);
		} break;
		case Node_Op::ADD_CHILD_DEFERRED:
		case Node_Op::ADD_CHILD: {
			auto *child = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			auto *child_node = (godot::Node *)uintptr_t(child->v.i);
			if (child_node == nullptr) {
				ERR_PRINT("Child Node object is not a Node");
				throw std::runtime_error("Child Node object is not a Node");
			}
			if (!emu.is_scoped_object(child_node)) {
				ERR_PRINT("Child Node object is not scoped");
				throw std::runtime_error("Child Node object is not scoped");
			}
			if (Node_Op(op) == Node_Op::ADD_CHILD_DEFERRED)
				node->call_deferred("add_child", child_node);
			else
				node->add_child(child_node);
		} break;
		case Node_Op::GET_NAME: {
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			var->set(emu, String(node->get_name()));
		} break;
		case Node_Op::GET_PATH: {
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			var->set(emu, String(node->get_path()));
		} break;
		case Node_Op::GET_PARENT: {
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			if (node->get_parent() == nullptr) {
				var->set(emu, Variant());
			} else {
				var->set(emu, node->get_parent());
			}
		} break;
		case Node_Op::GET_CHILDREN: {
			// Get a GuestStdVector from guest to store the children.
			auto *vec = emu.machine().memory.memarray<GuestStdVector>(gvar, 1);
			// Get the children of the node.
			auto children = node->get_children();
			// Allocate memory for the children in the guest vector.
			auto *cptr = vec->alloc<uint64_t>(emu.machine(), children.size());
			// Copy the children to the guest vector, and add them to the scoped objects.
			for (int i = 0; i < children.size(); i++) {
				auto *child = godot::Object::cast_to<godot::Node>(children[i]);
				if (child) {
					emu.add_scoped_object(child);
					cptr[i] = uint64_t(uintptr_t(child));
				} else {
					cptr[i] = 0;
				}
			}
			// No return value is needed.
		} break;
		case Node_Op::GET_METHOD_LIST: {
			// Get a GuestStdVector from guest to store the method list.
			auto *vec = emu.machine().memory.memarray<GuestStdVector>(gvar, 1);
			// Get the method list of the node.
			auto methods = node->get_method_list();
			// Allocate GuestStdStrings for the methods in the guest vector.
			auto *sptr = vec->alloc<GuestStdString>(emu.machine(), methods.size());
			// Copy the methods to the guest vector.
			for (size_t i = 0; i < methods.size(); i++) {
				Dictionary dict = methods[i].operator godot::Dictionary();
				auto name = String(dict["name"]).utf8();
				sptr[i].set_string(emu.machine(), sptr[i].ptr, name.ptr(), name.length());
			}
		} break;
		case Node_Op::GET: {
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 2);
			auto name = var[0].toVariant(emu).operator String();
			var[1].set(emu, node->get(name));
		} break;
		case Node_Op::SET: {
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 2);
			auto name = var->toVariant(emu).operator String();
			node->set(name, var[1].toVariant(emu));
		} break;
		default:
			throw std::runtime_error("Invalid Node operation");
	}
}

APICALL(api_node2d) {
	// Node2D operation, Node2D address, and the variant to get/set the value.
	auto [op, addr, gvar] = machine.sysargs<int, uint64_t, gaddr_t>();
	machine.penalize(100'000); // Costly Node2D operations.

	auto &emu = riscv::emu(machine);
	// Get the Node2D object by its name from the current scene.
	godot::Node *node = (godot::Node *)uintptr_t(addr);
	if (!emu.is_scoped_object(node)) {
		ERR_PRINT("Node2D object is not scoped");
		throw std::runtime_error("Node2D object is not scoped");
	}

	// Cast the Node2D object to a Node2D object.
	auto *node2d = godot::Object::cast_to<godot::Node2D>(node);
	if (node2d == nullptr) {
		ERR_PRINT("Node2D object is not a Node2D");
		throw std::runtime_error("Node2D object is not a Node2D");
	}

	// View the variant from the guest memory.
	auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
	switch (Node2D_Op(op)) {
		case Node2D_Op::GET_POSITION:
			var->set(emu, node2d->get_position());
			break;
		case Node2D_Op::SET_POSITION:
			node2d->set_deferred("position", var->toVariant(emu));
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

APICALL(api_node3d) {
	// Node3D operation, Node3D address, and the variant to get/set the value.
	auto [op, addr, gvar] = machine.sysargs<int, uint64_t, gaddr_t>();
	machine.penalize(100'000); // Costly Node3D operations.

	auto &emu = riscv::emu(machine);
	// Get the Node3D object by its name from the current scene.
	godot::Node *node = (godot::Node *)uintptr_t(addr);
	if (!emu.is_scoped_object(node)) {
		ERR_PRINT("Node3D object is not scoped");
		throw std::runtime_error("Node3D object is not scoped");
	}

	// Cast the Node3D object to a Node3D object.
	auto *node3d = godot::Object::cast_to<godot::Node3D>(node);
	if (node3d == nullptr) {
		ERR_PRINT("Node3D object is not a Node3D");
		throw std::runtime_error("Node3D object is not a Node3D");
	}

	// View the variant from the guest memory.
	auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
	switch (Node3D_Op(op)) {
		case Node3D_Op::GET_POSITION:
			var->set(emu, node3d->get_position());
			break;
		case Node3D_Op::SET_POSITION:
			node3d->set_position(var->toVariant(emu));
			break;
		case Node3D_Op::GET_ROTATION:
			var->set(emu, node3d->get_rotation());
			break;
		case Node3D_Op::SET_ROTATION:
			node3d->set_rotation(var->toVariant(emu));
			break;
		case Node3D_Op::GET_SCALE:
			var->set(emu, node3d->get_scale());
			break;
		case Node3D_Op::SET_SCALE:
			node3d->set_scale(var->toVariant(emu));
			break;
		default:
			ERR_PRINT("Invalid Node3D operation");
			throw std::runtime_error("Invalid Node3D operation");
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
			{ ECALL_VFREE, api_vfree },
			{ ECALL_OBJ_CALLP, api_obj_callp },
			{ ECALL_GET_NODE, api_get_node },
			{ ECALL_NODE, api_node },
			{ ECALL_NODE2D, api_node2d },
			{ ECALL_NODE3D, api_node3d },
	});
}
