#include "syscalls.h"
#include "guest_datatypes.h"

#include <cmath>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace riscv {
static const std::unordered_map<std::string, std::function<uint64_t()>> allowed_objects = {
	{ "Engine", [] { return uint64_t(uintptr_t(godot::Engine::get_singleton())); } },
	{ "Input", [] { return uint64_t(uintptr_t(godot::Input::get_singleton())); } },
	{ "Time", [] { return uint64_t(uintptr_t(godot::Time::get_singleton())); } },
};

inline Sandbox &emu(machine_t &m) {
	return *m.get_userdata<Sandbox>();
}

inline godot::Object *get_object_from_address(Sandbox &emu, uint64_t addr) {
	auto *obj = (godot::Object *)uintptr_t(addr);
	if (obj == nullptr) {
		ERR_PRINT("Object is Null");
		throw std::runtime_error("Object is Null");
	} else if (!emu.is_scoped_object(obj)) {
		ERR_PRINT("Object is not scoped");
		throw std::runtime_error("Object is not scoped");
	}
	return obj;
}
inline godot::Node *get_node_from_address(Sandbox &emu, uint64_t addr) {
	auto *node = (godot::Node *)uintptr_t(addr);
	if (node == nullptr) {
		ERR_PRINT("Node object is Null");
		throw std::runtime_error("Node object is Null");
	} else if (!emu.is_scoped_object(node)) {
		ERR_PRINT("Node object is not scoped");
		throw std::runtime_error("Node object is not scoped");
	}
	return node;
}

#define APICALL(func) static void func(machine_t &machine [[maybe_unused]])

APICALL(api_print) {
	auto [array, len] = machine.sysargs<gaddr_t, gaddr_t>();
	auto &emu = riscv::emu(machine);

	if (len >= 64) {
		ERR_PRINT("print(): Too many Variants to print");
		throw std::runtime_error("print(): Too many Variants to print");
	}
	const GuestVariant *array_ptr = emu.machine().memory.memarray<GuestVariant>(array, len);

	// We really want print_internal to be a public function.
	for (gaddr_t i = 0; i < len; i++) {
		auto &var = array_ptr[i];
		//printf("Variant[%lu]: type=%d\n", i, var.type);
		UtilityFunctions::print(var.toVariant(emu));
	}
}

APICALL(api_vcall) {
	auto [vp, method, mlen, args_ptr, args_size, vret] = machine.sysargs<GuestVariant *, std::string, unsigned, gaddr_t, gaddr_t, GuestVariant *>();
	(void)mlen;

	auto &emu = riscv::emu(machine);

	if (args_size > 8) {
		ERR_PRINT("Variant::call(): Too many arguments");
		throw std::runtime_error("Variant::call(): Too many arguments");
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

		auto *vcall = const_cast<Variant *>(vp->toVariantPtr(emu));
		Variant ret;
		vcall->callp(StringName(method.data()), argptrs.data(), args_size, ret, error);
		vret->set(emu, ret);
	} else if (vp->type == Variant::OBJECT) {
		auto *obj = get_object_from_address(emu, vp->v.i);

		Array vargs;
		vargs.resize(args_size);
		for (unsigned i = 0; i < args_size; i++) {
			vargs[i] = args[i].toVariant(emu);
		}
		Variant ret = obj->callv(String::utf8(method.data(), method.size()), vargs);
		vret->set(emu, ret, true); // Implicit trust, as we are returning engine-provided result.
	} else {
		ERR_PRINT("Invalid Variant type for Variant::call()");
		throw std::runtime_error("Invalid Variant type for Variant::call(): " + std::to_string(vp->type));
	}
}

APICALL(api_veval) {
	auto [op, ap, bp, retp] = machine.sysargs<int, GuestVariant *, GuestVariant *, GuestVariant *>();
	auto &emu = riscv::emu(machine);

	// Special case for comparing objects.
	if (ap->type == Variant::OBJECT && bp->type == Variant::OBJECT) {
		// Special case for equality, allowing invalid objects to be compared.
		if (op == static_cast<int>(Variant::Operator::OP_EQUAL)) {
			machine.set_result(true);
			retp->set(emu, Variant(ap->v.i == bp->v.i));
			return;
		}
		auto *a = get_object_from_address(emu, ap->v.i);
		auto *b = get_object_from_address(emu, bp->v.i);
		auto valid = false;
		Variant ret;
		Variant::evaluate(static_cast<Variant::Operator>(op), a, b, ret, valid);

		machine.set_result(valid);
		retp->set(emu, ret, true); // Implicit trust, as we are returning engine-provided result.
		return;
	}

	auto valid = false;
	Variant ret;
	Variant::evaluate(static_cast<Variant::Operator>(op), ap->toVariant(emu), bp->toVariant(emu), ret, valid);

	machine.set_result(valid);
	retp->set(emu, ret);
}

APICALL(api_vcreate) {
	auto [vp, type, gdata] = machine.sysargs<GuestVariant *, Variant::Type, gaddr_t>();
	auto &emu = riscv::emu(machine);
	machine.penalize(10'000);

	switch (type) {
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH: {
			auto *str = emu.machine().memory.memarray<GuestStdString>(gdata, 1);
			auto godot_str = str->to_godot_string(emu.machine());
			// Create a new Variant with the string, modify vp.
			unsigned idx = emu.create_scoped_variant(Variant(std::move(godot_str)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::ARRAY: {
			// Create a new empty? array, assign to vp.
			Array a;
			if (gdata != 0x0) {
				// Copy std::vector<Variant> from guest memory.
				auto *gvec = emu.machine().memory.memarray<GuestStdVector>(gdata, 1);
				auto vec = gvec->to_vector<GuestVariant>(emu.machine());
				for (const auto &v : vec) {
					a.push_back(std::move(v.toVariant(emu)));
				}
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::DICTIONARY: {
			// Create a new empty? dictionary, assign to vp.
			unsigned idx = emu.create_scoped_variant(Variant(Dictionary()));
			vp->type = type;
			vp->v.i = idx;
		} break;
		default:
			ERR_PRINT("Unsupported Variant type for Variant::create()");
			throw std::runtime_error("Unsupported Variant type for Variant::create()");
	}
}

APICALL(api_vfetch) {
	auto [vp, gdata] = machine.sysargs<GuestVariant *, gaddr_t>();
	auto &emu = riscv::emu(machine);
	machine.penalize(10'000);

	// Find scoped Variant and copy data into gdata.
	auto opt = emu.get_scoped_variant(vp->v.i);
	if (opt.has_value()) {
		auto &var = *opt.value();
		switch (var.get_type()) {
			case Variant::STRING:
			case Variant::STRING_NAME:
			case Variant::NODE_PATH: {
				auto u8str = var.operator String().utf8();
				auto *gstr = emu.machine().memory.memarray<GuestStdString>(gdata, 1);
				gstr->set_string(emu.machine(), gdata, u8str.ptr(), u8str.length());
				break;
			}
			case Variant::PACKED_BYTE_ARRAY: {
				auto *gvec = emu.machine().memory.memarray<GuestStdVector>(gdata, 1);
				auto arr = var.operator PackedByteArray();
				auto [sptr, saddr] = gvec->alloc<uint8_t>(emu.machine(), arr.size());
				std::memcpy(sptr, arr.ptr(), arr.size());
				break;
			}
			case Variant::PACKED_FLOAT32_ARRAY: {
				auto *gvec = emu.machine().memory.memarray<GuestStdVector>(gdata, 1);
				auto arr = var.operator PackedFloat32Array();
				auto [sptr, saddr] = gvec->alloc<float>(emu.machine(), arr.size());
				std::memcpy(sptr, arr.ptr(), arr.size() * sizeof(float));
				break;
			}
			case Variant::PACKED_FLOAT64_ARRAY: {
				auto *gvec = emu.machine().memory.memarray<GuestStdVector>(gdata, 1);
				auto arr = var.operator PackedFloat64Array();
				auto [sptr, saddr] = gvec->alloc<double>(emu.machine(), arr.size());
				std::memcpy(sptr, arr.ptr(), arr.size() * sizeof(double));
				break;
			}
			case Variant::PACKED_INT32_ARRAY: {
				auto *gvec = emu.machine().memory.memarray<GuestStdVector>(gdata, 1);
				auto arr = var.operator PackedInt32Array();
				auto [sptr, saddr] = gvec->alloc<int32_t>(emu.machine(), arr.size());
				std::memcpy(sptr, arr.ptr(), arr.size() * sizeof(int32_t));
				break;
			}
			case Variant::PACKED_INT64_ARRAY: {
				auto *gvec = emu.machine().memory.memarray<GuestStdVector>(gdata, 1);
				auto arr = var.operator PackedInt64Array();
				auto [sptr, saddr] = gvec->alloc<int64_t>(emu.machine(), arr.size());
				std::memcpy(sptr, arr.ptr(), arr.size() * sizeof(int64_t));
				break;
			}
			case Variant::PACKED_VECTOR2_ARRAY: {
				auto *gvec = emu.machine().memory.memarray<GuestStdVector>(gdata, 1);
				auto arr = var.operator PackedVector2Array();
				auto [sptr, saddr] = gvec->alloc<Vector2>(emu.machine(), arr.size());
				std::memcpy(sptr, arr.ptr(), arr.size() * sizeof(Vector2));
				break;
			}
			case Variant::PACKED_VECTOR3_ARRAY: {
				auto *gvec = emu.machine().memory.memarray<GuestStdVector>(gdata, 1);
				auto arr = var.operator PackedVector3Array();
				auto [sptr, saddr] = gvec->alloc<Vector3>(emu.machine(), arr.size());
				std::memcpy(sptr, arr.ptr(), arr.size() * sizeof(Vector3));
				break;
			}
			default:
				ERR_PRINT("vfetch: Cannot fetch value into guest for Variant type");
				throw std::runtime_error("vfetch: Cannot fetch value into guest for Variant type");
		}
	} else {
		ERR_PRINT("vfetch: Variant is not scoped");
		throw std::runtime_error("vfetch: Variant is not scoped");
	}
}

APICALL(api_vclone) {
	auto [vp, vret] = machine.sysargs<GuestVariant *, GuestVariant *>();
	auto &emu = riscv::emu(machine);
	machine.penalize(10'000);

	// Find scoped Variant and clone it.
	auto var = emu.get_scoped_variant(vp->v.i);
	if (var.has_value()) {
		const auto index = emu.create_scoped_variant(var.value()->duplicate());
		vret->type = var.value()->get_type();
		vret->v.i = index;
	} else {
		ERR_PRINT("vclone: Variant is not scoped");
		throw std::runtime_error("vclone: Variant is not scoped");
	}
}

APICALL(api_vstore) {
	auto [vp, gdata] = machine.sysargs<GuestVariant *, gaddr_t>();
	auto &emu = riscv::emu(machine);
	machine.penalize(10'000);

	// Find scoped Variant and store its data into gdata (which could be a, eg., a std::string).
	auto opt = emu.get_scoped_variant(vp->v.i);
	if (opt.has_value()) {
		auto &var = *opt.value();
		switch (var.get_type()) {
			case Variant::STRING:
			case Variant::STRING_NAME:
			case Variant::NODE_PATH: {
				auto *gstr = emu.machine().memory.memarray<GuestStdString>(gdata, 1);
				auto u8str = var.operator String().utf8();
				gstr->set_string(emu.machine(), gdata, u8str.ptr(), u8str.length());
				break;
			}
			default:
				ERR_PRINT("vstore: Cannot store value into guest for Variant type");
				throw std::runtime_error("vstore: Cannot store value into guest for Variant type");
		}
	} else {
		ERR_PRINT("vstore: Variant is not scoped");
		throw std::runtime_error("vstore: Variant is not scoped");
	}
}

APICALL(api_vfree) {
	auto [vp] = machine.sysargs<GuestVariant *>();
	auto &emu = riscv::emu(machine);
	machine.penalize(10'000);

	// XXX: No longer needed, as we are abstracting the Variant object.
}

APICALL(api_get_obj) {
	auto [name] = machine.sysargs<std::string>();
	auto &emu = riscv::emu(machine);
	machine.penalize(150'000);

	// Find allowed object by name and get its address from a lambda.
	auto it = allowed_objects.find(name);
	if (it != allowed_objects.end()) {
		auto obj = it->second();
		emu.add_scoped_object(reinterpret_cast<godot::Object *>(obj));
		machine.set_result(obj);
		return;
	}
	// Special case for SceneTree.
	if (name == "SceneTree") {
		// Get the current SceneTree.
		auto *owner_node = emu.get_tree_base();
		if (owner_node == nullptr) {
			ERR_PRINT("Sandbox has no parent Node");
			machine.set_result(0);
			return;
		}
		auto *tree = owner_node->get_tree();
		emu.add_scoped_object(tree);
		machine.set_result(uint64_t(uintptr_t(tree)));
	} else {
		ERR_PRINT(("Unknown or inaccessible object: " + name).c_str());
		machine.set_result(0);
	}
}

APICALL(api_obj) {
	auto [op, addr, gvar] = machine.sysargs<int, uint64_t, gaddr_t>();
	auto &emu = riscv::emu(machine);
	machine.penalize(250'000); // Costly Object operations.

	auto *obj = reinterpret_cast<godot::Object *>(uintptr_t(addr));
	if (!emu.is_scoped_object(obj)) {
		ERR_PRINT("Object is not scoped");
		throw std::runtime_error("Object is not scoped");
	}

	switch (Object_Op(op)) {
		case Object_Op::GET_METHOD_LIST: {
			auto *vec = emu.machine().memory.memarray<GuestStdVector>(gvar, 1);
			// XXX: vec->free(emu.machine());
			auto methods = obj->get_method_list();
			auto [sptr, saddr] = vec->alloc<GuestStdString>(emu.machine(), methods.size());
			for (size_t i = 0; i < methods.size(); i++) {
				Dictionary dict = methods[i].operator godot::Dictionary();
				auto name = String(dict["name"]).utf8();
				const auto self = saddr + sizeof(GuestStdString) * i;
				sptr[i].set_string(emu.machine(), self, name.ptr(), name.length());
			}
		} break;
		case Object_Op::GET: { // Get a property of the object.
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 2);
			auto name = var[0].toVariant(emu).operator String();
			var[1].set(emu, obj->get(name), true); // Implicit trust, as we are returning engine-provided result.
		} break;
		case Object_Op::SET: { // Set a property of the object.
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 2);
			auto name = var[0].toVariant(emu).operator String();
			obj->set(name, var[1].toVariant(emu));
		} break;
		case Object_Op::GET_PROPERTY_LIST: {
			auto *vec = emu.machine().memory.memarray<GuestStdVector>(gvar, 1);
			// XXX: vec->free(emu.machine());
			auto properties = obj->get_property_list();
			auto [sptr, saddr] = vec->alloc<GuestStdString>(emu.machine(), properties.size());
			for (size_t i = 0; i < properties.size(); i++) {
				Dictionary dict = properties[i].operator godot::Dictionary();
				auto name = String(dict["name"]).utf8();
				const auto self = saddr + sizeof(GuestStdString) * i;
				sptr[i].set_string(emu.machine(), self, name.ptr(), name.length());
			}
		} break;
		case Object_Op::CONNECT: {
			auto *vars = emu.machine().memory.memarray<GuestVariant>(gvar, 3);
			auto *target = get_object_from_address(emu, vars[0].v.i);
			auto callable = Callable(target, vars[2].toVariant(emu).operator String());
			obj->connect(vars[1].toVariant(emu).operator String(), callable);
		} break;
		case Object_Op::DISCONNECT: {
			auto *vars = emu.machine().memory.memarray<GuestVariant>(gvar, 3);
			auto *target = get_object_from_address(emu, vars[0].v.i);
			auto callable = Callable(target, vars[2].toVariant(emu).operator String());
			obj->disconnect(vars[1].toVariant(emu).operator String(), callable);
		} break;
		case Object_Op::GET_SIGNAL_LIST: {
			auto *vec = emu.machine().memory.memarray<GuestStdVector>(gvar, 1);
			// XXX: vec->free(emu.machine());
			auto signals = obj->get_signal_list();
			auto [sptr, saddr] = vec->alloc<GuestStdString>(emu.machine(), signals.size());
			for (size_t i = 0; i < signals.size(); i++) {
				Dictionary dict = signals[i].operator godot::Dictionary();
				auto name = String(dict["name"]).utf8();
				const auto self = saddr + sizeof(GuestStdString) * i;
				sptr[i].set_string(emu.machine(), self, name.ptr(), name.length());
			}
		} break;
		default:
			throw std::runtime_error("Invalid Object operation");
	}
}

APICALL(api_obj_callp) {
	auto [addr, method, deferred, vret, args_addr, args_size] = machine.sysargs<uint64_t, std::string_view, bool, GuestVariant *, gaddr_t, unsigned>();
	auto &emu = riscv::emu(machine);
	machine.penalize(250'000); // Costly Object call operation.

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
			vargs[i] = std::move(g_args[i].toVariant(emu));
		}
		Variant ret = obj->callv(String::utf8(method.data(), method.size()), vargs);
		vret->set(emu, ret, true); // Implicit trust, as we are returning engine-provided result.
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
	auto [addr, name] = machine.sysargs<uint64_t, std::string_view>();
	auto &emu = riscv::emu(machine);
	machine.penalize(150'000);
	Node *node = nullptr;
	const std::string c_name(name);

	if (addr == 0) {
		auto *owner_node = emu.get_tree_base();
		if (owner_node == nullptr) {
			ERR_PRINT("Sandbox has no parent Node");
			machine.set_result(0);
			return;
		}
		node = owner_node->get_node<Node>(NodePath(c_name.c_str()));
	} else {
		Node *base_node = reinterpret_cast<Node *>(uintptr_t(addr));
		if (!emu.is_scoped_object(base_node)) {
			ERR_PRINT("Node object is not scoped");
			machine.set_result(0);
			return;
		}
		node = base_node->get_node<Node>(NodePath(c_name.c_str()));
	}
	if (node == nullptr) {
		ERR_PRINT(("Node not found: " + c_name).c_str());
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
	// Get the Node object by its address.
	godot::Node *node = get_node_from_address(emu, addr);

	switch (Node_Op(op)) {
		case Node_Op::GET_NAME: {
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			var->create(emu, String(node->get_name()));
		} break;
		case Node_Op::GET_PATH: {
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			var->create(emu, String(node->get_path()));
		} break;
		case Node_Op::GET_PARENT: {
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			if (node->get_parent() == nullptr) {
				var->set(emu, Variant());
			} else {
				// TODO: Parent nodes allow access higher up the tree, which could be a security issue.
				var->set(emu, node->get_parent(), true); // Implicit trust, as we are returning engine-provided result.
			}
		} break;
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
			var->set(emu, new_node, true); // Implicit trust, as we are returning our own object.
		} break;
		case Node_Op::GET_CHILD_COUNT: {
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			var->set(emu, node->get_child_count());
		} break;
		case Node_Op::GET_CHILD: {
			auto *var = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			auto *child_node = node->get_child(var[0].v.i);
			if (child_node == nullptr) {
				var[0].set(emu, Variant());
			} else {
				emu.add_scoped_object(child_node);
				var[0].set(emu, int64_t(uintptr_t(child_node)));
			}
		} break;
		case Node_Op::ADD_CHILD_DEFERRED:
		case Node_Op::ADD_CHILD: {
			auto *child = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			auto *child_node = get_node_from_address(emu, child->v.i);
			if (Node_Op(op) == Node_Op::ADD_CHILD_DEFERRED)
				node->call_deferred("add_child", child_node);
			else
				node->add_child(child_node);
		} break;
		case Node_Op::ADD_SIBLING_DEFERRED:
		case Node_Op::ADD_SIBLING: {
			auto *sibling = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			auto *sibling_node = get_node_from_address(emu, sibling->v.i);
			if (Node_Op(op) == Node_Op::ADD_SIBLING_DEFERRED)
				node->call_deferred("add_sibling", sibling_node);
			else
				node->add_sibling(sibling_node);
		} break;
		case Node_Op::MOVE_CHILD: {
			auto *vars = emu.machine().memory.memarray<GuestVariant>(gvar, 2);
			auto *child_node = get_node_from_address(emu, vars[0].v.i);
			// TODO: Check if the child is actually a child of the node? Verify index?
			node->move_child(child_node, vars[1].v.i);
		} break;
		case Node_Op::REMOVE_CHILD_DEFERRED:
		case Node_Op::REMOVE_CHILD: {
			auto *child = emu.machine().memory.memarray<GuestVariant>(gvar, 1);
			auto *child_node = get_node_from_address(emu, child->v.i);
			if (Node_Op(op) == Node_Op::REMOVE_CHILD_DEFERRED)
				node->call_deferred("remove_child", child_node);
			else
				node->remove_child(child_node);
		} break;
		case Node_Op::GET_CHILDREN: {
			// Get a GuestStdVector from guest to store the children.
			auto *vec = emu.machine().memory.memarray<GuestStdVector>(gvar, 1);
			// Get the children of the node.
			auto children = node->get_children();
			// Allocate memory for the children in the guest vector.
			auto [cptr, _] = vec->alloc<uint64_t>(emu.machine(), children.size());
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
		default:
			throw std::runtime_error("Invalid Node operation");
	}
}

APICALL(api_node2d) {
	// Node2D operation, Node2D address, and the variant to get/set the value.
	auto [op, addr, gvar] = machine.sysargs<int, uint64_t, gaddr_t>();
	machine.penalize(100'000); // Costly Node2D operations.

	auto &emu = riscv::emu(machine);
	// Get the Node2D object by its address.
	godot::Node *node = get_node_from_address(emu, addr);

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
	// Get the Node3D object by its address
	godot::Node *node = get_node_from_address(emu, addr);

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

APICALL(api_throw) {
	auto [type, msg, vaddr] = machine.sysargs<std::string_view, std::string_view, gaddr_t>();

	auto &emu = riscv::emu(machine);
	auto *var = emu.machine().memory.memarray<GuestVariant>(vaddr, 1);
	String error_string = "Sandbox exception of type " + String::utf8(type.data(), type.size()) + ": " + String::utf8(msg.data(), msg.size()) + " for Variant of type " + itos(var->type);
	ERR_PRINT(error_string);
	throw std::runtime_error("Sandbox exception of type " + std::string(type) + ": " + std::string(msg) + " for Variant of type " + std::to_string(var->type));
}

APICALL(api_vector2_length) {
	auto [dx, dy] = machine.sysargs<float, float>();
	const float length = std::sqrt(dx * dx + dy * dy);
	machine.set_result(length);
}

APICALL(api_vector2_normalize) {
	auto [dx, dy] = machine.sysargs<float, float>();
	const float length = std::sqrt(dx * dx + dy * dy);
	if (length > 0.0001f) // FLT_EPSILON?
	{
		dx /= length;
		dy /= length;
	}
	machine.set_result(dx, dy);
}

APICALL(api_vector2_rotated) {
	auto [dx, dy, angle] = machine.sysargs<float, float, float>();
	const float x = std::cos(angle) * dx - std::sin(angle) * dy;
	const float y = std::sin(angle) * dx + std::cos(angle) * dy;
	machine.set_result(x, y);
}

APICALL(api_array_ops) {
	auto [op, arr_idx, idx, vaddr] = machine.sysargs<Array_Op, unsigned, int, gaddr_t>();
	auto &emu = riscv::emu(machine);

	auto opt_array = emu.get_scoped_variant(arr_idx);
	if (!opt_array.has_value() || opt_array.value()->get_type() != Variant::ARRAY) {
		ERR_PRINT("Invalid Array object");
		throw std::runtime_error("Invalid Array object, idx = " + std::to_string(arr_idx));
	}
	godot::Array array = opt_array.value()->operator Array();

	switch (op) {
		case Array_Op::CREATE: {
			Array a;
			a.resize(arr_idx); // Resize the array to the given size.
			const unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			auto *vp = emu.machine().memory.memarray<GuestVariant>(vaddr, 1);
			vp->type = Variant::ARRAY;
			vp->v.i = idx;
			break;
		}
		case Array_Op::PUSH_BACK:
			array.push_back(emu.machine().memory.memarray<GuestVariant>(vaddr, 1)->toVariant(emu));
			break;
		case Array_Op::PUSH_FRONT:
			array.push_front(emu.machine().memory.memarray<GuestVariant>(vaddr, 1)->toVariant(emu));
			break;
		case Array_Op::POP_AT:
			array.pop_at(idx);
			break;
		case Array_Op::POP_BACK:
			array.pop_back();
			break;
		case Array_Op::POP_FRONT:
			array.pop_front();
			break;
		case Array_Op::INSERT:
			array.insert(idx, emu.machine().memory.memarray<GuestVariant>(vaddr, 1)->toVariant(emu));
			break;
		case Array_Op::ERASE:
			array.erase(idx);
			break;
		case Array_Op::RESIZE:
			array.resize(idx);
			break;
		case Array_Op::CLEAR:
			array.clear();
			break;
		case Array_Op::SORT:
			array.sort();
			break;
		default:
			ERR_PRINT("Invalid Array operation");
			throw std::runtime_error("Invalid Array operation");
	}
}

APICALL(api_array_at) {
	auto [arr_idx, idx, vret] = machine.sysargs<unsigned, int, GuestVariant *>();
	auto &emu = riscv::emu(machine);

	auto opt_array = emu.get_scoped_variant(arr_idx);
	if (!opt_array.has_value() || opt_array.value()->get_type() != Variant::ARRAY) {
		ERR_PRINT("Invalid Array object");
		throw std::runtime_error("Invalid Array object");
	}

	godot::Array array = opt_array.value()->operator Array();
	if (idx < 0 || idx >= array.size()) {
		ERR_PRINT("Array index out of bounds");
		throw std::runtime_error("Array index out of bounds");
	}

	vret->set(emu, array[idx]);
}

APICALL(api_array_size) {
	auto [arr_idx] = machine.sysargs<unsigned>();
	auto &emu = riscv::emu(machine);

	auto opt_array = emu.get_scoped_variant(arr_idx);
	if (!opt_array.has_value() || opt_array.value()->get_type() != Variant::ARRAY) {
		ERR_PRINT("Invalid Array object");
		throw std::runtime_error("Invalid Array object");
	}

	godot::Array array = opt_array.value()->operator Array();
	machine.set_result(array.size());
}

APICALL(api_dict_ops) {
	auto [op, dict_idx, vkey, vaddr] = machine.sysargs<Dictionary_Op, unsigned, gaddr_t, gaddr_t>();
	auto &emu = riscv::emu(machine);

	auto opt_dict = emu.get_scoped_variant(dict_idx);
	if (!opt_dict.has_value() || opt_dict.value()->get_type() != Variant::DICTIONARY) {
		ERR_PRINT("Invalid Dictionary object");
		throw std::runtime_error("Invalid Dictionary object");
	}
	godot::Dictionary dict = opt_dict.value()->operator Dictionary();

	switch (op) {
		case Dictionary_Op::GET: {
			auto *key = emu.machine().memory.memarray<GuestVariant>(vkey, 1);
			auto *vp = emu.machine().memory.memarray<GuestVariant>(vaddr, 1);
			// TODO: Check if the value is already scoped?
			auto v = dict[key->toVariant(emu)];
			vp->create(emu, std::move(v));
			break;
		}
		case Dictionary_Op::SET: {
			auto *key = emu.machine().memory.memarray<GuestVariant>(vkey, 1);
			auto *value = emu.machine().memory.memarray<GuestVariant>(vaddr, 1);
			dict[key->toVariant(emu)] = value->toVariant(emu);
			break;
		}
		case Dictionary_Op::ERASE: {
			auto *key = emu.machine().memory.memarray<GuestVariant>(vkey, 1);
			dict.erase(key->toVariant(emu));
			break;
		}
		case Dictionary_Op::HAS: {
			auto *key = emu.machine().memory.memarray<GuestVariant>(vkey, 1);
			machine.set_result(dict.has(key->toVariant(emu)));
			break;
		}
		case Dictionary_Op::GET_SIZE:
			machine.set_result(dict.size());
			break;
		case Dictionary_Op::CLEAR:
			dict.clear();
			break;
		case Dictionary_Op::MERGE: {
			auto *other_dict = emu.machine().memory.memarray<GuestVariant>(vkey, 1);
			dict.merge(other_dict->toVariant(emu).operator Dictionary());
			break;
		}
		default:
			ERR_PRINT("Invalid Dictionary operation");
			throw std::runtime_error("Invalid Dictionary operation");
	}
}

APICALL(api_string_create) {
	auto [strview] = machine.sysargs<std::string_view>();
	auto &emu = riscv::emu(machine);
	machine.penalize(10'000);

	auto str = String::utf8(strview.data(), strview.size());
	const unsigned idx = emu.create_scoped_variant(Variant(std::move(str)));
	machine.set_result(idx);
}

APICALL(api_string_ops) {
	auto [op, str_idx, index, vaddr] = machine.sysargs<String_Op, unsigned, int, gaddr_t>();
	auto &emu = riscv::emu(machine);

	auto opt_str = emu.get_scoped_variant(str_idx);
	if (!opt_str.has_value() || opt_str.value()->get_type() != Variant::STRING) {
		ERR_PRINT("Invalid String object");
		throw std::runtime_error("Invalid String object");
	}
	godot::String str = opt_str.value()->operator String();

	switch (op) {
		case String_Op::APPEND: {
			auto *gvar = emu.machine().memory.memarray<GuestVariant>(vaddr, 1);
			str += gvar->toVariant(emu).operator String();
			break;
		}
		case String_Op::GET_LENGTH:
			machine.set_result(str.length());
			break;
		case String_Op::TO_STD_STRING: {
			// Get the string as a std::string.
			auto utf8 = str.utf8();
			auto *gstr = emu.machine().memory.memarray<GuestStdString>(vaddr, 1);
			gstr->set_string(emu.machine(), vaddr, utf8.ptr(), utf8.length());
			break;
		}
		default:
			ERR_PRINT("Invalid String operation");
			throw std::runtime_error("Invalid String operation");
	}
}

APICALL(api_string_at) {
	auto [str_idx, index] = machine.sysargs<unsigned, int>();
	auto &emu = riscv::emu(machine);

	auto opt_str = emu.get_scoped_variant(str_idx);
	if (!opt_str.has_value() || opt_str.value()->get_type() != Variant::STRING) {
		ERR_PRINT("Invalid String object");
		throw std::runtime_error("Invalid String object");
	}
	godot::String str = opt_str.value()->operator String();

	if (index < 0 || index >= str.length()) {
		ERR_PRINT("String index out of bounds");
		throw std::runtime_error("String index out of bounds");
	}

	auto new_string = str[index];
	auto new_varidx = emu.create_scoped_variant(Variant(std::move(new_string)));
	machine.set_result(new_varidx);
}

APICALL(api_string_size) {
	auto [str_idx] = machine.sysargs<unsigned>();
	auto &emu = riscv::emu(machine);

	auto opt_str = emu.get_scoped_variant(str_idx);
	if (!opt_str.has_value() || opt_str.value()->get_type() != Variant::STRING) {
		ERR_PRINT("Invalid String object");
		throw std::runtime_error("Invalid String object");
	}
	godot::String str = opt_str.value()->operator String();
	machine.set_result(str.length());
}

APICALL(api_string_append) {
	auto [str_idx, strview] = machine.sysargs<unsigned, std::string_view>();
	auto &emu = riscv::emu(machine);

	auto &var = emu.get_mutable_scoped_variant(str_idx);

	godot::String str = var.operator String();
	str += String::utf8(strview.data(), strview.size());
	var = Variant(std::move(str));
}

APICALL(api_timer_periodic) {
	auto [interval, oneshot, callback, capture, vret] = machine.sysargs<double, bool, gaddr_t, std::array<uint8_t, 32> *, GuestVariant *>();
	auto &emu = riscv::emu(machine);
	machine.penalize(100'000); // Costly Timer node creation.

	Timer *timer = memnew(Timer);
	timer->set_wait_time(interval);
	timer->set_one_shot(oneshot);
	Node *topnode = emu.get_tree_base();
	topnode->add_child(timer);
	timer->set_owner(topnode);
	timer->start();
	// Copy the callback capture storage to the timer timeout callback.
	PackedByteArray capture_data;
	capture_data.resize(capture->size());
	memcpy(capture_data.ptrw(), capture->data(), capture->size());
	// Connect the timer to the guest callback function.
	Array args;
	args.push_back(Variant(timer));
	args.push_back(Variant(std::move(capture_data)));
	timer->connect("timeout", emu.vmcallable_address(callback, std::move(args)));
	// Return the timer object to the guest.
	vret->set(emu, timer, true); // Implicit trust, as we are returning our own object.
}

APICALL(api_timer_stop) {
	throw std::runtime_error("timer_stop: Not implemented");
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
			{ ECALL_GET_OBJ, api_get_obj },
			{ ECALL_OBJ, api_obj },
			{ ECALL_OBJ_CALLP, api_obj_callp },
			{ ECALL_GET_NODE, api_get_node },
			{ ECALL_NODE, api_node },
			{ ECALL_NODE2D, api_node2d },
			{ ECALL_NODE3D, api_node3d },
			{ ECALL_THROW, api_throw },
			{ ECALL_IS_EDITOR, [](machine_t &machine) {
				 machine.set_result(godot::Engine::get_singleton()->is_editor_hint());
			 } },
			{ ECALL_SINCOS, [](machine_t &machine) {
				 auto [angle] = machine.sysargs<float>();
				 machine.set_result(std::cos(angle), std::sin(angle));
			 } },
			{ ECALL_VEC2_LENGTH, api_vector2_length },
			{ ECALL_VEC2_NORMALIZED, api_vector2_normalize },
			{ ECALL_VEC2_ROTATED, api_vector2_rotated },

			{ ECALL_VCREATE, api_vcreate },
			{ ECALL_VFETCH, api_vfetch },
			{ ECALL_VCLONE, api_vclone },
			{ ECALL_VSTORE, api_vstore },

			{ ECALL_ARRAY_OPS, api_array_ops },
			{ ECALL_ARRAY_AT, api_array_at },
			{ ECALL_ARRAY_SIZE, api_array_size },

			{ ECALL_DICTIONARY_OPS, api_dict_ops },

			{ ECALL_STRING_CREATE, api_string_create },
			{ ECALL_STRING_OPS, api_string_ops },
			{ ECALL_STRING_AT, api_string_at },
			{ ECALL_STRING_SIZE, api_string_size },
			{ ECALL_STRING_APPEND, api_string_append },

			{ ECALL_TIMER_PERIODIC, api_timer_periodic },
			{ ECALL_TIMER_STOP, api_timer_stop },
	});
}
