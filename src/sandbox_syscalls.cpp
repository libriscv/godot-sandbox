#include "guest_datatypes.h"
#include "gdscript/compiler/call_abi.h"
#include "syscalls.h"

#include <algorithm>

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/templates/hashfuncs.hpp>
//#define ENABLE_SYSCALL_TRACE 1
#include "syscalls_helpers.hpp"
#include <libriscv/rv32i_instr.hpp>

#define PENALIZE(x) \
	if (!emu.get_profiling()) { \
		machine.penalize(x); \
	}

// Break state in debug_safegdscript.cpp. No-op for non-.sgd guests.
#ifndef SAFEGDSCRIPT_DISABLED
void safegdscript_breakpoint(Sandbox &p_sandbox, uint32_t p_reported_line, bool p_user_stop,
		bool p_source_stop);
String safegdscript_source_location(Sandbox &p_sandbox, gaddr_t p_pc);

// Nested-class script instances, in script_class_safegdscript.cpp. A Sandbox
// with no SafeGDScript behind it owns no Script resources; both are no-ops there.
void safegdscript_bind_nested_class(Sandbox &p_sandbox, godot::Object *p_base,
		const godot::Dictionary &p_instance, const godot::String &p_class_name);
void safegdscript_bypass_super(godot::Object *p_object, const godot::StringName &p_method);
bool safegdscript_nominal_uses(godot::Object *p_object,
		const godot::StringName &p_trait, bool &r_recognized);
#else
static void safegdscript_breakpoint(Sandbox &, uint32_t, bool, bool) {}
static String safegdscript_source_location(Sandbox &, gaddr_t) { return {}; }
static void safegdscript_bind_nested_class(Sandbox &, godot::Object *,
		const godot::Dictionary &, const godot::String &) {}
static void safegdscript_bypass_super(godot::Object *, const godot::StringName &) {}
static bool safegdscript_nominal_uses(godot::Object *,
		const godot::StringName &, bool &r_recognized) {
	r_recognized = false;
	return false;
}
#endif

namespace riscv {
extern std::unordered_map<std::string, std::function<uint64_t()>> global_singleton_list;

// The largest number of elements a guest may ask an array-like Variant to hold. Godot
// treats an allocation failure as fatal, so a length the guest invented has to be
// rejected here rather than discovered when the allocator gives up.
static constexpr int64_t MAX_ARRAY_ELEMENTS = 16'777'216;

godot::Object *get_object_from_address(const Sandbox &emu, uint64_t addr) {
	SYS_TRACE("get_object_from_address", addr);
	if (UNLIKELY(addr == 0)) {
		ERR_PRINT("Object is Null");
		throw std::runtime_error("Object is Null");
	}
	const uint64_t object_id = Sandbox::object_id_from_handle(addr);
	char buffer[256];
	if (UNLIKELY(object_id == 0)) {
		snprintf(buffer, sizeof(buffer), "Object is not found, but likely a Variant with index: %ld", long(int32_t(addr)));
		ERR_PRINT(buffer);
		throw std::runtime_error(buffer);
	}

	if (emu.is_object_access_unrestricted()) {
		if (godot::Object *live = emu.resolve_live_object(object_id))
			return live;
		snprintf(buffer, sizeof(buffer), "Object no longer exists: %lu", (unsigned long)object_id);
		ERR_PRINT(buffer);
		throw std::runtime_error(buffer);
	}

	if (Sandbox::CurrentState::ScopedObject *so = emu.find_scoped_object(object_id))
		return Sandbox::resolve_scoped_object(*so);

	if (godot::Object *allowed = emu.get_explicitly_allowed_object(object_id))
		return allowed;

	snprintf(buffer, sizeof(buffer), "Object is not scoped: %lu", (unsigned long)object_id);
	ERR_PRINT(buffer);
	throw std::runtime_error(buffer);
}
/// @brief Look up a scoped object and confirm it is a T, throwing if it is not.
template <typename T>
inline T *get_class_from_address(const Sandbox &emu, uint64_t addr) {
	godot::Object *obj = get_object_from_address(emu, addr);
	T *result = fast_cast_to<T>(obj);
	if (UNLIKELY(result == nullptr)) {
		const godot::String class_name = godot::String(T::get_class_static());
		ERR_PRINT("Object is not a " + class_name + ": " + obj->get_class());
		throw std::runtime_error("Object was not a " + std::string(class_name.utf8().get_data()));
	}
	return result;
}

inline godot::Node *get_node_from_address(const Sandbox &emu, uint64_t addr) {
	SYS_TRACE("get_node_from_address", addr);
	return get_class_from_address<godot::Node>(emu, addr);
}

// Resolved once at load time: godot::Object::call(), the variadic entry point that
// dispatches by name, honouring script instances the way GDScript's own calls do.
static GDExtensionMethodBindPtr object_call_mtd = nullptr;

// Storage for a Variant that Godot constructs in place. The method-bind call always
// placement-constructs its return value, so default-constructing one first only pays
// for an out-of-line call into Godot to write a nil that is immediately overwritten.
struct CallResult {
	Variant &get() noexcept { return *std::launder(reinterpret_cast<Variant *>(&storage)); }
	// Only flagged once Godot has actually placed a Variant here, so that an exception
	// thrown while marshalling arguments cannot destroy uninitialized storage.
	void mark_constructed() noexcept { m_constructed = true; }
	~CallResult() {
		if (m_constructed)
			get().~Variant();
	}

private:
	std::aligned_storage_t<sizeof(Variant), alignof(Variant)> storage;
	bool m_constructed = false;
};

// Throws on any call error: a failed call answers Nil, indistinguishable from a method
// that returned nothing. Probe with has_method(), not by reading the result back.
static inline void object_callp(godot::Object *obj, const Variant **args, int argc, CallResult &result) {
	GDExtensionCallError error;
	internal::gdextension_interface_object_method_bind_call(object_call_mtd, obj->_owner, reinterpret_cast<GDExtensionConstVariantPtr *>(args), argc, &result.get(), &error);
	result.mark_constructed();
	if (UNLIKELY(error.error != GDEXTENSION_CALL_OK)) {
		// Object::call("call", "name", ...) wraps the real name in args[1].
		const bool via_call = argc >= 2 && args[0]->operator String() == "call" &&
				(args[1]->get_type() == Variant::STRING || args[1]->get_type() == Variant::STRING_NAME);
		const String failed = via_call
				? args[1]->operator String() + " (via call)"
				: args[0]->operator String();
		const CharString method = failed.utf8();
		const int skip = via_call ? 2 : 1;
		throw_on_call_error(error, std::string_view(method.get_data(), method.length()),
				obj->get_class(), args + skip, argc - skip);
	}
}

// Scratch space for arguments that have to be materialized as real Variants.
// godot::Variant has a non-trivial constructor and destructor, so an array of them
// costs 8 constructions and 8 destructions per call even when nothing is passed.
// This constructs exactly the slots that are used, and unwinds them on the way out.
template <unsigned CAPACITY>
struct VariantScratchN {
	static constexpr unsigned MAX = CAPACITY;

	Variant *emplace(Variant &&value) {
		return new (&storage[m_count++]) Variant(std::move(value));
	}
	~VariantScratchN() {
		for (unsigned i = 0; i < m_count; i++)
			std::launder(reinterpret_cast<Variant *>(&storage[i]))->~Variant();
	}

private:
	std::aligned_storage_t<sizeof(Variant), alignof(Variant)> storage[MAX];
	unsigned m_count = 0;
};

// SafeGDScript calls use the shared boxed-call limit. print() takes more, and
// says so where it declares its own scratch.
using VariantScratch = VariantScratchN<gdscript::CallABI::MAX_ARGUMENTS>;

// Scratch space for read-only guest arguments. Unlike VariantScratchN, an inline
// scalar/vector is exposed in place and a scoped value is referenced directly, so
// neither path pays a godot::Variant construction and destruction pair.
template <unsigned CAPACITY>
struct BorrowedVariantScratchN {
	const Variant *emplace(const Sandbox &emu, const GuestVariant &value) {
		BorrowedVariant *borrowed = new (&storage[m_count++]) BorrowedVariant(emu, value);
		return &**borrowed;
	}
	~BorrowedVariantScratchN() {
		for (unsigned i = 0; i < m_count; i++)
			std::launder(reinterpret_cast<BorrowedVariant *>(&storage[i]))->~BorrowedVariant();
	}

private:
	std::aligned_storage_t<sizeof(BorrowedVariant), alignof(BorrowedVariant)> storage[CAPACITY];
	unsigned m_count = 0;
};

using BorrowedVariantScratch = BorrowedVariantScratchN<gdscript::CallABI::MAX_ARGUMENTS>;

// The counterpart to object_callp() for the built-in Variant types. Godot
// placement-constructs the return value here too, so it goes straight into
// uninitialized storage instead of overwriting a freshly made nil.
static inline void variant_callp(Variant *self, const StringName &method, const Variant **args, int argc, CallResult &result, GDExtensionCallError &error) {
	internal::gdextension_interface_variant_call(self->_native_ptr(), method._native_ptr(), reinterpret_cast<GDExtensionConstVariantPtr *>(args), argc, &result.get(), &error);
	result.mark_constructed();
}

// A guest class that extends an engine class is a Dictionary holding that engine
// object under `@base`, alongside the class's own fields. The guest keeps the
// Dictionary, but Godot only knows the object: an engine method takes it as an
// argument, and answers a method the Dictionary does not have.
static constexpr const char *CLASS_INSTANCE_BASE_KEY = "@base";

static inline godot::Object *class_instance_base(Sandbox &emu, const Variant &v) {
	if (LIKELY(variant_type(v) != Variant::DICTIONARY)) {
		return nullptr;
	}
	const Dictionary dict = v;
	if (!dict.has(CLASS_INSTANCE_BASE_KEY)) {
		return nullptr;
	}
	const Variant base = dict[CLASS_INSTANCE_BASE_KEY];
	if (variant_type(base) != Variant::OBJECT) {
		return nullptr;
	}
	// Resolved the way an OBJECT argument is, so a restricted Sandbox still decides.
	return get_object_from_address(emu,
			Sandbox::object_handle_from_id(Sandbox::engine_object_id(base.operator godot::Object *())));
}

static uint32_t method_compatibility_hash(const MethodInfo &method) {
	const bool has_return = method.return_val.type != Variant::NIL ||
			(method.return_val.usage & PROPERTY_USAGE_NIL_IS_VARIANT);
	uint32_t hash = hash_murmur3_one_32(has_return);
	hash = hash_murmur3_one_32(uint32_t(method.arguments.size()), hash);
	if (has_return) {
		hash = hash_murmur3_one_32(method.return_val.type, hash);
		if (method.return_val.class_name != StringName())
			hash = hash_murmur3_one_32(method.return_val.class_name.hash(), hash);
	}
	for (const PropertyInfo &arg : method.arguments) {
		hash = hash_murmur3_one_32(arg.type, hash);
		if (arg.class_name != StringName())
			hash = hash_murmur3_one_32(arg.class_name.hash(), hash);
	}
	hash = hash_murmur3_one_32(uint32_t(method.default_arguments.size()), hash);
	for (const Variant &value : method.default_arguments)
		hash = hash_murmur3_one_32(value.hash(), hash);
	hash = hash_murmur3_one_32(method.flags & GDEXTENSION_METHOD_FLAG_CONST ? 1 : 0, hash);
	hash = hash_murmur3_one_32(method.flags & GDEXTENSION_METHOD_FLAG_VARARG ? 1 : 0, hash);
	return hash_fmix32(hash);
}

static GDExtensionMethodBindPtr cached_method_bind(godot::Object *obj,
		Sandbox::CachedName &method) {
	const std::type_info *object_type = &typeid(*obj);
	const unsigned index = object_type->hash_code() & (Sandbox::CachedName::METHOD_CACHE_SIZE - 1);
	Sandbox::CachedName::MethodEntry &entry = method.methods[index];
	if (entry.object_type == object_type && entry.resolved)
		return entry.bind;

	entry = {};
	entry.object_type = object_type;
	entry.resolved = true;

	StringName class_name;
	if (!internal::gdextension_interface_object_get_class_name(obj->_owner, internal::library,
			class_name._native_ptr())) {
		return nullptr;
	}

	const TypedArray<Dictionary> methods = ClassDBSingleton::get_singleton()->class_get_method_list(class_name);
	for (int i = 0; i < methods.size(); i++) {
		const Dictionary info_dict = methods[i];
		if (StringName(info_dict["name"]) != method.sname)
			continue;
		// Script-facing pseudo-methods such as Object.free() have no MethodBind.
		// ClassDB reports those with ID zero; leave them on Object::call().
		if (int64_t(info_dict["id"]) == 0)
			break;
		const MethodInfo info = MethodInfo::from_dict(info_dict);
		entry.bind = internal::gdextension_interface_classdb_get_method_bind(
				class_name._native_ptr(), method.sname._native_ptr(), method_compatibility_hash(info));
		break;
	}
	return entry.bind;
}

static inline void object_call(Sandbox &emu, godot::Object *obj,
		Sandbox::CachedName &method, std::string_view method_name,
		const GuestVariant *args, int argc, CallResult &result, bool native_only = false) {
	SYS_TRACE("object_call", method.sname, argc);
	BorrowedVariantScratch borrowed;
	VariantScratch replacements;
	const Variant *vargs[gdscript::CallABI::MAX_ARGUMENTS];
	for (int i = 0; i < argc; i++) {
		const Variant *arg = borrowed.emplace(emu, args[i]);
		// The guest's own tag gates this: it costs nothing to read, and a wrong one
		// only means an instance is passed along whole, which class_instance_base()
		// re-checks against the resolved Variant anyway.
		if (UNLIKELY(args[i].type == Variant::DICTIONARY)) {
			if (godot::Object *base = class_instance_base(emu, *arg)) {
				arg = replacements.emplace(Variant(base));
			}
		}
		vargs[i] = arg;
	}

	GDExtensionCallError error;
	if (!native_only && internal::gdextension_interface_object_has_script_method(
			obj->_owner, method.sname._native_ptr())) {
		internal::gdextension_interface_object_call_script_method(obj->_owner,
				method.sname._native_ptr(), reinterpret_cast<GDExtensionConstVariantPtr *>(vargs),
				argc, &result.get(), &error);
		result.mark_constructed();
	// Object.call() is itself a variadic dispatcher. Keep it on the fallback so
	// the dispatched method name remains the first user argument; besides
	// preserving script dispatch, object_callp() uses that shape to report
	// failures as "name (via call)" instead of a failure to call "call".
	} else if (GDExtensionMethodBindPtr bind = method_name != "call"
			? cached_method_bind(obj, method) : nullptr) {
		internal::gdextension_interface_object_method_bind_call(bind, obj->_owner,
				reinterpret_cast<GDExtensionConstVariantPtr *>(vargs), argc, &result.get(), &error);
		result.mark_constructed();
	} else {
		// Dynamic extension methods are not necessarily present in ClassDB. Preserve
		// Object::call() as the cold fallback for those and for its normal diagnostics.
		if (native_only)
			safegdscript_bypass_super(obj, method.sname);
		const Variant *fallback_args[gdscript::CallABI::MAX_ARGUMENTS + 1];
		fallback_args[0] = &method.variant;
		for (int i = 0; i < argc; i++)
			fallback_args[i + 1] = vargs[i];
		object_callp(obj, fallback_args, argc + 1, result);
		return;
	}

	if (UNLIKELY(error.error != GDEXTENSION_CALL_OK)) {
		throw_on_call_error(error, method_name, obj->get_class(), vargs, argc);
	}
}

/// @brief Call a method on an Object, after checking that the sandbox allows it.
static inline void object_call_checked(Sandbox &emu, godot::Object *obj,
		Sandbox::CachedNameRef &cached_method, std::string_view method_name,
		const GuestVariant *args, int argc, CallResult &result, bool native_only = false) {
	if (UNLIKELY(!emu.is_allowed_method(obj, cached_method->variant))) {
		ERR_PRINT("Variant::call(): Method not allowed: " + cached_method->variant.operator String());
		throw std::runtime_error("Variant::call(): Method not allowed: " + std::string(method_name));
	}
	object_call(emu, obj, cached_method.get(), method_name, args, argc, result, native_only);
}

/// @brief Call a method on a resolved Variant, whatever the guest labelled it as.
static inline void variant_or_object_call(Sandbox &emu, Variant *vcall,
		Sandbox::CachedNameRef &cached_method, std::string_view method_name,
		const GuestVariant *args, int argc, CallResult &result) {
	// The guest owns the type tag, but not the Variant its index refers to, and the two
	// need not agree. An Object reached through a non-OBJECT tag still has to take the
	// object path: calling it as a built-in Variant would reach every method on it
	// without ever asking is_allowed_method().
	const Variant::Type vtype = variant_type(*vcall);
	if (UNLIKELY(vtype == Variant::OBJECT)) {
		godot::Object *obj = get_object_from_address(emu,
				Sandbox::object_handle_from_id(Sandbox::engine_object_id(vcall->operator godot::Object *())));
		object_call_checked(emu, obj, cached_method, method_name, args, argc, result);
		return;
	}

	const StringName &method_sn = cached_method->sname; // The cache entry is pinned.

	// A class instance is a Dictionary. Dictionary's own methods answer first, the
	// way a class answers before its base does in GDScript; everything else belongs
	// to the engine class it extends.
	if (UNLIKELY(vtype == Variant::DICTIONARY) && !vcall->has_method(method_sn)) {
		if (godot::Object *base = class_instance_base(emu, *vcall)) {
			object_call_checked(emu, base, cached_method, method_name, args, argc, result);
			return;
		}
	}

	BorrowedVariantScratch scratch;
	const Variant *argptrs[gdscript::CallABI::MAX_ARGUMENTS];
	for (int i = 0; i < argc; i++) {
		argptrs[i] = scratch.emplace(emu, args[i]);
	}

	GDExtensionCallError error;
	variant_callp(vcall, method_sn, argptrs, argc, result, error);
	throw_on_call_error(error, method_name, GuestVariant::type_name(vtype), argptrs, argc);
}

// a0 = awaited GuestVariant, a1 = frame base, a2 = frame size, a3 = state index,
// a4 = resume entry, a5 = result slot offset (or -1). Returns 1 when it suspended.
APICALL(api_await) {
	auto [operand, frame_base, frame_size, state_index, resume, result_offset] =
			machine.sysargs<gaddr_t, gaddr_t, unsigned, int, gaddr_t, int>();
	Sandbox &emu = riscv::emu(machine);
	const bool suspended = emu.coroutine_suspend(operand, frame_base, frame_size,
			state_index, resume, result_offset);
	machine.set_result(suspended ? 1 : 0);
}

// a0 = frame base, a1 = frame size. Returns the state index to dispatch to.
APICALL(api_await_restore) {
	auto [frame_base, frame_size] = machine.sysargs<gaddr_t, unsigned>();
	Sandbox &emu = riscv::emu(machine);
	machine.set_result(emu.coroutine_restore(frame_base, frame_size));
}

APICALL(api_call_guest) {
	auto [address, args_addr, argc, result_addr] =
			machine.sysargs<gaddr_t, gaddr_t, unsigned, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	SYS_TRACE("call_guest", address, argc);

	if (argc > gdscript::CallABI::MAX_ARGUMENTS) {
		ERR_PRINT("call_guest: too many arguments");
		throw std::runtime_error("call_guest: too many arguments");
	}
	// Level-0 state: vmcall_internal would reset SP/RA, destroying the initializer frame.
	if (!emu.is_in_vmcall()) {
		ERR_PRINT("call_guest: cannot call into the program outside a host call");
		throw std::runtime_error("call_guest: not inside a host call");
	}
	if (address == 0 || (address & 0x1) != 0) {
		ERR_PRINT("call_guest: address is not executable");
		throw std::runtime_error("call_guest: address is not executable");
	}
	const auto &exec = machine.memory.exec_segment_for(address);
	if (!exec->is_within(address)) {
		ERR_PRINT("call_guest: address is not executable");
		throw std::runtime_error("call_guest: address is not executable");
	}

	std::array<Variant, gdscript::CallABI::MAX_ARGUMENTS> values;
	std::array<const Variant *, gdscript::CallABI::MAX_ARGUMENTS> argv;
	if (argc > 0) {
		const GuestVariant *guest_args = machine.memory.memarray<GuestVariant>(args_addr, argc);
		for (unsigned i = 0; i < argc; i++) {
			values[i] = guest_args[i].toVariant(emu);
			argv[i] = &values[i];
		}
	}

	Variant result = emu.vmcall_internal(address, argv.data(), int(argc));

	// create() owns the value; set() would alias a local that's about to die.
	GuestVariant *g_result = machine.memory.memarray<GuestVariant>(result_addr, 1);
	*g_result = GuestVariant{};
	g_result->create(emu, std::move(result));
}

// a0 = source line. Handler cross-checks against line table. No writeback.
APICALL(api_breakpoint) {
	safegdscript_breakpoint(riscv::emu(machine), uint32_t(machine.cpu.reg(riscv::REG_ARG0)),
			machine.cpu.reg(riscv::REG_ARG1) != 0, machine.cpu.reg(riscv::REG_ARG2) != 0);
}

APICALL(api_print) {
	auto [array, len] = machine.sysargs<gaddr_t, unsigned>();
	Sandbox &emu = riscv::emu(machine);

	if (len >= 64) {
		ERR_PRINT("print(): Too many Variants to print");
		throw std::runtime_error("print(): Too many Variants to print");
	}
	const GuestVariant *array_ptr = machine.memory.memarray<GuestVariant>(array, len);

	// Stringifying an argument is host work the guest asked for, and there can
	// be up to 63 of them in one call. Charge for it, or a print() loop costs
	// the guest nothing while the host does all the formatting.
	PENALIZE(10'000 + 10'000 * len);

	// One line per print() call, not per argument: Godot's print() concatenates
	// its arguments with no separator and emits a single line, and every guest
	// language here spells its print() to mean Godot's.
	//
	// Only the resolution happens here. Turning the arguments into text is left
	// to Sandbox::print(), because stringify() can re-enter the guest and has to
	// run under the same latch as the output. See the comment there.
	BorrowedVariantScratchN<64> scratch;
	const Variant *args[64];
	for (unsigned i = 0; i < len; i++) {
		args[i] = scratch.emplace(emu, array_ptr[i]);
	}
	// A zero-argument print() still prints an empty line, as it does in GDScript.
	emu.print(args, len);
}

// Channelled print: printerr, prints, printt, printraw, print_rich,
// print_verbose, push_error, push_warning. ECALL_PRINT unchanged for ABI compat.
APICALL(api_print_channel) {
	auto [array, len, channel] = machine.sysargs<gaddr_t, unsigned, unsigned>();
	Sandbox &emu = riscv::emu(machine);

	if (len >= 64) {
		ERR_PRINT("print(): Too many Variants to print");
		throw std::runtime_error("print(): Too many Variants to print");
	}
	if (channel >= unsigned(Print_Channel::CHANNEL_COUNT)) {
		ERR_PRINT("print(): Unknown output channel");
		throw std::runtime_error("print(): Unknown output channel: " + std::to_string(channel));
	}
	const GuestVariant *array_ptr = machine.memory.memarray<GuestVariant>(array, len);

	PENALIZE(10'000 + 10'000 * len);

	BorrowedVariantScratchN<64> scratch;
	const Variant *args[64];
	for (unsigned i = 0; i < len; i++) {
		args[i] = scratch.emplace(emu, array_ptr[i]);
	}
	const Print_Channel print_channel = Print_Channel(channel);
	if (print_channel == Print_Channel::PUSH_ERROR ||
			print_channel == Print_Channel::PUSH_WARNING) {
		const String prefix = safegdscript_source_location(emu, machine.cpu.pc());
		if (!prefix.is_empty()) {
			Variant location(prefix);
			const Variant *attributed[64];
			attributed[0] = &location;
			for (unsigned i = 0; i < len; i++) {
				attributed[i + 1] = args[i];
			}
			emu.print(attributed, len + 1, print_channel);
			return;
		}
	}
	emu.print(args, len, print_channel);
}

// -= @GlobalScope's utility functions =-
//
// ECALL_UTILITY performs the GDScript globals that a guest cannot compute for
// itself: str() and len(), which need the Variant API, and the global math
// functions, which need libm.
//
// The formulas below are Godot's, from math_funcs.h. They are repeated here
// rather than called through godot::Math:: for two reasons: several of Godot's
// are written against real_t, so that a single-precision build would answer
// something a double-precision build does not, and the GDScript-to-RISC-V
// compiler evaluates the same ops in
// src/gdscript/compiler/globals.cpp -- for constant folding, for its IR
// interpreter, and for its differential test against a real machine. Those two
// have to agree, so both are written out in doubles. A change to one belongs in
// the other.
static constexpr double UTILITY_PI = 3.1415926535897932384626433833;
static constexpr double UTILITY_TAU = 6.2831853071795864769252867666;
static constexpr double UTILITY_CMP_EPSILON = 0.00001;

static double utility_sign(double x) {
	// Godot's SIGN(): zero, and NaN, are neither positive nor negative.
	return (x < 0.0) ? -1.0 : ((x > 0.0) ? 1.0 : 0.0);
}

static bool utility_is_equal_approx(double a, double b) {
	// Exact equality first, so that infinities compare equal.
	if (a == b) {
		return true;
	}
	double tolerance = UTILITY_CMP_EPSILON * std::fabs(a);
	if (tolerance < UTILITY_CMP_EPSILON) {
		tolerance = UTILITY_CMP_EPSILON;
	}
	return std::fabs(a - b) < tolerance;
}

static bool utility_is_zero_approx(double x) {
	return std::fabs(x) < UTILITY_CMP_EPSILON;
}

static double utility_lerp(double from, double to, double weight) {
	return from + weight * (to - from);
}

static double utility_inverse_lerp(double from, double to, double value) {
	return (value - from) / (to - from);
}

static double utility_angle_difference(double from, double to) {
	const double difference = std::fmod(to - from, UTILITY_TAU);
	return std::fmod(2.0 * difference, UTILITY_TAU) - difference;
}

// The cubic family. Mirrors globals.cpp's eval_cubic_* -- the compiler folds
// with those and the guest calls these, so the two must agree bit for bit.
static double utility_cubic_interpolate(double from, double to, double pre, double post, double weight) {
	return 0.5 *
			((from * 2.0) +
					(-pre + to) * weight +
					(2.0 * pre - 5.0 * from + 4.0 * to - post) * (weight * weight) +
					(-pre + 3.0 * from - 3.0 * to + post) * (weight * weight * weight));
}

// Barry-Goldman: the four values sit at four times rather than at even spacing.
static double utility_cubic_interpolate_in_time(double from, double to, double pre, double post,
		double weight, double to_t, double pre_t, double post_t) {
	const double t = utility_lerp(0.0, to_t, weight);
	const double a1 = utility_lerp(pre, from, (pre_t == 0.0) ? 0.0 : (t - pre_t) / -pre_t);
	const double a2 = utility_lerp(from, to, (to_t == 0.0) ? 0.5 : t / to_t);
	const double a3 = utility_lerp(to, post, (post_t - to_t == 0.0) ? 1.0 : (t - to_t) / (post_t - to_t));
	const double b1 = utility_lerp(a1, a2, (to_t - pre_t == 0.0) ? 0.0 : (t - pre_t) / (to_t - pre_t));
	const double b2 = utility_lerp(a2, a3, (post_t == 0.0) ? 1.0 : t / post_t);
	return utility_lerp(b1, b2, (to_t == 0.0) ? 0.5 : t / to_t);
}

// The angular forms bring all four control values onto one branch of the
// circle first, then interpolate them as ordinary numbers.
struct UtilityCubicAngles {
	double from, to, pre, post;
};

static UtilityCubicAngles utility_cubic_angles(double from, double to, double pre, double post) {
	const double from_rot = std::fmod(from, UTILITY_TAU);
	const double pre_diff = std::fmod(pre - from_rot, UTILITY_TAU);
	const double pre_rot = from_rot + std::fmod(2.0 * pre_diff, UTILITY_TAU) - pre_diff;
	const double to_diff = std::fmod(to - from_rot, UTILITY_TAU);
	const double to_rot = from_rot + std::fmod(2.0 * to_diff, UTILITY_TAU) - to_diff;
	const double post_diff = std::fmod(post - to_rot, UTILITY_TAU);
	const double post_rot = to_rot + std::fmod(2.0 * post_diff, UTILITY_TAU) - post_diff;
	return { from_rot, to_rot, pre_rot, post_rot };
}

static double utility_math_op(Utility_Op op, const double args[8]) {
	const double a = args[0];
	const double b = args[1];
	const double c = args[2];
	const double d = args[3];
	const double e = args[4];
	const double f = args[5];
	const double g = args[6];
	const double h = args[7];

	switch (op) {
		case Utility_Op::FLOOR: return std::floor(a);
		case Utility_Op::CEIL: return std::ceil(a);
		// Godot rounds half away from zero through floor(), not through
		// ::round(), and the two differ where adding 0.5 is not exact.
		case Utility_Op::ROUND: return (a >= 0) ? std::floor(a + 0.5) : -std::floor(-a + 0.5);
		case Utility_Op::SIGN: return utility_sign(a);
		case Utility_Op::SIN: return std::sin(a);
		case Utility_Op::COS: return std::cos(a);
		case Utility_Op::TAN: return std::tan(a);
		case Utility_Op::ASIN: return std::asin(a);
		case Utility_Op::ACOS: return std::acos(a);
		case Utility_Op::ATAN: return std::atan(a);
		case Utility_Op::SINH: return std::sinh(a);
		case Utility_Op::COSH: return std::cosh(a);
		case Utility_Op::TANH: return std::tanh(a);
		case Utility_Op::ASINH: return std::asinh(a);
		case Utility_Op::ACOSH: return std::acosh(a);
		case Utility_Op::ATANH: return std::atanh(a);
		case Utility_Op::EXP: return std::exp(a);
		case Utility_Op::LOG: return std::log(a);
		case Utility_Op::DEG_TO_RAD: return a * UTILITY_PI / 180.0;
		case Utility_Op::RAD_TO_DEG: return a * 180.0 / UTILITY_PI;
		case Utility_Op::LINEAR_TO_DB: return std::log(a) * 8.6858896380650365530225783783321;
		case Utility_Op::DB_TO_LINEAR: return std::exp(a * 0.11512925464970228420089957273422);
		case Utility_Op::IS_NAN: return std::isnan(a) ? 1.0 : 0.0;
		case Utility_Op::IS_INF: return std::isinf(a) ? 1.0 : 0.0;
		case Utility_Op::IS_FINITE: return std::isfinite(a) ? 1.0 : 0.0;
		case Utility_Op::IS_ZERO_APPROX: return utility_is_zero_approx(a) ? 1.0 : 0.0;

		// Math::ease() and Math::step_decimals(). Must match globals.cpp
		// (differential test compares both).
		case Utility_Op::EASE: {
			double x = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
			if (b > 0.0) {
				return (b < 1.0) ? 1.0 - std::pow(1.0 - x, 1.0 / b) : std::pow(x, b);
			}
			if (b < 0.0) {
				return (x < 0.5)
						? std::pow(x * 2.0, -b) * 0.5
						: (1.0 - std::pow(1.0 - (x - 0.5) * 2.0, -b)) * 0.5 + 0.5;
			}
			return 0.0;
		}
		case Utility_Op::STEP_DECIMALS: {
			static const double sd[] = { 0.9999, 0.09999, 0.009999, 0.0009999, 0.00009999,
				0.000009999, 0.0000009999, 0.00000009999, 0.000000009999, 0.0000000009999 };
			const double magnitude = std::fabs(a);
			const double decimals = magnitude - std::floor(magnitude);
			for (int i = 0; i < 10; i++) {
				if (decimals >= sd[i]) {
					return double(i);
				}
			}
			return 0.0;
		}

		case Utility_Op::ATAN2: return std::atan2(a, b);
		case Utility_Op::POW: return std::pow(a, b);
		case Utility_Op::FMOD: return std::fmod(a, b);
		case Utility_Op::FPOSMOD: {
			double value = std::fmod(a, b);
			if ((value < 0 && b > 0) || (value > 0 && b < 0)) {
				value += b;
			}
			value += 0.0;
			return value;
		}
		case Utility_Op::SNAPPED: return (b != 0) ? std::floor(a / b + 0.5) * b : a;
		case Utility_Op::IS_EQUAL_APPROX: return utility_is_equal_approx(a, b) ? 1.0 : 0.0;
		case Utility_Op::ANGLE_DIFFERENCE: return utility_angle_difference(a, b);
		case Utility_Op::PINGPONG: {
			if (b == 0.0) {
				return 0.0;
			}
			const double x = (a - b) / (b * 2.0);
			const double fract = x - std::floor(x);
			return std::fabs(fract * b * 2.0 - b);
		}

		case Utility_Op::LERP: return utility_lerp(a, b, c);
		case Utility_Op::INVERSE_LERP: return utility_inverse_lerp(a, b, c);
		case Utility_Op::SMOOTHSTEP: {
			if (utility_is_equal_approx(a, b)) {
				return a;
			}
			double x = utility_inverse_lerp(a, b, c);
			x = (x < 0.0) ? 0.0 : ((x > 1.0) ? 1.0 : x);
			return x * x * (3.0 - 2.0 * x);
		}
		case Utility_Op::MOVE_TOWARD:
			return std::fabs(b - a) <= c ? b : a + utility_sign(b - a) * c;
		case Utility_Op::LERP_ANGLE:
			return a + utility_angle_difference(a, b) * c;
		case Utility_Op::ROTATE_TOWARD: {
			// A negative delta moves no further than PI radians away from `to`,
			// which is the largest an angular distance can be.
			const double difference = utility_angle_difference(a, b);
			const double abs_difference = std::fabs(difference);
			const double lower = abs_difference - UTILITY_PI;
			double delta = c;
			delta = (delta < lower) ? lower : ((delta > abs_difference) ? abs_difference : delta);
			return a + delta * ((difference >= 0.0) ? 1.0 : -1.0);
		}
		case Utility_Op::WRAP: {
			const double range = c - b;
			if (utility_is_zero_approx(range)) {
				return b;
			}
			const double result = a - (range * std::floor((a - b) / range));
			return utility_is_equal_approx(result, c) ? b : result;
		}

		case Utility_Op::REMAP:
			return utility_lerp(d, e, utility_inverse_lerp(b, c, a));
		case Utility_Op::CUBIC_INTERPOLATE:
			// cubic_interpolate(from, to, pre, post, weight)
			return utility_cubic_interpolate(a, b, c, d, e);
		case Utility_Op::CUBIC_INTERPOLATE_ANGLE: {
			const UtilityCubicAngles rot = utility_cubic_angles(a, b, c, d);
			return utility_cubic_interpolate(rot.from, rot.to, rot.pre, rot.post, e);
		}
		// cubic_interpolate_in_time(from, to, pre, post, weight, to_t, pre_t, post_t)
		case Utility_Op::CUBIC_INTERPOLATE_IN_TIME:
			return utility_cubic_interpolate_in_time(a, b, c, d, e, f, g, h);
		case Utility_Op::CUBIC_INTERPOLATE_ANGLE_IN_TIME: {
			const UtilityCubicAngles rot = utility_cubic_angles(a, b, c, d);
			return utility_cubic_interpolate_in_time(rot.from, rot.to, rot.pre, rot.post, e, f, g, h);
		}
		case Utility_Op::BEZIER_INTERPOLATE: {
			const double omt = 1.0 - e;
			const double omt2 = omt * omt;
			const double omt3 = omt2 * omt;
			const double t2 = e * e;
			const double t3 = t2 * e;
			return a * omt3 + b * omt2 * e * 3.0 + c * omt * t2 * 3.0 + d * t3;
		}
		case Utility_Op::BEZIER_DERIVATIVE: {
			const double omt = 1.0 - e;
			const double omt2 = omt * omt;
			const double t2 = e * e;
			return (b - a) * 3.0 * omt2 + (c - b) * 6.0 * omt * e + (d - c) * 3.0 * t2;
		}

		// Not arithmetic on fa0-fa4: the Variant-shaped ops and the integer
		// random draws are performed by api_utility() before it gets here.
		case Utility_Op::STR:
		case Utility_Op::LEN:
		case Utility_Op::TO_INT:
		case Utility_Op::TO_FLOAT:
		case Utility_Op::TO_BOOL:
		case Utility_Op::HASH:
		case Utility_Op::VAR_TO_STR:
		case Utility_Op::STR_TO_VAR:
		case Utility_Op::VAR_TO_BYTES:
		case Utility_Op::BYTES_TO_VAR:
		case Utility_Op::TYPE_STRING:
		case Utility_Op::TYPE_CONVERT:
		case Utility_Op::ERROR_STRING:
		case Utility_Op::IS_SAME:
		case Utility_Op::RANDI:
		case Utility_Op::RANDI_RANGE:
		case Utility_Op::NEAREST_PO2:
		case Utility_Op::CHAR:
		case Utility_Op::ORD:
		case Utility_Op::IS_INSTANCE_VALID:
		case Utility_Op::RAND_FROM_SEED:
		case Utility_Op::RANDOMIZE:
		case Utility_Op::SEED:
			break;

		// The random draws that *are* doubles. UtilityFunctions:: rather than
		// a formula repeated here: these read the generator the rest of the
		// project draws from, so there is nothing to keep in step with the
		// compiler -- it cannot evaluate them at all.
		case Utility_Op::RANDF: return UtilityFunctions::randf();
		case Utility_Op::RANDF_RANGE: return UtilityFunctions::randf_range(a, b);
		case Utility_Op::RANDFN: return UtilityFunctions::randfn(a, b);
	}
	ERR_PRINT("Invalid utility operation");
	throw std::runtime_error("Invalid utility operation: " + std::to_string(int(op)));
}

// Godot's len(): the number of elements, for the Variants that have one.
static int64_t utility_len(const Variant &value) {
	const Variant::Type type = variant_type(value);
	switch (type) {
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			return value.operator String().length();
		case Variant::ARRAY:
			return value.operator Array().size();
		case Variant::DICTIONARY:
			return value.operator Dictionary().size();
		case Variant::PACKED_BYTE_ARRAY:
			return value.operator PackedByteArray().size();
		case Variant::PACKED_INT32_ARRAY:
			return value.operator PackedInt32Array().size();
		case Variant::PACKED_INT64_ARRAY:
			return value.operator PackedInt64Array().size();
		case Variant::PACKED_FLOAT32_ARRAY:
			return value.operator PackedFloat32Array().size();
		case Variant::PACKED_FLOAT64_ARRAY:
			return value.operator PackedFloat64Array().size();
		case Variant::PACKED_STRING_ARRAY:
			return value.operator PackedStringArray().size();
		case Variant::PACKED_VECTOR2_ARRAY:
			return value.operator PackedVector2Array().size();
		case Variant::PACKED_VECTOR3_ARRAY:
			return value.operator PackedVector3Array().size();
		case Variant::PACKED_COLOR_ARRAY:
			return value.operator PackedColorArray().size();
		case Variant::PACKED_VECTOR4_ARRAY:
			return value.operator PackedVector4Array().size();
		default:
			break;
	}
	ERR_PRINT("len(): Value of this type can't provide a length");
	throw std::runtime_error(std::string("len(): A ") + GuestVariant::type_name(type) +
			" can't provide a length");
}

// -= The type constructors =-
//
// int(x), float(x) and bool(x). A String parses the way Godot's String.to_int()
// and String.to_float() parse, because that is what those calls mean in
// GDScript; anything else that is not a number or a bool converts to zero
// rather than raising a call error, which is the same deviation the rest of
// the global functions make (see src/gdscript/compiler/globals.h). The
// guest performs all three inline when it already knows the argument is a
// number or a bool, so what arrives here is the case where it could be
// anything.
static int64_t utility_to_int(const Variant &value) {
	switch (variant_type(value)) {
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
			return value.operator int64_t();
		case Variant::STRING:
		case Variant::STRING_NAME:
			return value.operator String().to_int();
		default:
			return 0;
	}
}

static double utility_to_float(const Variant &value) {
	switch (variant_type(value)) {
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
			return value.operator double();
		case Variant::STRING:
		case Variant::STRING_NAME:
			return value.operator String().to_float();
		default:
			return 0.0;
	}
}

static inline String utility_stringify(const Sandbox &emu, const GuestVariant &gv) {
	return BorrowedVariant(emu, gv)->operator String();
}

APICALL(api_utility) {
	auto [op, vres_addr, args_addr, arg_count] = machine.sysargs<Utility_Op, gaddr_t, gaddr_t, unsigned>();
	SYS_TRACE("utility", int(op), vres_addr, args_addr, arg_count);

	switch (op) {
		// -= Variant in, Variant out =-
		//
		// a1 = where the answer goes, a2 = the arguments, a3 = how many.
		case Utility_Op::STR:
		case Utility_Op::LEN:
		case Utility_Op::TO_INT:
		case Utility_Op::TO_FLOAT:
		case Utility_Op::TO_BOOL:
		case Utility_Op::HASH:
		case Utility_Op::VAR_TO_STR:
		case Utility_Op::STR_TO_VAR:
		case Utility_Op::VAR_TO_BYTES:
		case Utility_Op::BYTES_TO_VAR:
		case Utility_Op::TYPE_STRING:
		case Utility_Op::TYPE_CONVERT:
		case Utility_Op::ERROR_STRING:
		case Utility_Op::IS_SAME:
		case Utility_Op::CHAR:
		case Utility_Op::ORD:
		case Utility_Op::RAND_FROM_SEED:
		case Utility_Op::RANDOMIZE:
		case Utility_Op::SEED:
		case Utility_Op::IS_INSTANCE_VALID: {
			Sandbox &emu = riscv::emu(machine);
			if (UNLIKELY((op == Utility_Op::RANDOMIZE || op == Utility_Op::SEED) &&
					!emu.is_fully_unrestricted())) {
				throw std::runtime_error("utility(): Shared RNG mutation is refused under restrictions");
			}
			// str() takes up to 63 arguments and String() takes none at all;
			// Binary: type_convert(), is_same(). Unary: the rest.
			const bool binary = (op == Utility_Op::TYPE_CONVERT || op == Utility_Op::IS_SAME);
			const bool no_args = (op == Utility_Op::RANDOMIZE);
			const unsigned max_args = (op == Utility_Op::STR) ? 63 : (no_args ? 0 : (binary ? 2 : 1));
			const unsigned min_args = (op == Utility_Op::STR || no_args) ? 0 : (binary ? 2 : 1);
			if (arg_count < min_args || arg_count > max_args) {
				ERR_PRINT("utility(): Wrong number of arguments");
				throw std::runtime_error("utility(): Wrong number of arguments: " + std::to_string(arg_count));
			}
			GuestVariant *vres = machine.memory.memarray<GuestVariant>(vres_addr, 1);
			const GuestVariant *args = arg_count
					? machine.memory.memarray<GuestVariant>(args_addr, arg_count)
					: nullptr;
			auto arg = [&](unsigned index) -> BorrowedVariant {
				return BorrowedVariant(emu, args[index]);
			};

			switch (op) {
				case Utility_Op::STR: {
					// Stringifying is host work the guest asked for, the same
					// as it is in print(), and there can be up to 63 arguments
					// in one call.
					PENALIZE(10'000 + 10'000 * arg_count);
					if (UNLIKELY(arg_count == 0)) {
						vres->create(emu, Variant(String()));
						break;
					}
					// Start from args[0] to avoid an empty-string realloc.
					String result = utility_stringify(emu, args[0]);
					for (unsigned i = 1; i < arg_count; i++) {
						result += utility_stringify(emu, args[i]);
					}
					vres->create(emu, Variant(std::move(result)));
					break;
				}
				case Utility_Op::LEN:
					PENALIZE(10'000);
					vres->create(emu, Variant(utility_len(*arg(0))));
					break;
				case Utility_Op::IS_INSTANCE_VALID: {
					PENALIZE(10'000);
					bool valid = false;
					if (args[0].type == Variant::OBJECT) {
						const uint64_t object_id = Sandbox::object_id_from_handle(uint64_t(args[0].v.i));
						godot::Object *obj = nullptr;
						if (object_id == 0) {
						} else if (emu.is_object_access_unrestricted()) {
							obj = emu.resolve_live_object(object_id);
						} else if (Sandbox::CurrentState::ScopedObject *so = emu.find_scoped_object(object_id)) {
							obj = Sandbox::resolve_scoped_object(*so);
						} else {
							obj = emu.get_explicitly_allowed_object(object_id);
						}
						valid = Sandbox::engine_object_id(obj) != 0;
					}
					vres->create(emu, Variant(valid));
					break;
				}
				case Utility_Op::TO_INT:
					PENALIZE(10'000);
					vres->create(emu, Variant(utility_to_int(*arg(0))));
					break;
				case Utility_Op::TO_FLOAT:
					PENALIZE(10'000);
					vres->create(emu, Variant(utility_to_float(*arg(0))));
					break;

				// Serialization and identity. Host-only: engine Variant encoding.
				case Utility_Op::HASH:
					PENALIZE(20'000);
					vres->create(emu, UtilityFunctions::hash(*arg(0)));
					break;
				case Utility_Op::VAR_TO_STR:
					PENALIZE(50'000);
					vres->create(emu, UtilityFunctions::var_to_str(*arg(0)));
					break;
				case Utility_Op::STR_TO_VAR:
					PENALIZE(50'000);
					vres->create(emu, UtilityFunctions::str_to_var(*arg(0)));
					break;
				case Utility_Op::VAR_TO_BYTES:
					PENALIZE(50'000);
					vres->create(emu, UtilityFunctions::var_to_bytes(*arg(0)));
					break;
				case Utility_Op::BYTES_TO_VAR:
					PENALIZE(50'000);
					vres->create(emu, UtilityFunctions::bytes_to_var(*arg(0)));
					break;
				case Utility_Op::TYPE_STRING:
					PENALIZE(10'000);
					vres->create(emu, UtilityFunctions::type_string(*arg(0)));
					break;
				case Utility_Op::TYPE_CONVERT:
					PENALIZE(20'000);
					vres->create(emu, UtilityFunctions::type_convert(
							*arg(0), *arg(1)));
					break;
				case Utility_Op::ERROR_STRING:
					PENALIZE(10'000);
					vres->create(emu, UtilityFunctions::error_string(*arg(0)));
					break;
				case Utility_Op::IS_SAME:
					PENALIZE(10'000);
					vres->create(emu, UtilityFunctions::is_same(
							*arg(0), *arg(1)));
					break;

				// char() and ord(). godot-cpp binds neither -- `char` is a C++
				// keyword -- so these are the String operations they are made of.
				case Utility_Op::CHAR:
					PENALIZE(10'000);
					vres->create(emu, Variant(String::chr(arg(0)->operator int64_t())));
					break;
				// The one seeded draw: no shared generator is read or written,
				// so a restricted program may call it.
				case Utility_Op::RAND_FROM_SEED:
					PENALIZE(20'000);
					vres->create(emu, UtilityFunctions::rand_from_seed(
							arg(0)->operator int64_t()));
					break;
				case Utility_Op::RANDOMIZE:
					PENALIZE(10'000);
					UtilityFunctions::randomize();
					vres->create(emu, Variant());
					break;
				case Utility_Op::SEED:
					PENALIZE(10'000);
					UtilityFunctions::seed(arg(0)->operator int64_t());
					vres->create(emu, Variant());
					break;

				case Utility_Op::ORD: {
					PENALIZE(10'000);
					// Godot refuses anything but a single character and answers 0.
					const String text = arg(0)->operator String();
					if (text.length() != 1) {
						ERR_PRINT("ord(): Expected a string of length 1 (a character)");
						vres->create(emu, Variant(int64_t(0)));
						break;
					}
					vres->create(emu, Variant(text.unicode_at(0)));
					break;
				}

				default:
					PENALIZE(10'000);
					vres->create(emu, Variant(arg(0)->booleanize()));
					break;
			}
			return;
		}

		// -= 64-bit integers in a1-a2, the answer in a0 =-
		//
		// randi() draws 32 bits, but randi_range()'s bounds are whatever the
		// program says they are, and a double would not carry them back.
		case Utility_Op::RANDI:
			machine.set_result(UtilityFunctions::randi());
			return;
		case Utility_Op::RANDI_RANGE: {
			const int64_t from = machine.sysarg<int64_t>(1); // a1
			const int64_t to = machine.sysarg<int64_t>(2); // a2
			machine.set_result(UtilityFunctions::randi_range(from, to));
			return;
		}
		case Utility_Op::NEAREST_PO2:
			machine.set_result(UtilityFunctions::nearest_po2(machine.sysarg<int64_t>(1)));
			return;

		default:
			break;
	}

	// Everything else is arithmetic on doubles in fa0-fa7, answering in fa0.
	double args[8];
	for (int i = 0; i < 8; i++) {
		args[i] = machine.cpu.registers().getfl(10 + i).get<double>(); // fa0-fa7
	}
	machine.set_result(utility_math_op(op, args));
}

static void vcall_impl(machine_t &machine, bool super) {
	auto [vp, method, mlen, args_ptr, args_size, vret_addr] = machine.sysargs<GuestVariant *, gaddr_t, unsigned, gaddr_t, gaddr_t, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	SYS_TRACE("vcall", method, mlen, args_ptr, args_size, vret_addr);

	if (UNLIKELY(args_size > gdscript::CallABI::MAX_ARGUMENTS)) {
		ERR_PRINT("Variant::call(): Too many arguments");
		throw std::runtime_error("Variant::call(): Too many arguments");
	}

	// Zero-argument calls are the common case, and have nothing to translate.
	const GuestVariant *args = args_size ? machine.memory.memarray<GuestVariant>(args_ptr, args_size) : nullptr;
	const std::string_view method_sv = memview_with_terminator(machine, method, mlen).substr(0, size_t(mlen) + 1); // Include null terminator.
	// Reuse the name built for this call site. Only the form the branch below actually
	// needs is copied out: the two forms are not interchangeable without Godot building
	// one from the other, which is the cost the cache exists to avoid.
	Sandbox::CachedNameRef cached_method = emu.cached_guest_name(method, method_sv.substr(0, mlen), method_sv.back() == '\0');

	// Both call paths have Godot construct the return value directly in this storage.
	CallResult result;
	const std::string_view method_name = method_sv.substr(0, mlen);

	if (vp->type == Variant::OBJECT) {
		godot::Object *obj = get_object_from_address(emu, vp->v.i);
		object_call_checked(emu, obj, cached_method, method_name, args, args_size, result, super);
	} else if (vp->is_scoped_variant()) {
		Variant *vcall = const_cast<Variant *>(vp->toVariantPtr(emu));
		// Godot's read-only enforcement only prints and no-ops; throw so the guest unwinds.
		// Keyed on the resolved Variant: vp->type is the guest's, and every scoped type
		// reaches this one branch.
		const Variant::Type vtype = variant_type(*vcall);
		if (UNLIKELY(vtype == Variant::ARRAY || vtype == Variant::DICTIONARY)) {
			if (is_container_mutator(method_name)) {
				throw_if_read_only(*vcall, "Variant::call");
			}
		}
		variant_or_object_call(emu, vcall, cached_method, method_name, args, args_size, result);
	} else {
		BorrowedVariant vcall(emu, *vp);
		variant_or_object_call(emu, const_cast<Variant *>(&*vcall), cached_method,
				method_name, args, args_size, result);
	}
	// Create a new Variant with the result, if any.
	if (vret_addr != 0) {
		GuestVariant *vret = machine.memory.memarray<GuestVariant>(vret_addr, 1);
		vret->create(emu, std::move(result.get()));
	}
}

APICALL(api_vcall) {
	vcall_impl(machine, false);
}

APICALL(api_vcall_super) {
	vcall_impl(machine, true);
}

APICALL(api_class_bind) {
	auto [dict_idx, name] = machine.sysargs<int32_t, std::string>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(150'000);
	SYS_TRACE("class_bind", dict_idx, String::utf8(name.c_str(), name.size()));

	// The compile that produces this syscall already refuses an engine base
	// under restrictions; this is the same gate seen from the other side, for a
	// guest program that was not built by that compiler.
	if (UNLIKELY(emu.is_class_access_restricted())) {
		ERR_PRINT("Sandbox: a restricted program cannot attach a script to an engine object.");
		throw std::runtime_error("class_bind: refused while restricted");
	}

	const Variant &instance = get_scoped_variant_or_throw(emu, dict_idx, "class_bind");
	godot::Object *base = class_instance_base(emu, instance);
	if (UNLIKELY(base == nullptr)) {
		ERR_PRINT("Sandbox: class_bind was given something that is not a class instance.");
		throw std::runtime_error("class_bind: no engine object under '@base'");
	}
	safegdscript_bind_nested_class(emu, base, Dictionary(instance),
			String::utf8(name.c_str(), int64_t(name.size())));
}

APICALL(api_obj_uses_trait) {
	auto [handle, trait_utf8, methods] =
		machine.sysargs<uint64_t, std::string_view, std::string_view>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(50'000);
	godot::Object *object = get_object_from_address(emu, handle);
	const StringName trait_name(String::utf8(trait_utf8.data(),
		int64_t(trait_utf8.size())));
	bool recognized = false;
	const bool nominal = safegdscript_nominal_uses(object, trait_name, recognized);
	if (recognized) {
		machine.set_result(nominal ? 1 : 0);
		return;
	}
	if (methods.empty()) {
		machine.set_result(0);
		return;
	}

	size_t begin = 0;
	while (begin < methods.size()) {
		size_t end = methods.find('\0', begin);
		if (end == std::string::npos) end = methods.size();
		if (end > begin) {
			const StringName method(String::utf8(methods.data() + begin,
				int64_t(end - begin)));
			if (!object->has_method(method)) {
				machine.set_result(0);
				return;
			}
		}
		begin = end + 1;
	}
	machine.set_result(1);
}

APICALL(api_veval) {
	auto [op, ap, bp, retp] = machine.sysargs<int, GuestVariant *, GuestVariant *, GuestVariant *>();
	auto &emu = riscv::emu(machine);
	SYS_TRACE("veval", op, ap, bp, retp);

	// Godot indexes its operator table with this directly, so an out-of-range operator
	// is an out-of-bounds read on the host. The guest picks the number, so check it here.
	if (UNLIKELY(unsigned(op) >= Variant::OP_MAX)) {
		ERR_PRINT("Variant::evaluate(): Invalid operator: " + itos(op));
		throw std::runtime_error("Variant::evaluate(): Invalid operator: " + std::to_string(op));
	}

	// Special case for comparing objects.
	if (ap->type == Variant::OBJECT && bp->type == Variant::OBJECT) {
		// Special case for equality, allowing invalid objects to be compared.
		if (op == static_cast<int>(Variant::Operator::OP_EQUAL)) {
			machine.set_result(false);
			retp->set(emu, Variant(ap->v.i == bp->v.i));
			return;
		}
		godot::Object *a = get_object_from_address(emu, ap->v.i);
		godot::Object *b = get_object_from_address(emu, bp->v.i);
		bool valid = false;
		Variant ret;
		Variant::evaluate(static_cast<Variant::Operator>(op), a, b, ret, valid);

		machine.set_result(valid);
		retp->set(emu, ret, false);
		return;
	}

	const BorrowedVariant a(emu, *ap);
	const BorrowedVariant b(emu, *bp);

	// Shifting two integers. Variant::evaluate() refuses a negative operand,
	// but it is not the path GDScript runs: with the types known the engine
	// takes the validated evaluator, a native int64 shift with the count
	// masked to six bits, and `-16 >> 2` is -4 there. Match that, which is
	// also what the compiler emits when it knows both operands are integers.
	if ((op == Variant::OP_SHIFT_LEFT || op == Variant::OP_SHIFT_RIGHT) &&
			ap->type == Variant::INT && bp->type == Variant::INT) {
		const int64_t lhs = a->operator int64_t();
		const int shift = int(b->operator int64_t() & 63);
		const int64_t answer = (op == Variant::OP_SHIFT_LEFT)
				? int64_t(uint64_t(lhs) << shift)
				: (lhs >> shift);
		machine.set_result(true);
		retp->set(emu, Variant(answer));
		return;
	}

	// Uninitialized storage; Godot placement-constructs the result.
	CallResult result;
	GDExtensionBool valid = false;
	internal::gdextension_interface_variant_evaluate(static_cast<GDExtensionVariantOperator>(op),
			a->_native_ptr(), b->_native_ptr(), &result.get(), &valid);
	result.mark_constructed();

	machine.set_result(bool(valid));
	if (UNLIKELY(!valid) && (op == Variant::OP_EQUAL || op == Variant::OP_NOT_EQUAL)) {
		// No evaluator → NIL result; fused branches read NIL as false/equal.
		// Different types are never equal (matches engine Variant comparison).
		retp->set(emu, Variant(op == Variant::OP_NOT_EQUAL));
		return;
	}
	retp->create(emu, std::move(result.get()));
}

APICALL(api_vconstruct) {
	auto [vp, type, gargs, argc] = machine.sysargs<GuestVariant *, int32_t, gaddr_t, int32_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(20'000);
	SYS_TRACE("vconstruct", vp, type, gargs, argc);

	if (UNLIKELY(type < 0 || type >= Variant::VARIANT_MAX)) {
		ERR_PRINT("vconstruct: Invalid Variant type " + itos(type));
		throw std::runtime_error("vconstruct: Invalid Variant type " + std::to_string(type));
	}
	if (UNLIKELY(argc < 0 || unsigned(argc) > VariantScratch::MAX)) {
		ERR_PRINT("vconstruct: Invalid argument count " + itos(argc));
		throw std::runtime_error("vconstruct: Invalid argument count " + std::to_string(argc));
	}
	// Callable(Object, method) is unsafe and implies unrestricted access to the wider engine
	if (UNLIKELY(type == Variant::CALLABLE && argc == 2 && !emu.is_fully_unrestricted())) {
		ERR_PRINT("Callable(Object, method) is only available to a fully unrestricted Sandbox");
		throw std::runtime_error(
			"vconstruct: Callable(Object, method) refused under restrictions");
	}

	BorrowedVariantScratch scratch;
	const Variant *argptrs[VariantScratch::MAX];
	if (argc > 0) {
		const GuestVariant *args = machine.memory.memarray<const GuestVariant>(gargs, argc);
		for (int i = 0; i < argc; i++) {
			argptrs[i] = scratch.emplace(emu, args[i]);
		}
	}

	CallResult result;
	GDExtensionCallError error;
	internal::gdextension_interface_variant_construct(static_cast<GDExtensionVariantType>(type),
			&result.get(), reinterpret_cast<GDExtensionConstVariantPtr *>(argptrs), argc, &error);
	result.mark_constructed();
	if (UNLIKELY(error.error != GDEXTENSION_CALL_OK)) {
		const String type_name = Variant::get_type_name(Variant::Type(type));
		String message;
		if (error.error == GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT &&
			error.argument >= 0 && error.argument < argc) {
			message = type_name + String("(): argument ") + itos(error.argument + 1) +
					" is " + String::utf8(GuestVariant::type_name(argptrs[error.argument]->get_type())) +
					", not " + Variant::get_type_name(Variant::Type(error.expected));
		} else {
			message = type_name + String("(): no constructor takes ") + itos(argc) + " argument(s)";
		}
		ERR_PRINT(message);
		throw std::runtime_error(std::string(message.utf8().get_data()));
	}

	vp->create(emu, std::move(result.get()));
}

/// @brief Construct a Variant from data the guest describes.
/// @note The Packed*Array branches take their element count from the guest, either as
/// the method argument or out of a guest-side std::vector header. That count is only
/// checked against guest memory by memarray()/as_array(), so the view is always taken
/// before the Godot array is resized: resizing first would let the guest name a length
/// it has no memory to back, and Godot treats an allocation failure as fatal. The copy
/// is then sized from the same count, never from size_bytes(), which need not be a
/// whole number of elements.
APICALL(api_vcreate) {
	auto [vp, type, method, gdata] = machine.sysargs<GuestVariant *, int32_t, int, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("vcreate", vp, type, method, gdata);

	switch (type) {
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH: { // From std::string
			String godot_str;
			if (method == 0) {
				const CppString *str = machine.memory.memarray<const CppString>(gdata, 1);
				godot_str = to_godot_string(str, machine);
			} else if (method == 1) { // const char*, size_t
				const struct Buffer {
					gaddr_t data;
					gaddr_t size;
				} *buffer = machine.memory.memarray<const Buffer>(gdata, 1);
				// View the string from guest memory.
				std::string_view view = machine.memory.memview(buffer->data, buffer->size);
				godot_str = String::utf8(view.data(), view.size());
			} else if (method == 2) { // From std::u32string
				const GuestStdU32String *str = machine.memory.memarray<const GuestStdU32String>(gdata, 1);
				godot_str = str->to_godot_string(machine);
			} else if (method == 3) { // const int32_t*, size_t
				const struct Buffer {
					gaddr_t data;
					gaddr_t size;
				} *buffer = machine.memory.memarray<const Buffer>(gdata, 1);
				// View the string from guest memory.
				const int32_t* chars = machine.memory.memarray<const int32_t>(buffer->data, buffer->size);
				std::u32string str(chars, chars + buffer->size);
				godot_str = String(str.data());
			} else {
				ERR_PRINT("vcreate: Unsupported method for Variant::STRING");
				throw std::runtime_error("vcreate: Unsupported method for Variant::STRING: " + std::to_string(method));
			}
			// Scope as the requested type; a String scoped as STRING_NAME
			// would read back as String and lose the distinction.
			Variant value;
			if (type == Variant::STRING_NAME) {
				value = StringName(godot_str);
			} else if (type == Variant::NODE_PATH) {
				value = NodePath(godot_str);
			} else {
				value = std::move(godot_str);
			}
			unsigned idx = emu.create_scoped_variant(std::move(value));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::ARRAY: {
			// Create a new empty? array, assign to vp.
			Array a;
			if (gdata != 0x0) {
				if (method == -1) {
					// Copy std::vector<Variant> from guest memory.
					const CppVector<GuestVariant> *vec = machine.memory.memarray<const CppVector<GuestVariant>>(gdata, 1);
					for (size_t i = 0; i < vec->size(); i++) {
						const GuestVariant &v = vec->at(machine, i);
						a.push_back(std::move(v.toVariant(emu)));
					}
				} else if (method >= 0) {
					// Get elements from method argument.
					const unsigned size = method;
					// Copy array of Variants from guest memory.
					const GuestVariant *gvec = machine.memory.memarray<const GuestVariant>(gdata, size);
					for (int i = 0; i < size; i++) {
						a.push_back(gvec[i].toVariant(emu));
					}
				} else {
					// In order to support future methods, we shouldn't throw an error here.
					WARN_PRINT("vcreate: Unsupported method for Variant::ARRAY: " + itos(method));
				}
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::DICTIONARY: {
			// Create a new empty? dictionary, assign to vp.
			Dictionary d;
			const unsigned size = method;
			if (gdata == 0x0 && size > 0) {
				ERR_PRINT("vcreate: gdata is null for non-empty Dictionary");
				throw std::runtime_error("vcreate: gdata is null for non-empty Dictionary");
			} else if (size > 1024) {
				ERR_PRINT("vcreate: Dictionary size too large: " + itos(size));
				throw std::runtime_error("vcreate: Dictionary size too large: " + std::to_string(size));
			} else if (size % 2 != 0) {
				ERR_PRINT("vcreate: Dictionary size is not a multiple of 2: " + itos(size));
				throw std::runtime_error("vcreate: Dictionary size is not a multiple of 2: " + std::to_string(size));
			}
			const GuestVariant *gdata_ptr = machine.memory.memarray<const GuestVariant>(gdata, size);
			for (unsigned i = 0; i < size; i += 2) {
				const GuestVariant &gkey = gdata_ptr[i];
				const GuestVariant &gval = gdata_ptr[i + 1];
				d.set(gkey.toVariant(emu), gval.toVariant(emu));
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(d)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::PACKED_BYTE_ARRAY: {
			PackedByteArray a;
			if (gdata != 0x0) {
				if (method < 0) {
					// Copy std::vector<uint8_t> from guest memory.
					const CppVector<uint8_t> *gvec = machine.memory.memarray<const CppVector<uint8_t>>(gdata, 1);
					// View before resize; see the note at the top of api_vcreate().
					const uint8_t *elements = gvec->as_array(machine);
					const size_t count = gvec->size();
					a.resize(count);
					guest_memcpy(a.ptrw(), elements, count * sizeof(uint8_t));
				} else {
					// Method is the buffer length. View before resize; see api_vcreate().
					const uint8_t *ptr = machine.memory.memarray<const uint8_t>(gdata, method);
					a.resize(method);
					guest_memcpy(a.ptrw(), ptr, method);
				}
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::PACKED_FLOAT32_ARRAY: {
			PackedFloat32Array a;
			if (gdata != 0x0) {
				if (method < 0) {
					// Copy std::vector<float> from guest memory.
					const CppVector<float> *gvec = machine.memory.memarray<const CppVector<float>>(gdata, 1);
					// View before resize; see the note at the top of api_vcreate().
					const float *elements = gvec->as_array(machine);
					const size_t count = gvec->size();
					a.resize(count);
					guest_memcpy(a.ptrw(), elements, count * sizeof(float));
				} else {
					// Method is the buffer length. View before resize; see api_vcreate().
					const float *ptr = machine.memory.memarray<const float>(gdata, method);
					a.resize(method);
					guest_memcpy(a.ptrw(), ptr, method * sizeof(float));
				}
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::PACKED_FLOAT64_ARRAY: {
			PackedFloat64Array a;
			if (gdata != 0x0) {
				if (method < 0) {
					// Copy std::vector<double> from guest memory.
					const CppVector<double> *gvec = machine.memory.memarray<const CppVector<double>>(gdata, 1);
					// View before resize; see the note at the top of api_vcreate().
					const double *elements = gvec->as_array(machine);
					const size_t count = gvec->size();
					a.resize(count);
					guest_memcpy(a.ptrw(), elements, count * sizeof(double));
				} else {
					// Method is the buffer length. View before resize; see api_vcreate().
					const double *ptr = machine.memory.memarray<const double>(gdata, method);
					a.resize(method);
					guest_memcpy(a.ptrw(), ptr, method * sizeof(double));
				}
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::PACKED_INT32_ARRAY: {
			PackedInt32Array a;
			if (gdata != 0x0) {
				if (method < 0) {
					// Copy std::vector<int32_t> from guest memory.
					const CppVector<int32_t> *gvec = machine.memory.memarray<const CppVector<int32_t>>(gdata, 1);
					// View before resize; see the note at the top of api_vcreate().
					const int32_t *elements = gvec->as_array(machine);
					const size_t count = gvec->size();
					a.resize(count);
					guest_memcpy(a.ptrw(), elements, count * sizeof(int32_t));
				} else {
					// Method is the buffer length. View before resize; see api_vcreate().
					const int32_t *ptr = machine.memory.memarray<const int32_t>(gdata, method);
					a.resize(method);
					guest_memcpy(a.ptrw(), ptr, method * sizeof(int32_t));
				}
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::PACKED_INT64_ARRAY: {
			PackedInt64Array a;
			if (gdata != 0x0) {
				if (method < 0) {
					// Copy std::vector<int64_t> from guest memory.
					const CppVector<int64_t> *gvec = machine.memory.memarray<const CppVector<int64_t>>(gdata, 1);
					// View before resize; see the note at the top of api_vcreate().
					const int64_t *elements = gvec->as_array(machine);
					const size_t count = gvec->size();
					a.resize(count);
					guest_memcpy(a.ptrw(), elements, count * sizeof(int64_t));
				} else {
					// Method is the buffer length. View before resize; see api_vcreate().
					const int64_t *ptr = machine.memory.memarray<const int64_t>(gdata, method);
					a.resize(method);
					guest_memcpy(a.ptrw(), ptr, method * sizeof(int64_t));
				}
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::PACKED_VECTOR2_ARRAY: {
			PackedVector2Array a;
			if (gdata != 0x0) {
				if (method < 0) {
					// Copy std::vector<Vector2> from guest memory.
					const CppVector<Vector2> *gvec = machine.memory.memarray<const CppVector<Vector2>>(gdata, 1);
					// View before resize; see the note at the top of api_vcreate().
					const Vector2 *elements = gvec->as_array(machine);
					const size_t count = gvec->size();
					a.resize(count);
					guest_memcpy(a.ptrw(), elements, count * sizeof(Vector2));
				} else {
					// Method is the buffer length. View before resize; see api_vcreate().
					const Vector2 *ptr = machine.memory.memarray<const Vector2>(gdata, method);
					a.resize(method);
					guest_memcpy(a.ptrw(), ptr, method * sizeof(Vector2));
				}
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::PACKED_VECTOR3_ARRAY: {
			PackedVector3Array a;
			if (gdata != 0x0) {
				if (method < 0) {
					// Copy std::vector<Vector3> from guest memory.
					const CppVector<Vector3> *gvec = machine.memory.memarray<const CppVector<Vector3>>(gdata, 1);
					// View before resize; see the note at the top of api_vcreate().
					const Vector3 *elements = gvec->as_array(machine);
					const size_t count = gvec->size();
					a.resize(count);
					guest_memcpy(a.ptrw(), elements, count * sizeof(Vector3));
				} else {
					// Method is the buffer length. View before resize; see api_vcreate().
					const Vector3 *ptr = machine.memory.memarray<const Vector3>(gdata, method);
					a.resize(method);
					guest_memcpy(a.ptrw(), ptr, method * sizeof(Vector3));
				}
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::PACKED_VECTOR4_ARRAY: {
			PackedVector4Array a;
			if (gdata != 0x0) {
				if (method < 0) {
					// Copy std::vector<Vector4> from guest memory.
					const CppVector<Vector4> *gvec = machine.memory.memarray<const CppVector<Vector4>>(gdata, 1);
					// View before resize; see the note at the top of api_vcreate().
					const Vector4 *elements = gvec->as_array(machine);
					const size_t count = gvec->size();
					a.resize(count);
					guest_memcpy(a.ptrw(), elements, count * sizeof(Vector4));
				} else {
					// Method is the buffer length. View before resize; see api_vcreate().
					const Vector4 *ptr = machine.memory.memarray<const Vector4>(gdata, method);
					a.resize(method);
					guest_memcpy(a.ptrw(), ptr, method * sizeof(Vector4));
				}
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::PACKED_COLOR_ARRAY: {
			PackedColorArray a;
			if (gdata != 0x0) {
				// Copy std::vector<Color> from guest memory.
				const CppVector<Color> *gvec = machine.memory.memarray<const CppVector<Color>>(gdata, 1);
				// View before resize; see the note at the top of api_vcreate().
				const Color *elements = gvec->as_array(machine);
				const size_t count = gvec->size();
				a.resize(count);
				guest_memcpy(a.ptrw(), elements, count * sizeof(Color));
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		case Variant::PACKED_STRING_ARRAY: {
			PackedStringArray a;
			if (gdata != 0x0) {
				// Copy std::vector<String> from guest memory.
				if (method == -1) {
					const CppVector<CppString> *gvec = machine.memory.memarray<const CppVector<CppString>>(gdata, 1);
					const CppString *str_array = gvec->as_array(machine);
					for (size_t i = 0; i < gvec->size(); i++) {
						a.push_back(to_godot_string(&str_array[i], machine));
					}
				} else if (method == -2) {
					// libc++ std::string implementation.
					struct Buffer {
						gaddr_t ptr;
						gaddr_t size;
					};
					const CppVector<Buffer> *gvec = machine.memory.memarray<const CppVector<Buffer>>(gdata, 1);
					const Buffer *buffers = gvec->as_array(machine);
					for (size_t i = 0; i < gvec->size(); i++) {
						const Buffer &buf = buffers[i];
						std::string_view view = machine.memory.memview(buf.ptr, buf.size);
						a.push_back(String::utf8(view.data(), view.size()));
					}
				} else {
					ERR_PRINT("vcreate: Unsupported method for Variant::PACKED_STRING_ARRAY");
					throw std::runtime_error("vcreate: Unsupported method for Variant::PACKED_STRING_ARRAY: " + std::to_string(method));
				}
			}
			unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
			vp->type = type;
			vp->v.i = idx;
		} break;
		default:
			ERR_PRINT("Unsupported Variant type for Variant::create()");
			throw std::runtime_error("Unsupported Variant type for Variant::create(): " + std::string(GuestVariant::type_name(type)));
	}
}

APICALL(api_vfetch) {
	auto [index, gdata, method] = machine.sysargs<unsigned, gaddr_t, int>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("vfetch", index, gdata, method);

	// Find scoped Variant and copy data into gdata.
	const godot::Variant &var = get_scoped_variant_or_throw(emu, index, "Variant::fetch");
	switch (var.get_type()) {
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH: {
			if (method == 0) { // std::string
				auto u8str = var.operator String().utf8();
				CppString *gstr = machine.memory.memarray<CppString>(gdata, 1);
				gstr->set_string(machine, gdata, u8str.ptr(), u8str.length());
			} else if (method == 1) { // const char*, size_t struct
				auto u8str = var.operator String().utf8();
				struct Buffer {
					gaddr_t ptr;
					gaddr_t size;
				} *gstr = machine.memory.memarray<Buffer>(gdata, 1);
				gstr->ptr  = machine.arena().malloc(u8str.length());
				gstr->size = u8str.length();
				machine.memory.memcpy(gstr->ptr, u8str.ptr(), u8str.length());
			} else if (method == 2) { // std::u32string
				auto u32str = var.operator String();
				auto *gstr = machine.memory.memarray<GuestStdU32String>(gdata, 1);
				gstr->set_string(machine, gdata, u32str.ptr(), u32str.length());
			} else {
				ERR_PRINT("vfetch: Unsupported method for Variant::STRING");
				throw std::runtime_error("vfetch: Unsupported method for Variant::STRING");
			}
			break;
		}
		case Variant::PACKED_BYTE_ARRAY: {
			CppVector<uint8_t> *gvec = machine.memory.memarray<CppVector<uint8_t>>(gdata, 1);
			auto arr = var.operator PackedByteArray();
			gvec->assign(machine, arr.ptr(), arr.size());
			break;
		}
		case Variant::PACKED_FLOAT32_ARRAY: {
			CppVector<float> *gvec = machine.memory.memarray<CppVector<float>>(gdata, 1);
			auto arr = var.operator PackedFloat32Array();
			gvec->assign(machine, arr.ptr(), arr.size());
			break;
		}
		case Variant::PACKED_FLOAT64_ARRAY: {
			CppVector<double> *gvec = machine.memory.memarray<CppVector<double>>(gdata, 1);
			auto arr = var.operator PackedFloat64Array();
			gvec->assign(machine, arr.ptr(), arr.size());
			break;
		}
		case Variant::PACKED_INT32_ARRAY: {
			CppVector<int32_t> *gvec = machine.memory.memarray<CppVector<int32_t>>(gdata, 1);
			auto arr = var.operator PackedInt32Array();
			gvec->assign(machine, arr.ptr(), arr.size());
			break;
		}
		case Variant::PACKED_INT64_ARRAY: {
			CppVector<int64_t> *gvec = machine.memory.memarray<CppVector<int64_t>>(gdata, 1);
			auto arr = var.operator PackedInt64Array();
			gvec->assign(machine, arr.ptr(), arr.size());
			break;
		}
		case Variant::PACKED_VECTOR2_ARRAY: {
			CppVector<Vector2> *gvec = machine.memory.memarray<CppVector<Vector2>>(gdata, 1);
			auto arr = var.operator PackedVector2Array();
			gvec->assign(machine, arr.ptr(), arr.size());
			break;
		}
		case Variant::PACKED_VECTOR3_ARRAY: {
			CppVector<Vector3> *gvec = machine.memory.memarray<CppVector<Vector3>>(gdata, 1);
			auto arr = var.operator PackedVector3Array();
			gvec->assign(machine, arr.ptr(), arr.size());
			break;
		}
		case Variant::PACKED_VECTOR4_ARRAY: {
			CppVector<Vector4> *gvec = machine.memory.memarray<CppVector<Vector4>>(gdata, 1);
			auto arr = var.operator PackedVector4Array();
			gvec->assign(machine, arr.ptr(), arr.size());
			break;
		}
		case Variant::PACKED_COLOR_ARRAY: {
			CppVector<Color> *gvec = machine.memory.memarray<CppVector<Color>>(gdata, 1);
			auto arr = var.operator PackedColorArray();
			gvec->assign(machine, arr.ptr(), arr.size());
			break;
		}
		case Variant::PACKED_STRING_ARRAY: {
			auto arr = var.operator PackedStringArray();
			if (method == 0) {
				CppVector<CppString> *gvec = machine.memory.memarray<CppVector<CppString>>(gdata, 1);
				gvec->resize(machine, arr.size());
				for (unsigned i = 0; i < arr.size(); i++) {
					auto u8str = arr[i].utf8();
					const gaddr_t self = gvec->address_at(i);
					gvec->at(machine, i).set_string(machine, self, std::string_view(u8str.ptr(), u8str.length()));
				}
			} else if (method == 1) {
				// libc++ std::string implementation.
				struct Buffer {
					gaddr_t ptr;
					gaddr_t size;
				};
				CppVector<Buffer> *gvec = machine.memory.memarray<CppVector<Buffer>>(gdata, 1);
				gvec->reserve(machine, arr.size());
				for (unsigned i = 0; i < arr.size(); i++) {
					auto u8str = arr[i].utf8();
					Buffer gb;
					gb.ptr  = machine.arena().malloc(u8str.length());
					gb.size = u8str.length();
					machine.memory.memcpy(gb.ptr, u8str.ptr(), u8str.length());
					gvec->push_back(machine, gb);
				}
			} else {
				ERR_PRINT("vfetch: Unsupported method for Variant::PACKED_STRING_ARRAY");
				throw std::runtime_error("vfetch: Unsupported method for Variant::PACKED_STRING_ARRAY");
			}
			break;
		}
		default:
			ERR_PRINT("vfetch: Cannot fetch value into guest for Variant type " + String(GuestVariant::type_name(var.get_type())));
			throw std::runtime_error("vfetch: Cannot fetch value into guest for Variant type " + std::string(GuestVariant::type_name(var.get_type())));
	}
}

APICALL(api_vclone) {
	auto [vp, vret_addr] = machine.sysargs<GuestVariant *, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("vclone", vp, vret);

	if (vret_addr != 0) {
		// Find scoped Variant and clone it.
		const Variant &var = get_scoped_variant_or_throw(emu, vp->v.i, "Variant::clone");
		const unsigned index = emu.create_scoped_variant(var.duplicate());
		// Duplicate the Variant and store the index in the guest memory.
		GuestVariant *vret = machine.memory.memarray<GuestVariant>(vret_addr, 1);
		vret->type = var.get_type();
		vret->v.i = index;
	} else {
		// Duplicate or move the Variant into permanent storage (m_level[0]).
		const unsigned idx = vp->v.i;
		unsigned new_idx = emu.create_permanent_variant(idx);
		// Update the Variant with the new index.
		vp->v.i = new_idx;
	}
}

struct ScopeRescue {
	struct Entry {
		const godot::Variant *addr;
		godot::Variant value;
		bool dying;
		int32_t reindexed;
	};
	std::vector<Entry> entries;
	std::vector<std::pair<GuestVariant *, uint32_t>> handle_fixups;
	std::vector<std::pair<uint32_t, uint32_t>> slot_fixups;

	void clear() {
		entries.clear();
		handle_fixups.clear();
		slot_fixups.clear();
	}
	uint32_t entry_for(const godot::Variant *addr, bool dying) {
		for (size_t i = 0; i < entries.size(); i++) {
			if (entries[i].addr == addr) {
				return uint32_t(i);
			}
		}
		Entry &e = entries.emplace_back();
		e.addr = addr;
		e.dying = dying;
		e.reindexed = -1;
		if (dying) {
			e.value = std::move(const_cast<godot::Variant &>(*addr));
		}
		return uint32_t(entries.size() - 1);
	}
};

static inline int32_t scope_owned_index(const Sandbox::CurrentState &st, const godot::Variant *ptr) {
	const godot::Variant *begin = st.variants.data();
	if (ptr >= begin && ptr < begin + st.variants.size()) {
		return int32_t(ptr - begin);
	}
	return -1;
}

static void scope_rescue_range(machine_t &machine, Sandbox &emu, Sandbox::CurrentState &st,
		gaddr_t base, gaddr_t size, uint32_t scoped_mark, uint32_t variant_mark, ScopeRescue &rescue) {
	if (base == 0 || size < sizeof(GuestVariant)) {
		return;
	}
	const size_t count = size / sizeof(GuestVariant);
	GuestVariant *slots = machine.memory.memarray<GuestVariant>(base, count);
	for (size_t i = 0; i < count; i++) {
		GuestVariant *gv = &slots[i];
		if (!gv->is_scoped_variant()) {
			continue;
		}
		const int32_t idx = int32_t(gv->v.i);
		if (Sandbox::is_permanent_variant(idx) || idx < int32_t(scoped_mark)) {
			continue;
		}
		std::optional<const godot::Variant *> var = emu.get_scoped_variant(idx);
		if (!var.has_value()) {
			gv->type = godot::Variant::NIL;
			gv->v.i = 0;
			continue;
		}
		const godot::Variant *addr = var.value();
		const int32_t owned = scope_owned_index(st, addr);
		const bool dying = owned >= 0 && uint32_t(owned) >= variant_mark;
		rescue.handle_fixups.emplace_back(gv, rescue.entry_for(addr, dying));
	}
}

APICALL(api_vscope) {
	auto [op, mark, frame_base, frame_size, globals_base, globals_size, members_size] =
			machine.sysargs<int, uint64_t, gaddr_t, gaddr_t, gaddr_t, gaddr_t, gaddr_t>();
	auto &emu = riscv::emu(machine);
	SYS_TRACE("vscope", op, mark, frame_base, frame_size);

	Sandbox::CurrentState &st = emu.state();

	static constexpr uint64_t SCOPE_MARK_TAG = uint64_t(1) << 63;

	if (Scope_Op(op) == Scope_Op::MARK) {
		machine.set_result(SCOPE_MARK_TAG | (uint64_t(st.scoped_variants.size()) << 32) |
				uint64_t(uint32_t(st.variants.size())));
		return;
	}
	if (Scope_Op(op) != Scope_Op::RELEASE) {
		ERR_PRINT("Unknown scope operation");
		throw std::runtime_error("Unknown scope operation");
	}
	if (!emu.is_in_vmcall()) {
		return;
	}
	if ((mark & SCOPE_MARK_TAG) == 0) {
		return;
	}
	const uint32_t scoped_mark = uint32_t((mark & ~SCOPE_MARK_TAG) >> 32);
	const uint32_t variant_mark = uint32_t(mark);
	if (scoped_mark > st.scoped_variants.size() || variant_mark > st.variants.size()) {
		return;
	}
	if (scoped_mark == st.scoped_variants.size() && variant_mark == st.variants.size()) {
		return;
	}
	PENALIZE(500);

	static thread_local ScopeRescue rescue;
	rescue.clear();
	scope_rescue_range(machine, emu, st, frame_base, frame_size, scoped_mark, variant_mark, rescue);
	scope_rescue_range(machine, emu, st, globals_base, globals_size, scoped_mark, variant_mark, rescue);
	const gaddr_t members_base = gaddr_t(machine.cpu.reg(riscv::REG_TP));
	scope_rescue_range(machine, emu, st, members_base, members_size, scoped_mark, variant_mark, rescue);

	// Slots below the mark can point above it after get_mutable_scoped_variant().
	for (uint32_t slot = 0; slot < scoped_mark; slot++) {
		const godot::Variant *addr = st.scoped_variants[slot];
		const int32_t owned = scope_owned_index(st, addr);
		if (owned < 0 || uint32_t(owned) < variant_mark) {
			continue;
		}
		rescue.slot_fixups.emplace_back(slot, rescue.entry_for(addr, true));
	}

	st.scoped_variants.resize(scoped_mark);
	st.variants.resize(variant_mark);

	for (auto &e : rescue.entries) {
		if (e.dying) {
			st.variants.push_back(std::move(e.value));
			e.addr = &st.variants.back();
		}
	}
	for (auto &[gv, index] : rescue.handle_fixups) {
		ScopeRescue::Entry &e = rescue.entries[index];
		if (e.reindexed < 0) {
			e.reindexed = int32_t(emu.add_scoped_variant(e.addr));
		}
		gv->v.i = e.reindexed;
	}
	for (auto &[slot, index] : rescue.slot_fixups) {
		st.scoped_variants[slot] = rescue.entries[index].addr;
	}
	rescue.clear();
}

APICALL(api_vstore) {
	auto [vidx, type, gdata, gsize] = machine.sysargs<unsigned *, int32_t, gaddr_t, gaddr_t>();
	auto &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("vstore", vidx, type, gdata, gsize);
	// Reuse the guest's Variant slot to avoid consuming one per iteration.
	// PACKED_STRING_ARRAY encodes "use the libc++ std::string layout" in the high bit of
	// the size, so it has to be stripped before the size is validated below.
	const bool libcpp_string_layout = type == Variant::PACKED_STRING_ARRAY && (gsize & 0x80000000);
	if (libcpp_string_layout) {
		gsize &= 0x7FFFFFFF;
	}
	if (gsize > MAX_ARRAY_ELEMENTS) {
		ERR_PRINT("vstore: Array size is too large: " + itos(gsize));
		throw std::runtime_error("vstore: Array size is too large: " + std::to_string(gsize));
	}

	// Find scoped Variant and store data from guest memory.
	switch (type) {
		case Variant::PACKED_BYTE_ARRAY: {
			PackedByteArray arr;
			// Copy the array from guest memory into the Variant.
			uint8_t *data = machine.memory.memarray<uint8_t>(gdata, gsize);
			arr.resize(gsize);
			guest_memcpy(arr.ptrw(), data, gsize);
			*vidx = emu.try_reuse_assign_variant(*vidx, Variant(std::move(arr)));
			break;
		}
		case Variant::PACKED_FLOAT32_ARRAY: {
			PackedFloat32Array arr;
			// Copy the array from guest memory into the Variant.
			float *data = machine.memory.memarray<float>(gdata, gsize);
			arr.resize(gsize);
			guest_memcpy(arr.ptrw(), data, gsize * sizeof(float));
			*vidx = emu.try_reuse_assign_variant(*vidx, Variant(std::move(arr)));
			break;
		}
		case Variant::PACKED_FLOAT64_ARRAY: {
			PackedFloat64Array arr;
			// Copy the array from guest memory into the Variant.
			double *data = machine.memory.memarray<double>(gdata, gsize);
			arr.resize(gsize);
			guest_memcpy(arr.ptrw(), data, gsize * sizeof(double));
			*vidx = emu.try_reuse_assign_variant(*vidx, Variant(std::move(arr)));
			break;
		}
		case Variant::PACKED_INT32_ARRAY: {
			PackedInt32Array arr;
			// Copy the array from guest memory into the Variant.
			int32_t *data = machine.memory.memarray<int32_t>(gdata, gsize);
			arr.resize(gsize);
			guest_memcpy(arr.ptrw(), data, gsize * sizeof(int32_t));
			*vidx = emu.try_reuse_assign_variant(*vidx, Variant(std::move(arr)));
			break;
		}
		case Variant::PACKED_INT64_ARRAY: {
			PackedInt64Array arr;
			// Copy the array from guest memory into the Variant.
			int64_t *data = machine.memory.memarray<int64_t>(gdata, gsize);
			arr.resize(gsize);
			guest_memcpy(arr.ptrw(), data, gsize * sizeof(int64_t));
			*vidx = emu.try_reuse_assign_variant(*vidx, Variant(std::move(arr)));
			break;
		}
		case Variant::PACKED_VECTOR2_ARRAY: {
			PackedVector2Array arr;
			// Copy the array from guest memory into the Variant.
			auto *data = machine.memory.memarray<Vector2>(gdata, gsize);
			arr.resize(gsize);
			guest_memcpy(arr.ptrw(), data, gsize * sizeof(Vector2));
			*vidx = emu.try_reuse_assign_variant(*vidx, Variant(std::move(arr)));
			break;
		}
		case Variant::PACKED_VECTOR3_ARRAY: {
			PackedVector3Array arr;
			// Copy the array from guest memory into the Variant.
			auto *data = machine.memory.memarray<Vector3>(gdata, gsize);
			arr.resize(gsize);
			guest_memcpy(arr.ptrw(), data, gsize * sizeof(Vector3));
			*vidx = emu.try_reuse_assign_variant(*vidx, Variant(std::move(arr)));
			break;
		}
		case Variant::PACKED_VECTOR4_ARRAY: {
			PackedVector4Array arr;
			// Copy the array from guest memory into the Variant.
			auto *data = machine.memory.memarray<Vector4>(gdata, gsize);
			arr.resize(gsize);
			guest_memcpy(arr.ptrw(), data, gsize * sizeof(Vector4));
			*vidx = emu.try_reuse_assign_variant(*vidx, Variant(std::move(arr)));
			break;
		}
		case Variant::PACKED_COLOR_ARRAY: {
			PackedColorArray arr;
			// Copy the array from guest memory into the Variant.
			auto *data = machine.memory.memarray<Color>(gdata, gsize);
			arr.resize(gsize);
			guest_memcpy(arr.ptrw(), data, gsize * sizeof(Color));
			*vidx = emu.try_reuse_assign_variant(*vidx, Variant(std::move(arr)));
			break;
		}
		case Variant::PACKED_STRING_ARRAY: {
			PackedStringArray arr;
			if (libcpp_string_layout) {
				// Work-around for libc++ std::string implementation.
				struct Buffer {
					gaddr_t ptr;
					gaddr_t size;
				};
				auto *buffers = machine.memory.memarray<Buffer>(gdata, gsize);
				arr.resize(gsize);
				for (unsigned i = 0; i < gsize; i++) {
					std::string_view view = machine.memory.memview(buffers[i].ptr, buffers[i].size);
					arr.set(i, String::utf8(view.data(), view.size()));
				}
			} else {
				// Copy the array from guest memory into the Variant.
				const CppString *data = machine.memory.memarray<CppString>(gdata, gsize);
				arr.resize(gsize);
				for (unsigned i = 0; i < gsize; i++) {
					arr.set(i, to_godot_string(&data[i], machine));
				}
			}
			*vidx = emu.try_reuse_assign_variant(*vidx, Variant(std::move(arr)));
			break;
		}
		default:
			ERR_PRINT("vstore: Cannot store value for Variant type");
			throw std::runtime_error("vstore: Cannot store value for Variant type " + std::to_string(type));
	}
}

APICALL(api_vassign) {
	auto [a_idx, b_idx] = machine.sysargs<unsigned, unsigned>();
	auto &emu = riscv::emu(machine);
	PENALIZE(150'000);
	SYS_TRACE("vassign", a_idx, b_idx);

	if (int32_t(a_idx) == INT32_MIN) {
		machine.set_result(b_idx); // Assign b to a directly when a is "empty".
		return;
	}

	// Find scoped Variants and assign the value of b to a.
	const Variant &va = get_scoped_variant_or_throw(emu, a_idx, "Variant::assign (destination)");
	const Variant &vb = get_scoped_variant_or_throw(emu, b_idx, "Variant::assign (source)");
	// XXX: This might be too strict. Assigning arbitrarily between different types is allowed in GDScript.
	const Variant::Type a_type = variant_type(va);
	const Variant::Type b_type = variant_type(vb);
	if (a_type != Variant::NIL && a_type != b_type) {
		ERR_PRINT("vassign: Variant types do not match");
		throw std::runtime_error("vassign: Variant types do not match: " + std::string(GuestVariant::type_name(a_type)) + " != " + std::string(GuestVariant::type_name(b_type)));
	}

	// Try assigning the value of b to a.
	unsigned res_idx = emu.try_reuse_assign_variant(b_idx, va, a_idx, vb);
	machine.set_result(res_idx);
}

APICALL(api_obj_retain) {
	auto [slot_addr] = machine.sysargs<gaddr_t>();
	auto &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("obj_retain", slot_addr);

	emu.retain_global_object(slot_addr);
}

APICALL(api_vstore_global) {
	auto [dst_addr, src_addr] = machine.sysargs<gaddr_t, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("vstore_global", dst_addr, src_addr);

	GuestVariant *dst = machine.memory.memarray<GuestVariant>(dst_addr, 1);
	const GuestVariant *src = machine.memory.memarray<const GuestVariant>(src_addr, 1);

	const int32_t held = dst->is_scoped_variant() ? int32_t(dst->v.i) : 0;
	const bool holds_permanent = Sandbox::is_permanent_variant(held);
	const int32_t previous_type = dst->type;

	if (!src->is_scoped_variant()) {
		if (holds_permanent) {
			emu.release_permanent_variant(held);
		}
		*dst = *src;
	} else if (holds_permanent && int32_t(src->v.i) == held) {
			dst->type = src->type;
	} else {
		Variant value = get_scoped_variant_or_throw(emu, int32_t(src->v.i),
				"assignment to a member");
		if (holds_permanent) {
			emu.release_permanent_variant(held);
		}
		const int32_t stored = emu.create_permanent_variant_from(std::move(value));
		if (UNLIKELY(stored == 0)) {
			dst->type = Variant::NIL;
			dst->v.i = 0;
		} else {
			dst->type = src->type;
			dst->v.i = stored;
		}
	}

	if (dst->type == Variant::OBJECT || previous_type == Variant::OBJECT) {
		emu.retain_global_object(dst_addr);
	}
}

APICALL(api_get_obj) {
	auto [name] = machine.sysargs<std::string>();
	auto &emu = riscv::emu(machine);
	PENALIZE(150'000);
	SYS_TRACE("get_obj", String::utf8(name.c_str(), name.size()));

	// Objects retrieved by name are named globals, eg. "Engine", "Input", "Time",
	// which are also their class names. As such, we can restrict access using
	// the allowed_classes list in the Sandbox.
	if (!emu.is_allowed_class(String::utf8(name.c_str(), name.size()))) {
		ERR_PRINT("Class is not allowed");
		machine.set_result(0);
		return;
	}

	// Find allowed object by name and get its address from a lambda.
	auto it = global_singleton_list.find(name);
	if (it != global_singleton_list.end()) {
		godot::Object *obj = reinterpret_cast<godot::Object *>(it->second());
		machine.set_result(emu.add_scoped_object(obj));
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
		SceneTree *tree = owner_node->get_tree();
		machine.set_result(emu.add_scoped_object(tree));
		return;
	}

	// Tree from main loop: _init() runs before the owner enters the tree.
	if (name.find('/') == std::string::npos) {
		SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
		Window *root = tree != nullptr ? tree->get_root() : nullptr;
		Node *autoload = root != nullptr
				? root->get_node_or_null(NodePath(String::utf8(name.c_str(), name.size())))
				: nullptr;
		if (autoload != nullptr) {
			machine.set_result(emu.add_scoped_object(autoload));
			return;
		}
	}

	ERR_PRINT(("Unknown or inaccessible object: " + name).c_str());
	machine.set_result(0);
}

APICALL(api_obj) {
	auto [op, addr, gvar] = machine.sysargs<int, uint64_t, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(250'000); // Costly Object operations.
	SYS_TRACE("obj_op", op, addr, gvar);

	godot::Object *obj = get_object_from_address(emu, addr);

	switch (Object_Op(op)) {
		case Object_Op::GET_METHOD_LIST: {
			// Enumerating an object tells the guest what it may try next, so it is gated
			// like the call it is a prelude to.
			if (UNLIKELY(!emu.is_allowed_method(obj, "get_method_list"))) {
				ERR_PRINT("Banned method called: get_method_list");
				throw std::runtime_error("Banned method called: get_method_list");
			}
			CppVector<CppString> *vec = machine.memory.memarray<CppVector<CppString>>(gvar, 1);
			// XXX: vec->free(machine);
			auto methods = obj->get_method_list();
			vec->resize(machine, methods.size());
			for (size_t i = 0; i < methods.size(); i++) {
				Dictionary dict = methods[i].operator godot::Dictionary();
				auto name = String(dict["name"]).utf8();
				const gaddr_t self = vec->address_at(i);
				vec->at(machine, i).set_string(machine, self, name.ptr(), name.length());
			}
		} break;
		case Object_Op::GET: { // Get a property of the object.
			GuestVariant *var = machine.memory.memarray<GuestVariant>(gvar, 2);
			String name = var[0].toVariant(emu).operator String();
			if (UNLIKELY(!emu.is_allowed_property(obj, name, false))) {
				ERR_PRINT("Banned property accessed: " + name);
				throw std::runtime_error("Banned property accessed");
			}
			var[1].create(emu, obj->get(name));
		} break;
		case Object_Op::SET: { // Set a property of the object.
			GuestVariant *var = machine.memory.memarray<GuestVariant>(gvar, 2);
			String name = var[0].toVariant(emu).operator String();
			if (UNLIKELY(!emu.is_allowed_property(obj, name, true))) {
				ERR_PRINT("Banned property set: " + name);
				throw std::runtime_error("Banned property set");
			}
			obj->set(name, var[1].toVariant(emu));
		} break;
		case Object_Op::GET_PROPERTY_LIST: {
			if (UNLIKELY(!emu.is_allowed_method(obj, "get_property_list"))) {
				ERR_PRINT("Banned method called: get_property_list");
				throw std::runtime_error("Banned method called: get_property_list");
			}
			CppVector<CppString> *vec = machine.memory.memarray<CppVector<CppString>>(gvar, 1);
			// XXX: vec->free(machine);
			TypedArray<Dictionary> properties = obj->get_property_list();
			vec->resize(machine, properties.size());
			for (size_t i = 0; i < properties.size(); i++) {
				Dictionary dict = properties[i].operator godot::Dictionary();
				auto name = String(dict["name"]).utf8();
				const gaddr_t self = vec->address_at(i);
				vec->at(machine, i).set_string(machine, self, name.ptr(), name.length());
			}
		} break;
		case Object_Op::CONNECT: {
			GuestVariant *vars = machine.memory.memarray<GuestVariant>(gvar, 3);
			godot::Object *target = get_object_from_address(emu, vars[0].v.i);
			const String signal_name = vars[1].toVariant(emu).operator String();
			const String method_name = vars[2].toVariant(emu).operator String();
			// Check connect() on the object, and also the method on the target, as
			// connecting is a deferred way of calling that method on the target.
			if (UNLIKELY(!emu.is_allowed_method(obj, "connect"))) {
				ERR_PRINT("Banned method called: connect");
				throw std::runtime_error("Banned method called: connect");
			}
			if (UNLIKELY(!emu.is_allowed_method(target, method_name))) {
				ERR_PRINT("Banned method connected: " + method_name);
				throw std::runtime_error("Banned method connected: " + std::string(method_name.utf8()));
			}
			obj->connect(signal_name, Callable(target, method_name));
		} break;
		case Object_Op::DISCONNECT: {
			GuestVariant *vars = machine.memory.memarray<GuestVariant>(gvar, 3);
			godot::Object *target = get_object_from_address(emu, vars[0].v.i);
			const String signal_name = vars[1].toVariant(emu).operator String();
			const String method_name = vars[2].toVariant(emu).operator String();
			if (UNLIKELY(!emu.is_allowed_method(obj, "disconnect"))) {
				ERR_PRINT("Banned method called: disconnect");
				throw std::runtime_error("Banned method called: disconnect");
			}
			obj->disconnect(signal_name, Callable(target, method_name));
		} break;
		case Object_Op::GET_SIGNAL_LIST: {
			if (UNLIKELY(!emu.is_allowed_method(obj, "get_signal_list"))) {
				ERR_PRINT("Banned method called: get_signal_list");
				throw std::runtime_error("Banned method called: get_signal_list");
			}
			CppVector<CppString> *vec = machine.memory.memarray<CppVector<CppString>>(gvar, 1);
			TypedArray<Dictionary> signals = obj->get_signal_list();
			vec->resize(machine, signals.size());
			for (size_t i = 0; i < signals.size(); i++) {
				Dictionary dict = signals[i].operator godot::Dictionary();
				auto name = String(dict["name"]).utf8();
				const gaddr_t self = vec->address_at(i);
				vec->at(machine, i).set_string(machine, self, std::string_view(name.ptr(), name.length()));
			}
		} break;
		default:
			throw std::runtime_error("Invalid Object operation: " + std::to_string(op));
	}
}

APICALL(api_obj_property_get) {
	auto [addr, g_property, g_property_len, vret] = machine.sysargs<uint64_t, gaddr_t, unsigned, GuestVariant *>();
	const std::string_view method = machine.memory.memview(g_property, g_property_len);
	auto &emu = riscv::emu(machine);
	PENALIZE(150'000);
	SYS_TRACE("obj_property_get", addr, method, vret);

	godot::Object *obj = nullptr;
	if (!Sandbox::is_variant_index_handle(addr)) {
		obj = get_object_from_address(emu, addr);
	} else {
		// It's a Variant index, scoped or permanent
		const Variant &var = get_scoped_variant_or_throw(emu, int32_t(addr), "Object::get_property");
		const Variant::Type var_type = variant_type(var);
		if (var_type != Variant::OBJECT) {
			bool valid = false;
			Sandbox::CachedNameRef cached_member = emu.cached_guest_name(g_property, method, false);
			const StringName &member = cached_member->sname;
			Variant value = var.get_named(member, valid);
			if (!valid) {
				ERR_PRINT("api_obj_property_get: " + String(GuestVariant::type_name(var_type)) +
						" has no member " + member);
				throw std::runtime_error("api_obj_property_get: " +
						std::string(GuestVariant::type_name(var_type)) + " has no member " +
						std::string(method));
			}
			vret->create(emu, std::move(value));
			return;
		}
		obj = var.operator godot::Object *();
	}
	Sandbox::CachedNameRef cached_property = emu.cached_guest_name(g_property, method, false);
	const StringName &prop_name = cached_property->sname;

	if (UNLIKELY(!emu.is_allowed_property(obj, prop_name, false))) {
		ERR_PRINT("Banned property accessed: " + prop_name);
		throw std::runtime_error("Banned property accessed: " + std::string(method));
	}

	vret->create(emu, obj->get(prop_name));
}

APICALL(api_obj_property_set) {
	auto [addr, g_property, g_property_len, g_value] = machine.sysargs<uint64_t, gaddr_t, unsigned, const GuestVariant *>();
	const std::string_view method = machine.memory.memview(g_property, g_property_len);
	auto &emu = riscv::emu(machine);
	PENALIZE(150'000);
	SYS_TRACE("obj_property_set", addr, method, value);

	godot::Object *obj = nullptr;
	if (!Sandbox::is_variant_index_handle(addr)) {
		obj = get_object_from_address(emu, addr);
	} else {
		// It's a Variant index, scoped or permanent
		const Variant &var = get_scoped_variant_or_throw(emu, int32_t(addr), "Object::set_property");
		if (variant_type(var) != Variant::OBJECT) {
			// Built-in member set: get_mutable copies a borrowed Variant first.
			Variant &target = emu.get_mutable_scoped_variant(int32_t(addr));
			bool valid = false;
			Sandbox::CachedNameRef cached_member = emu.cached_guest_name(g_property, method, false);
			const StringName &member = cached_member->sname;
			const BorrowedVariant value(emu, *g_value);
			target.set_named(member, *value, valid);
			if (!valid) {
				const Variant::Type target_type = variant_type(target);
				ERR_PRINT("api_obj_property_set: " + String(GuestVariant::type_name(target_type)) +
						" has no member " + member);
				throw std::runtime_error("api_obj_property_set: " +
						std::string(GuestVariant::type_name(target_type)) + " has no member " +
						std::string(method));
			}
			return;
		}
		obj = var.operator godot::Object *();
	}
	Sandbox::CachedNameRef cached_property = emu.cached_guest_name(g_property, method, false);
	const StringName &prop_name = cached_property->sname;

	if (UNLIKELY(!emu.is_allowed_property(obj, prop_name, true))) {
		ERR_PRINT("Banned property set: " + prop_name);
		throw std::runtime_error("Banned property set: " + std::string(method));
	}

	obj->set(prop_name, *BorrowedVariant(emu, *g_value));
}

APICALL(api_obj_callp) {
	auto [addr, g_method, g_method_len, deferred, vret_ptr, args_addr, args_size] = machine.sysargs<uint64_t, gaddr_t, unsigned, bool, gaddr_t, gaddr_t, unsigned>();
	auto &emu = riscv::emu(machine);
	PENALIZE(250'000); // Costly Object call operation.
	SYS_TRACE("obj_callp", addr, g_method, g_method_len, deferred, vret_ptr, args_addr, args_size);

	auto *obj = get_object_from_address(emu, addr);
	if (UNLIKELY(args_size > gdscript::CallABI::MAX_ARGUMENTS || (deferred && args_size > 8))) {
		ERR_PRINT("Too many arguments to obj_callp");
		throw std::runtime_error("Too many arguments to obj_callp");
	}
	// Zero-argument calls are the common case, and have nothing to translate or validate.
	const GuestVariant *g_args = args_size ? machine.memory.memarray<GuestVariant>(args_addr, args_size) : nullptr;

	const std::string_view method_view = memview_with_terminator(machine, g_method, g_method_len).substr(0, size_t(g_method_len) + 1);
	Sandbox::CachedNameRef method = emu.cached_guest_name(g_method,
			method_view.substr(0, g_method_len), method_view.back() == '\0');

	// Check for banned methods.
	if (UNLIKELY(!emu.is_allowed_method(obj, method->variant))) {
		ERR_PRINT("Banned method called: " + method->variant.operator String());
		throw std::runtime_error("Banned method called: " + std::string(method_view.substr(0, g_method_len)));
	}

	if (!deferred) {
		CallResult result;
		object_call(emu, obj, method.get(), method_view.substr(0, g_method_len),
				g_args, args_size, result);
		if (vret_ptr != 0) {
			GuestVariant *vret = machine.memory.memarray<GuestVariant>(vret_ptr, 1);
			vret->create(emu, std::move(result.get()));
		}
	} else {
		// The call itself runs after this syscall returns, with no frame left to throw
		// from; a missing method is still decidable here.
		if (UNLIKELY(!obj->has_method(method->sname))) {
			ERR_PRINT("Nonexistent method deferred: " + method->variant.operator String());
			throw std::runtime_error("Nonexistent method deferred: " + std::string(method_view.substr(0, g_method_len)));
		}
		// Call deferred unfortunately takes a parameter pack, so we have to manually
		// check the number of arguments, and call the correct function.
		if (args_size == 0) {
			obj->call_deferred(method->sname);
		} else if (args_size == 1) {
			obj->call_deferred(method->sname, g_args[0].toVariant(emu));
		} else if (args_size == 2) {
			obj->call_deferred(method->sname, g_args[0].toVariant(emu), g_args[1].toVariant(emu));
		} else if (args_size == 3) {
			obj->call_deferred(method->sname, g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu));
		} else if (args_size == 4) {
			obj->call_deferred(method->sname, g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu), g_args[3].toVariant(emu));
		} else if (args_size == 5) {
			obj->call_deferred(method->sname, g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu), g_args[3].toVariant(emu), g_args[4].toVariant(emu));
		} else if (args_size == 6) {
			obj->call_deferred(method->sname, g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu), g_args[3].toVariant(emu), g_args[4].toVariant(emu), g_args[5].toVariant(emu));
		} else if (args_size == 7) {
			obj->call_deferred(method->sname, g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu), g_args[3].toVariant(emu), g_args[4].toVariant(emu), g_args[5].toVariant(emu), g_args[6].toVariant(emu));
		} else if (args_size == 8) {
			obj->call_deferred(method->sname, g_args[0].toVariant(emu), g_args[1].toVariant(emu), g_args[2].toVariant(emu), g_args[3].toVariant(emu), g_args[4].toVariant(emu), g_args[5].toVariant(emu), g_args[6].toVariant(emu), g_args[7].toVariant(emu));
		}
	}
}

APICALL(api_get_node) {
	auto [addr, name] = machine.sysargs<uint64_t, std::string_view>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(150'000);
	SYS_TRACE("get_node", addr, String::utf8(name.data(), name.size()));

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
		Node *base_node = get_node_from_address(emu, addr);
		node = base_node->get_node<Node>(NodePath(c_name.c_str()));
	}
	if (node == nullptr) {
		ERR_PRINT(("Node not found: " + c_name).c_str());
		machine.set_result(0);
		return;
	}

	machine.set_result(emu.add_scoped_object(node));
}

APICALL(api_node_create) {
	auto [type, g_class_name, g_class_len, name] = machine.sysargs<Node_Create_Shortlist, gaddr_t, unsigned, std::string_view>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(150'000);
	Node *node = nullptr;

	switch (type) {
		case Node_Create_Shortlist::CREATE_CLASSDB: {
			// Get the class name from guest memory, including null terminator.
			std::string_view class_name = memview_with_terminator(machine, g_class_name, g_class_len);
			// Verify the class name is null-terminated.
			if (class_name[g_class_len] != '\0') {
				ERR_PRINT("Class name is not null-terminated");
				throw std::runtime_error("Class name is not null-terminated");
			}
			StringName class_name_sn(class_name.data());
			if (!emu.is_allowed_class(class_name_sn)) {
				ERR_PRINT("Class name is not allowed");
				throw std::runtime_error("Class name is not allowed");
			}
			// Now that it's null-terminated, we can use it for StringName.
			Variant result = ClassDBSingleton::get_singleton()->instantiate(class_name_sn);
			if (result.get_type() != Variant::OBJECT) {
				ERR_PRINT("Failed to create object from class name");
				throw std::runtime_error("Failed to create object from class name");
			}
			Object *obj = result.operator Object *();
			// Make sure the object held through the Variant has lifetime managed by the sandbox.
			emu.create_scoped_variant(std::move(result));

			node = fast_cast_to<Node>(obj);
			// If it's not a Node, just return the Object.
			if (node == nullptr) {
				machine.set_result(emu.add_scoped_object(obj));
				return;
			}
			// It's a Node, so continue to set the name.
			break;
		}
		case Node_Create_Shortlist::CREATE_NODE: // Node
			if (!emu.is_allowed_class("Node")) {
				ERR_PRINT("Class name is not allowed");
				throw std::runtime_error("Class name is not allowed");
			}
			node = memnew(Node);
			break;
		case Node_Create_Shortlist::CREATE_NODE2D: // Node2D
			if (!emu.is_allowed_class("Node2D")) {
				ERR_PRINT("Class name is not allowed");
				throw std::runtime_error("Class name is not allowed");
			}
			node = memnew(Node2D);
			break;
		case Node_Create_Shortlist::CREATE_NODE3D: // Node3D
			if (!emu.is_allowed_class("Node3D")) {
				ERR_PRINT("Class name is not allowed");
				throw std::runtime_error("Class name is not allowed");
			}
			node = memnew(Node3D);
			break;
		default:
			ERR_PRINT("Unknown Node type");
			throw std::runtime_error("Unknown Node type");
	}

	if (node == nullptr) {
		ERR_PRINT("Failed to create Node");
		throw std::runtime_error("Failed to create Node");
	}
	if (!name.empty()) {
		node->set_name(String::utf8(name.data(), name.size()));
	}
	machine.set_result(emu.add_scoped_object(node));
}

APICALL(api_node) {
	auto [op, addr, gvar] = machine.sysargs<int, uint64_t, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(250'000); // Costly Node operations.
	SYS_TRACE("node_op", op, addr, gvar);

	// Get the Node object by its address.
	godot::Node *node = get_node_from_address(emu, addr);

	switch (Node_Op(op)) {
		case Node_Op::GET_NAME: {
			// Check if getting the name is allowed.
			if (UNLIKELY(!emu.is_allowed_property(node, "name", false))) {
				ERR_PRINT("Banned property accessed: name");
				throw std::runtime_error("Banned property accessed: name");
			}
			GuestVariant *var = machine.memory.memarray<GuestVariant>(gvar, 1);
			var->create(emu, node->get_name());
		} break;
		case Node_Op::SET_NAME: {
			// Check if setting the name is allowed.
			if (UNLIKELY(!emu.is_allowed_property(node, "name", true))) {
				ERR_PRINT("Banned property set: name");
				throw std::runtime_error("Banned property set: name");
			}
			GuestVariant *var = machine.memory.memarray<GuestVariant>(gvar, 1);
			node->set_name(var->toVariant(emu));
		} break;
		case Node_Op::GET_PATH: {
			// Check if getting the path is allowed.
			if (UNLIKELY(!emu.is_allowed_method(node, "path"))) {
				ERR_PRINT("Banned method accessed: path");
				throw std::runtime_error("Banned method accessed: path");
			}
			GuestVariant *var = machine.memory.memarray<GuestVariant>(gvar, 1);
			var->create(emu, node->get_path());
		} break;
		case Node_Op::GET_PARENT: {
			// Check if getting the parent is allowed.
			if (UNLIKELY(!emu.is_allowed_method(node, "get_parent"))) {
				ERR_PRINT("Banned method accessed: get_parent");
				throw std::runtime_error("Banned method accessed: get_parent");
			}
			uint64_t *result = machine.memory.memarray<uint64_t>(gvar, 1);
			godot::Object *parent = node->get_parent();
			if (UNLIKELY(parent == nullptr)) {
				*result = 0;
			} else {
				// TODO: Parent nodes allow access higher up the tree, which could be a security issue.
				if (!emu.is_allowed_object(parent))
					throw std::runtime_error("Node::get_parent(): Parent is not allowed");
				*result = emu.add_scoped_object(parent);
			}
		} break;
		case Node_Op::QUEUE_FREE:
			if (UNLIKELY(node == &emu)) {
				ERR_PRINT("Cannot queue free the sandbox");
				throw std::runtime_error("Cannot queue free the sandbox");
			}
			// Check if queue_free is an allowed method.
			if (UNLIKELY(!emu.is_allowed_method(node, "queue_free"))) {
				ERR_PRINT("Banned method called: queue_free");
				throw std::runtime_error("Banned method called: queue_free");
			}
			//emu.rem_scoped_object(node);
			node->queue_free();
			break;
		case Node_Op::DUPLICATE: {
			// Check if creating a new node of this type is allowed.
			if (UNLIKELY(!emu.is_allowed_class(node->get_class()))) {
				throw std::runtime_error("Node::duplicate(): Creating a new node of this type is not allowed");
			}
			// Check if duplicate is an allowed method.
			if (UNLIKELY(!emu.is_allowed_method(node, "duplicate"))) {
				ERR_PRINT("Banned method called: duplicate");
				throw std::runtime_error("Banned method called: duplicate");
			}
			uint64_t *result = machine.memory.memarray<uint64_t>(gvar, 1);
			int flags = machine.cpu.reg(13); // Flags are passed in reg 13.
			auto *new_node = node->duplicate(flags);
			*result = emu.add_scoped_object(new_node);
		} break;
		case Node_Op::GET_CHILD_COUNT: {
			// Check if getting the child count is allowed.
			if (UNLIKELY(!emu.is_allowed_method(node, "get_child_count"))) {
				ERR_PRINT("Banned method accessed: get_child_count");
				throw std::runtime_error("Banned method accessed: get_child_count");
			}
			int64_t *result = machine.memory.memarray<int64_t>(gvar, 1);
			*result = node->get_child_count();
		} break;
		case Node_Op::GET_CHILD: {
			// Check if getting a child is allowed.
			if (UNLIKELY(!emu.is_allowed_method(node, "get_child"))) {
				ERR_PRINT("Banned method accessed: get_child");
				throw std::runtime_error("Banned method accessed: get_child");
			}
			GuestVariant *var = machine.memory.memarray<GuestVariant>(gvar, 1);
			Node *child_node = node->get_child(var[0].v.i);
			// Disallowed children are indistinguishable from missing ones, so that
			// iterating the tree does not become a way to probe for banned objects.
			if (UNLIKELY(child_node == nullptr || !emu.is_allowed_object(child_node))) {
				var[0].set(emu, Variant());
			} else {
				var[0].set(emu, int64_t(emu.add_scoped_object(child_node)));
			}
		} break;
		case Node_Op::ADD_CHILD_DEFERRED:
		case Node_Op::ADD_CHILD: {
			// Check for banned methods.
			if (UNLIKELY(!emu.is_allowed_method(node, "add_child"))) {
				ERR_PRINT("Banned method called: add_child");
				throw std::runtime_error("Banned method called: add_child");
			}
			GuestVariant *child = machine.memory.memarray<GuestVariant>(gvar, 1);
			godot::Node *child_node = get_node_from_address(emu, child->v.i);
			if (Node_Op(op) == Node_Op::ADD_CHILD_DEFERRED)
				node->call_deferred("add_child", child_node);
			else
				node->add_child(child_node);
		} break;
		case Node_Op::ADD_SIBLING_DEFERRED:
		case Node_Op::ADD_SIBLING: {
			// Check for banned methods.
			if (UNLIKELY(!emu.is_allowed_method(node, "add_sibling"))) {
				ERR_PRINT("Banned method called: add_sibling");
				throw std::runtime_error("Banned method called: add_sibling");
			}
			GuestVariant *sibling = machine.memory.memarray<GuestVariant>(gvar, 1);
			godot::Node *sibling_node = get_node_from_address(emu, sibling->v.i);
			if (Node_Op(op) == Node_Op::ADD_SIBLING_DEFERRED)
				node->call_deferred("add_sibling", sibling_node);
			else
				node->add_sibling(sibling_node);
		} break;
		case Node_Op::MOVE_CHILD: {
			// Check for banned methods.
			if (UNLIKELY(!emu.is_allowed_method(node, "move_child"))) {
				ERR_PRINT("Banned method called: move_child");
				throw std::runtime_error("Banned method called: move_child");
			}
			GuestVariant *vars = machine.memory.memarray<GuestVariant>(gvar, 2);
			godot::Node *child_node = get_node_from_address(emu, vars[0].v.i);
			// TODO: Check if the child is actually a child of the node? Verify index?
			node->move_child(child_node, vars[1].v.i);
		} break;
		case Node_Op::REMOVE_CHILD_DEFERRED:
		case Node_Op::REMOVE_CHILD: {
			// Check for banned methods.
			if (UNLIKELY(!emu.is_allowed_method(node, "remove_child"))) {
				ERR_PRINT("Banned method called: remove_child");
				throw std::runtime_error("Banned method called: remove_child");
			}
			GuestVariant *child = machine.memory.memarray<GuestVariant>(gvar, 1);
			godot::Node *child_node = get_node_from_address(emu, child->v.i);
			if (Node_Op(op) == Node_Op::REMOVE_CHILD_DEFERRED)
				node->call_deferred("remove_child", child_node);
			else
				node->remove_child(child_node);
		} break;
		case Node_Op::GET_CHILDREN: {
			// Check if getting the children is allowed.
			if (UNLIKELY(!emu.is_allowed_method(node, "get_children"))) {
				ERR_PRINT("Banned method accessed: get_children");
				throw std::runtime_error("Banned method accessed: get_children");
			}
			// Get a GuestStdVector from guest to store the children.
			CppVector<uint64_t> *vec = machine.memory.memarray<CppVector<uint64_t>>(gvar, 1);
			// Get the children of the node.
			TypedArray<Node> children = node->get_children();
			// Allocate memory for the children in the guest vector.
			vec->reserve(machine, children.size());
			// Copy the children to the guest vector, and add them to the scoped objects.
			for (int i = 0; i < children.size(); i++) {
				godot::Node *child = fast_cast_to<godot::Node>(children[i]);
				if (child && emu.is_allowed_object(child)) {
					vec->push_back(machine, emu.add_scoped_object(child));
				} else {
					// Disallowed children are returned as null, keeping the indices intact.
					vec->push_back(machine, 0);
				}
			}
			// No return value is needed.
		} break;
		case Node_Op::ADD_TO_GROUP: {
			// Check for banned methods.
			if (UNLIKELY(!emu.is_allowed_method(node, "add_to_group"))) {
				ERR_PRINT("Banned method called: add_to_group");
				throw std::runtime_error("Banned method called: add_to_group");
			}
			// Reg 12: Group string pointer, Reg 13: Group string length.
			std::string_view group = machine.memory.memview(gvar, machine.cpu.reg(13));
			node->add_to_group(String::utf8(group.data(), group.size()));
		} break;
		case Node_Op::REMOVE_FROM_GROUP: {
			// Check for banned methods.
			if (UNLIKELY(!emu.is_allowed_method(node, "remove_from_group"))) {
				ERR_PRINT("Banned method called: remove_from_group");
				throw std::runtime_error("Banned method called: remove_from_group");
			}
			// Reg 12: Group string pointer, Reg 13: Group string length.
			std::string_view group = machine.memory.memview(gvar, machine.cpu.reg(13));
			node->remove_from_group(String::utf8(group.data(), group.size()));
		} break;
		case Node_Op::IS_IN_GROUP: {
			// Check for banned methods.
			if (UNLIKELY(!emu.is_allowed_method(node, "is_in_group"))) {
				ERR_PRINT("Banned method accessed: is_in_group");
				throw std::runtime_error("Banned method accessed: is_in_group");
			}
			// Reg 12: Group string pointer, Reg 13: Group string length, Reg 14: Result bool pointer.
			std::string_view group = machine.memory.memview(gvar, machine.cpu.reg(13));
			bool *result = machine.memory.memarray<bool>(machine.cpu.reg(14), 1);
			*result = node->is_in_group(String::utf8(group.data(), group.size()));
		} break;
		case Node_Op::REPLACE_BY: {
			// Check for banned methods.
			if (UNLIKELY(!emu.is_allowed_method(node, "replace_by"))) {
				ERR_PRINT("Banned method called: replace_by");
				throw std::runtime_error("Banned method called: replace_by");
			}
			// Reg 12: Node address to replace with, Reg 13: Keep groups bool.
			godot::Node *replace_node = get_node_from_address(emu, gvar);
			bool keep_groups = machine.cpu.reg(13);
			node->replace_by(replace_node, keep_groups);
		} break;
		case Node_Op::REPARENT: {
			// Check for banned methods.
			if (UNLIKELY(!emu.is_allowed_method(node, "reparent"))) {
				ERR_PRINT("Banned method called: reparent");
				throw std::runtime_error("Banned method called: reparent");
			}
			// Reg 12: New parent node address, Reg 13: Keep transform bool.
			godot::Node *new_parent = get_node_from_address(emu, gvar);
			bool keep_transform = machine.cpu.reg(13);
			node->reparent(new_parent, keep_transform);
		} break;
		case Node_Op::IS_INSIDE_TREE: {
			// Check for banned methods.
			if (UNLIKELY(!emu.is_allowed_method(node, "is_inside_tree"))) {
				ERR_PRINT("Banned method accessed: is_inside_tree");
				throw std::runtime_error("Banned method accessed: is_inside_tree");
			}
			// Reg 12: Result bool pointer.
			bool *result = machine.memory.memarray<bool>(gvar, 1);
			*result = node->is_inside_tree();
		} break;
		default:
			throw std::runtime_error("Invalid Node operation");
	}
}

APICALL(api_node2d) {
	// Node2D operation, Node2D address, and the variant to get/set the value.
	auto [op, addr, gvar] = machine.sysargs<int, uint64_t, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(100'000); // Costly Node2D operations.
	SYS_TRACE("node2d_op", op, addr, gvar);

	// Get the Node2D object by its address. Checking for Node2D directly also establishes
	// that it is a Node, so there is no reason to pay for both checks.
	godot::Node2D *node2d = get_class_from_address<godot::Node2D>(emu, addr);

	// Every Node2D operation is a property access, and is gated the same way the
	// equivalent access through Object::get()/set() would be. Without this the whole
	// 2D transform of any scoped node is reachable with restrictions turned on.
	const auto allowed = [&emu, node2d](const char *property, bool is_set) {
		if (UNLIKELY(!emu.is_allowed_property(node2d, property, is_set))) {
			ERR_PRINT(String(is_set ? "Banned property set: " : "Banned property accessed: ") + property);
			throw std::runtime_error(std::string(is_set ? "Banned property set: " : "Banned property accessed: ") + property);
		}
	};

	// View the variant from the guest memory.
	GuestVariant *var = machine.memory.memarray<GuestVariant>(gvar, 1);
	switch (Node2D_Op(op)) {
		case Node2D_Op::GET_POSITION:
			allowed("position", false);
			var->set(emu, node2d->get_position());
			break;
		case Node2D_Op::SET_POSITION:
			allowed("position", true);
			node2d->set_deferred("position", var->toVariant(emu));
			break;
		case Node2D_Op::GET_ROTATION:
			allowed("rotation", false);
			var->set(emu, node2d->get_rotation());
			break;
		case Node2D_Op::SET_ROTATION:
			allowed("rotation", true);
			node2d->set_rotation(var->toVariant(emu));
			break;
		case Node2D_Op::GET_SCALE:
			allowed("scale", false);
			var->set(emu, node2d->get_scale());
			break;
		case Node2D_Op::SET_SCALE:
			allowed("scale", true);
			node2d->set_scale(var->toVariant(emu));
			break;
		case Node2D_Op::GET_SKEW:
			allowed("skew", false);
			var->set(emu, node2d->get_skew());
			break;
		case Node2D_Op::SET_SKEW:
			allowed("skew", true);
			node2d->set_skew(var->toVariant(emu));
			break;
		case Node2D_Op::GET_TRANSFORM:
			allowed("transform", false);
			var->create(emu, node2d->get_transform());
			break;
		case Node2D_Op::SET_TRANSFORM:
			allowed("transform", true);
			node2d->set_transform(*var->toVariantPtr(emu));
			break;
		default:
			ERR_PRINT("Invalid Node2D operation");
			throw std::runtime_error("Invalid Node2D operation");
	}
}

APICALL(api_node3d) {
	// Node3D operation, Node3D address, and the variant to get/set the value.
	auto [op, addr, gvar] = machine.sysargs<int, uint64_t, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(100'000); // Costly Node3D operations.
	SYS_TRACE("node3d_op", op, addr, gvar);

	// Get the Node3D object by its address. Checking for Node3D directly also establishes
	// that it is a Node, so there is no reason to pay for both checks.
	godot::Node3D *node3d = get_class_from_address<godot::Node3D>(emu, addr);

	// See api_node2d(): these are property accesses and are gated as such.
	const auto allowed = [&emu, node3d](const char *property, bool is_set) {
		if (UNLIKELY(!emu.is_allowed_property(node3d, property, is_set))) {
			ERR_PRINT(String(is_set ? "Banned property set: " : "Banned property accessed: ") + property);
			throw std::runtime_error(std::string(is_set ? "Banned property set: " : "Banned property accessed: ") + property);
		}
	};

	// View the variant from the guest memory.
	GuestVariant *var = machine.memory.memarray<GuestVariant>(gvar, 1);
	switch (Node3D_Op(op)) {
		case Node3D_Op::GET_POSITION:
			allowed("position", false);
			var->set(emu, node3d->get_position());
			break;
		case Node3D_Op::SET_POSITION:
			allowed("position", true);
			node3d->set_position(var->toVariant(emu));
			break;
		case Node3D_Op::GET_ROTATION:
			allowed("rotation", false);
			var->set(emu, node3d->get_rotation());
			break;
		case Node3D_Op::SET_ROTATION:
			allowed("rotation", true);
			node3d->set_rotation(var->toVariant(emu));
			break;
		case Node3D_Op::GET_SCALE:
			allowed("scale", false);
			var->set(emu, node3d->get_scale());
			break;
		case Node3D_Op::SET_SCALE:
			allowed("scale", true);
			node3d->set_scale(var->toVariant(emu));
			break;
		case Node3D_Op::GET_TRANSFORM:
			allowed("transform", false);
			var->create(emu, node3d->get_transform());
			break;
		case Node3D_Op::SET_TRANSFORM:
			allowed("transform", true);
			node3d->set_transform(*var->toVariantPtr(emu));
			break;
		case Node3D_Op::GET_QUATERNION:
			allowed("quaternion", false);
			var->set(emu, node3d->get_quaternion());
			break;
		case Node3D_Op::SET_QUATERNION:
			allowed("quaternion", true);
			node3d->set_quaternion(var->toVariant(emu));
			break;
		default:
			ERR_PRINT("Invalid Node3D operation");
			throw std::runtime_error("Invalid Node3D operation");
	}
}

APICALL(api_throw) {
	auto [type, msg, vaddr, vfunc] = machine.sysargs<std::string_view, std::string_view, gaddr_t, gaddr_t>();
	SYS_TRACE("throw", String::utf8(type.data(), type.size()), String::utf8(msg.data(), msg.size()), vaddr);

	const String exception_type = String::utf8(type.data(), type.size());
	String error_string;
	GuestVariant *var = vaddr != 0 ? machine.memory.memarray<GuestVariant>(vaddr, 1) : nullptr;

	if (var != nullptr && (var->type == Variant::STRING || var->type == Variant::STRING_NAME)) {
		error_string = "Sandbox exception in " + exception_type + ": " +
				var->toVariant(riscv::emu(machine)).operator String();
	} else if (var != nullptr) {
		error_string = "Sandbox exception of type " + exception_type + ": " +
				String::utf8(msg.data(), msg.size()) + " for Variant of type " + itos(var->type);
		if (var->type >= 0 && var->type < Variant::VARIANT_MAX) {
			error_string += " (" + String::utf8(GuestVariant::type_name(var->type)) + ")";
		}
	} else {
		error_string = "Sandbox exception in " + exception_type + ": " +
				String::utf8(msg.data(), msg.size());
	}

	if (vfunc != 0x0) {
		error_string += " in function " + String::utf8(machine.memory.memstring(vfunc).c_str());
	}
	ERR_PRINT(error_string);
	throw std::runtime_error(error_string.utf8().get_data());
}

APICALL(api_array_ops) {
	auto [op, arr_idx, idx, vaddr] = machine.sysargs<Array_Op, unsigned, int, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(50'000); // Costly Array operations.
	SYS_TRACE("array_ops", int(op), arr_idx, idx, vaddr);

	if (op == Array_Op::CREATE) {
		// CREATE reuses arr_idx as the initial element count.
		if (UNLIKELY(arr_idx > MAX_ARRAY_ELEMENTS)) {
			ERR_PRINT("Array::create(): Size is too large: " + itos(arr_idx));
			throw std::runtime_error("Array::create(): Size is too large: " + std::to_string(arr_idx));
		}
		// There is no scoped array, so we need to create one.
		Array a;
		a.resize(arr_idx); // Resize the array to the given size.
		const unsigned idx = emu.create_scoped_variant(Variant(std::move(a)));
		GuestVariant *vp = machine.memory.memarray<GuestVariant>(vaddr, 1);
		vp->type = Variant::ARRAY;
		vp->v.i = idx;
		return;
	}

	const Variant &var_array = get_scoped_variant_or_throw(emu, arr_idx, "Array::operation");
	if (variant_type(var_array) != Variant::ARRAY) {
		ERR_PRINT("Invalid Array object, type = " + String(GuestVariant::type_name(var_array.get_type())));
		throw std::runtime_error("Invalid Array object, idx = " + std::to_string(arr_idx) + " type = " + GuestVariant::type_name(var_array.get_type()));
	}

	switch (op) {
		case Array_Op::FETCH_TO_VECTOR:
		case Array_Op::HAS:
			break;
		default:
			throw_if_read_only(var_array, "Array::operation");
			break;
	}
	godot::Array &array = variant_container<Array>(var_array);

	auto operand = [&]() -> BorrowedVariant {
		return BorrowedVariant(emu, *machine.memory.memarray<GuestVariant>(vaddr, 1));
	};

	switch (op) {
		case Array_Op::PUSH_BACK:
			array.push_back(*operand());
			break;
		case Array_Op::PUSH_FRONT:
			array.push_front(*operand());
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
			array.insert(idx, *operand());
			break;
		case Array_Op::ERASE:
			array.erase(*operand());
			break;
		case Array_Op::RESIZE:
			if (UNLIKELY(idx < 0 || idx > MAX_ARRAY_ELEMENTS)) {
				ERR_PRINT("Array::resize(): Invalid size: " + itos(idx));
				throw std::runtime_error("Array::resize(): Invalid size: " + std::to_string(idx));
			}
			array.resize(idx);
			break;
		case Array_Op::CLEAR:
			array.clear();
			break;
		case Array_Op::SORT:
			array.sort();
			break;
		case Array_Op::FETCH_TO_VECTOR: {
			CppVector<GuestVariant> *vec = machine.memory.memarray<CppVector<GuestVariant>>(vaddr, 1);
			vec->resize(machine, array.size());
			for (int i = 0; i < array.size(); i++) {
				vec->at(machine, i).create(emu, array[i].duplicate(false));
			}
			break;
		}
		case Array_Op::HAS: {
			auto *vp = machine.memory.memarray<GuestVariant>(vaddr, 1);
			const bool result = array.has(*BorrowedVariant(emu, *vp));
			vp->set(emu, result);
			break;
		}
		default:
			ERR_PRINT("Invalid Array operation");
			throw std::runtime_error("Invalid Array operation");
	}
}

APICALL(api_array_at) {
	auto [arr_idx, idx, encoded_vret] = machine.sysargs<unsigned, int64_t, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000); // Costly Array operations.
	SYS_TRACE("array_at", arr_idx, idx, encoded_vret);

	// GuestVariant addresses are naturally aligned. A set keeps the legacy
	// negative-index encoding; a negative constant get tags the otherwise-unused
	// low pointer bit so Godot can wrap the signed index in this same syscall.
	const bool negative_get = (encoded_vret & 1u) != 0;
	GuestVariant *vret = machine.memory.memarray<GuestVariant>(encoded_vret & ~gaddr_t(1), 1);

	const Variant &var_array = get_scoped_variant_or_throw(emu, arr_idx, "Array::at");
	if (variant_type(var_array) != Variant::ARRAY) {
		ERR_PRINT("Invalid Array object, type = " + String(GuestVariant::type_name(var_array.get_type())));
		throw std::runtime_error("Invalid Array object, idx = " + std::to_string(arr_idx) + " type = " + GuestVariant::type_name(var_array.get_type()));
	}

	const bool set_mode = idx < 0 && !negative_get;
	const int64_t index = set_mode ? -int64_t(idx) - 1 : idx;
	GDExtensionBool valid = false;
	GDExtensionBool oob = false;

	if (set_mode) {
		throw_if_read_only(var_array, "Array::at (assignment)");
		const BorrowedVariant value(emu, *vret);
		internal::gdextension_interface_variant_set_indexed(
				const_cast<Variant &>(var_array)._native_ptr(), index,
				value->_native_ptr(), &valid, &oob);
	} else {
		CallResult result;
		internal::gdextension_interface_variant_get_indexed(
				var_array._native_ptr(), index, &result.get(), &valid, &oob);
		result.mark_constructed();
		if (LIKELY(valid && !oob)) {
			vret->create(emu, std::move(result.get()));
		}
	}

	if (UNLIKELY(!valid || oob)) {
		ERR_PRINT("Array index out of bounds: " + itos(index));
		throw std::runtime_error("Array index out of bounds: " + std::to_string(index));
	}
}

APICALL(api_array_size) {
	auto [arr_idx] = machine.sysargs<unsigned>();
	Sandbox &emu = riscv::emu(machine);

	const Variant &var_array = get_scoped_variant_or_throw(emu, arr_idx, "Array::size");
	if (variant_type(var_array) != Variant::ARRAY) {
		ERR_PRINT("Invalid Array object, type = " + String(GuestVariant::type_name(var_array.get_type())));
		throw std::runtime_error("Invalid Array object, idx = " + std::to_string(arr_idx) + " type = " + GuestVariant::type_name(var_array.get_type()));
	}

	machine.set_result(variant_container<Array>(var_array).size());
}

APICALL(api_dict_ops) {
	auto [op, dict_idx, vkey, vaddr] = machine.sysargs<Dictionary_Op, unsigned, gaddr_t, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(50'000); // Costly Dictionary operations.
	SYS_TRACE("dict_ops", int(op), dict_idx, vkey, vaddr);

	struct RawKey {
		gaddr_t pointer;
		gaddr_t length;
	};
	const auto raw_key = [&](gaddr_t pointer, gaddr_t length) {
		const std::string_view text = machine.memory.memview(pointer, length);
		return emu.cached_guest_name(pointer, text, false);
	};

	// MAKE_KEYED creates the Dictionary, so a1 is a destination GuestVariant
	// pointer instead of an existing scoped Dictionary handle.  The key table is
	// an array of guest { pointer, length } pairs and a4 points at the values.
	if (op == Dictionary_Op::MAKE_KEYED) {
		const size_t count = size_t(vaddr);
		const RawKey *keys = machine.memory.memarray<RawKey>(vkey, count);
		const gaddr_t values_addr = machine.cpu.reg(14); // A4
		const GuestVariant *values = machine.memory.memarray<GuestVariant>(values_addr, count);
		GuestVariant *destination = machine.memory.memarray<GuestVariant>(gaddr_t(dict_idx), 1);
		Dictionary dict;
		for (size_t i = 0; i < count; i++) {
			auto key = raw_key(keys[i].pointer, keys[i].length);
			Variant *slot = reinterpret_cast<Variant *>(
					internal::gdextension_interface_dictionary_operator_index(
						dict._native_ptr(), key->variant._native_ptr()));
			store_guest_variant(*slot, values[i], emu);
		}
		destination->create(emu, std::move(dict));
		return;
	}
	const Variant &var_dict = get_scoped_variant_or_throw(emu, dict_idx, "Dictionary::operation");
	if (variant_type(var_dict) != Variant::DICTIONARY) {
		ERR_PRINT("Invalid Dictionary object, type = " + String(GuestVariant::type_name(var_dict.get_type())));
		throw std::runtime_error("Invalid Dictionary object, idx = " + std::to_string(dict_idx) + " type = " + GuestVariant::type_name(var_dict.get_type()));
	}

	switch (op) {
		case Dictionary_Op::SET:
		case Dictionary_Op::SET_RAW:
		case Dictionary_Op::ERASE:
		case Dictionary_Op::CLEAR:
		case Dictionary_Op::MERGE:
		case Dictionary_Op::GET_OR_ADD:
			throw_if_read_only(var_dict, "Dictionary::operation");
			break;
		default:
			break;
	}

	if (op == Dictionary_Op::GET_RAW || op == Dictionary_Op::SET_RAW ||
			op == Dictionary_Op::HAS_RAW) {
		auto key = raw_key(vkey, vaddr);
		godot::Dictionary &dict = variant_container<Dictionary>(var_dict);
		if (op == Dictionary_Op::HAS_RAW) {
			machine.set_result(dict.has(key->sname));
			return;
		}
		const gaddr_t value_addr = machine.cpu.reg(14); // A4
		GuestVariant *value = machine.memory.memarray<GuestVariant>(value_addr, 1);
		if (op == Dictionary_Op::GET_RAW) {
			CallResult answer;
			GDExtensionBool valid = false;
			internal::gdextension_interface_variant_get_keyed(
					var_dict._native_ptr(), key->variant._native_ptr(), &answer.get(), &valid);
			answer.mark_constructed();
			if (LIKELY(valid)) {
				value->create(emu, std::move(answer.get()));
			} else {
				value->type = Variant::NIL;
				value->v.i = 0;
			}
		} else {
			Variant *slot = reinterpret_cast<Variant *>(
					internal::gdextension_interface_dictionary_operator_index(
						dict._native_ptr(), key->variant._native_ptr()));
			store_guest_variant(*slot, *value, emu);
		}
		return;
	}

	if (op == Dictionary_Op::HAS_EXACT_KEYS) {
		const size_t count = size_t(vaddr);
		const RawKey *keys = machine.memory.memarray<RawKey>(vkey, count);
		const godot::Dictionary &dict = variant_container<Dictionary>(var_dict);
		bool exact = size_t(dict.size()) == count;
		for (size_t i = 0; exact && i < count; i++) {
			auto key = raw_key(keys[i].pointer, keys[i].length);
			exact = dict.has(key->sname);
		}
		machine.set_result(exact);
		return;
	}

	switch (op) {
		case Dictionary_Op::GET: {
			GuestVariant *key = machine.memory.memarray<GuestVariant>(vkey, 1);
			GuestVariant *vp = machine.memory.memarray<GuestVariant>(vaddr, 1);
			const BorrowedVariant key_variant(emu, *key);
			CallResult value;
			GDExtensionBool valid = false;
			internal::gdextension_interface_variant_get_keyed(
					var_dict._native_ptr(), key_variant->_native_ptr(), &value.get(), &valid);
			value.mark_constructed();
			if (LIKELY(valid)) {
				vp->create(emu, std::move(value.get()));
			} else {
				vp->type = Variant::NIL;
				vp->v.i = 0;
			}
			return;
		}
		case Dictionary_Op::SET: {
			GuestVariant *key = machine.memory.memarray<GuestVariant>(vkey, 1);
			GuestVariant *value = machine.memory.memarray<GuestVariant>(vaddr, 1);
			const BorrowedVariant key_variant(emu, *key);
			const BorrowedVariant value_variant(emu, *value);
			GDExtensionBool valid = false;
			internal::gdextension_interface_variant_set_keyed(
					const_cast<Variant &>(var_dict)._native_ptr(), key_variant->_native_ptr(),
					value_variant->_native_ptr(), &valid);
			if (UNLIKELY(!valid)) {
				ERR_PRINT("Dictionary::set(): the key could not be assigned");
				throw std::runtime_error("Dictionary::set(): the key could not be assigned");
			}
			return;
		}
		default:
			break;
	}

	godot::Dictionary &dict = variant_container<Dictionary>(var_dict);

	switch (op) {
		case Dictionary_Op::GET:
		case Dictionary_Op::SET:
			break; // Handled above
		case Dictionary_Op::ERASE: {
			GuestVariant *key = machine.memory.memarray<GuestVariant>(vkey, 1);
			dict.erase(*BorrowedVariant(emu, *key));
			break;
		}
		case Dictionary_Op::HAS: {
			GuestVariant *key = machine.memory.memarray<GuestVariant>(vkey, 1);
			machine.set_result(dict.has(*BorrowedVariant(emu, *key)));
			break;
		}
		case Dictionary_Op::GET_KEYS: {
			GuestVariant *vp = machine.memory.memarray<GuestVariant>(vkey, 1);
			vp->create(emu, dict.keys());
			break;
		}
		case Dictionary_Op::GET_VALUES: {
			GuestVariant *vp = machine.memory.memarray<GuestVariant>(vkey, 1);
			vp->create(emu, dict.values());
			break;
		}
		case Dictionary_Op::GET_SIZE:
			machine.set_result(dict.size());
			break;
		case Dictionary_Op::CLEAR:
			dict.clear();
			break;
		case Dictionary_Op::MERGE: {
			GuestVariant *other_dict = machine.memory.memarray<GuestVariant>(vkey, 1);
			dict.merge(other_dict->toVariant(emu).operator Dictionary());
			break;
		}
		case Dictionary_Op::GET_OR_ADD: {
			GuestVariant *key = machine.memory.memarray<GuestVariant>(vkey, 1);
			GuestVariant *vp = machine.memory.memarray<GuestVariant>(vaddr, 1);
			Variant &v = dict[*BorrowedVariant(emu, *key)];
			if (v.get_type() == Variant::NIL) {
				const gaddr_t vdefaddr = machine.cpu.reg(14); // A4
				const GuestVariant *vdef = machine.memory.memarray<GuestVariant>(vdefaddr, 1);
				v = vdef->toVariant(emu);
			}
			// A copy, not a reference: Dictionary::operator[] hands back a pointer into the
			// Dictionary's own storage, and scoping that pointer would leave the guest holding
			// a dangling index the moment it erases the key or clears the Dictionary.
			vp->create(emu, Variant(v));
			break;
		}
		case Dictionary_Op::HAS_EXACT_KEYS:
		case Dictionary_Op::GET_RAW:
		case Dictionary_Op::SET_RAW:
		case Dictionary_Op::HAS_RAW:
		case Dictionary_Op::MAKE_KEYED:
			break; // Handled before this switch.
		default:
			ERR_PRINT("Invalid Dictionary operation");
			throw std::runtime_error("Invalid Dictionary operation");
	}
}

APICALL(api_string_create) {
	auto [strview] = machine.sysargs<std::string_view>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("string_create", String::utf8(strview.data(), strview.size()));

	String str = String::utf8(strview.data(), strview.size());
	const unsigned idx = emu.create_scoped_variant(Variant(std::move(str)));
	machine.set_result(idx);
}

// Preserve the guest's original Variant type (StringName, NodePath) after mutation.
static Variant string_variant_of_type(Variant::Type type, godot::String &&str) {
	switch (type) {
		case Variant::STRING_NAME:
			return Variant(StringName(str));
		case Variant::NODE_PATH:
			return Variant(NodePath(str));
		default:
			return Variant(std::move(str));
	}
}

APICALL(api_string_ops) {
	auto [op, str_idx, index, vaddr] = machine.sysargs<String_Op, unsigned, int, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000); // Costly String operations.
	SYS_TRACE("string_ops", int(op), str_idx, index, vaddr);

	const Variant &var_str = get_scoped_variant_or_throw(emu, str_idx, "String::operation");
	const Variant::Type type = variant_type(var_str);
	if (type != Variant::STRING && type != Variant::STRING_NAME && type != Variant::NODE_PATH) {
		ERR_PRINT("Invalid String object type: " + String(GuestVariant::type_name(type)));
		throw std::runtime_error("Invalid String object type: " + std::string(GuestVariant::type_name(type)));
	}
	godot::String str = var_str.operator String();

	switch (op) {
		case String_Op::APPEND: {
			const unsigned *vother = machine.memory.memarray<const unsigned>(vaddr, 1);
			str += get_scoped_variant_or_throw(emu, *vother, "String::append").operator String();
			// str is a detached copy; assign back through a mutable slot.
			emu.get_mutable_scoped_variant(str_idx) = string_variant_of_type(type, std::move(str));
			break;
		}
		case String_Op::GET_LENGTH:
			machine.set_result(str.length());
			break;
		case String_Op::TO_STD_STRING: {
			if (index == 0) { // Get the string as a std::string.
				CharString utf8 = str.utf8();
				CppString *gstr = machine.memory.memarray<CppString>(vaddr, 1);
				gstr->set_string(machine, vaddr, utf8.ptr(), utf8.length());
			} else if (index == 1) { // Fill a guest-provided char*, size_t buffer.
				struct Buffer {
					gaddr_t ptr;
					gaddr_t size;
				} *buffer = machine.memory.memarray<Buffer>(vaddr, 1);
				CharString utf8 = str.utf8();
				const size_t size = utf8.length();
				const size_t copied = std::min<size_t>(buffer->size, size);
				if (copied > 0)
					machine.memory.memcpy(buffer->ptr, utf8.ptr(), copied);
				// The reported size is always the real length of the string
				buffer->size = size;
			} else if (index == 2) { // Get the string as a std::u32string.
				GuestStdU32String *gstr = machine.memory.memarray<GuestStdU32String>(vaddr, 1);
				gstr->set_string(machine, vaddr, str.ptr(), str.length());
			} else {
				ERR_PRINT("Invalid String conversion");
				throw std::runtime_error("Invalid String conversion");
			}
			break;
		}
		case String_Op::COMPARE: {
			unsigned *vother = machine.memory.memarray<unsigned>(vaddr, 1);
			const Variant &other = get_scoped_variant_or_throw(emu, *vother, "String::compare");
			machine.set_result(str == other.operator String());
			break;
		}
		case String_Op::COMPARE_CSTR: {
			const std::string vother = machine.memory.memstring(vaddr);
			machine.set_result(str == String::utf8(vother.c_str(), vother.size()));
			break;
		}
		default:
			ERR_PRINT("Invalid String operation");
			throw std::runtime_error("Invalid String operation");
	}
}

APICALL(api_string_at) {
	auto [str_idx, index] = machine.sysargs<unsigned, int>();
	Sandbox &emu = riscv::emu(machine);
	SYS_TRACE("string_at", str_idx, index);

	const Variant &var_str = get_scoped_variant_or_throw(emu, str_idx, "String::at");
	const Variant::Type type = variant_type(var_str);
	if (type != Variant::STRING) {
		ERR_PRINT("Invalid String object, type = " + String(GuestVariant::type_name(type)));
		throw std::runtime_error("Invalid String object, idx = " + std::to_string(str_idx) + " type = " + GuestVariant::type_name(type));
	}

	// Indexing the Variant hands back the one-character String already boxed,
	// negative indices wrapped and the bound checked -- one engine call where
	// unboxing the String, indexing it and calling String::chr() is four. This
	// is the per-character cost of `for c in text`, so the difference shows.
	CallResult result;
	GDExtensionBool valid = false;
	GDExtensionBool oob = false;
	internal::gdextension_interface_variant_get_indexed(
			var_str._native_ptr(), index, &result.get(), &valid, &oob);
	result.mark_constructed();
	if (UNLIKELY(!valid || oob)) {
		ERR_PRINT("String index out of bounds");
		throw std::runtime_error("String index out of bounds");
	}

	machine.set_result(emu.create_scoped_variant(std::move(result.get())));
}

// The per-character cost of `for c in text`. One call fills a run of scoped
// slots, so the walk's ecall count is its length divided by the batch size
// rather than its length.
APICALL(api_string_batch) {
	auto [str_idx, start, max_count] = machine.sysargs<unsigned, int64_t, unsigned>();
	Sandbox &emu = riscv::emu(machine);
	SYS_TRACE("string_batch", str_idx, start, max_count);

	const Variant &var_str = get_scoped_variant_or_throw(emu, str_idx, "String::batch");
	const Variant::Type type = variant_type(var_str);
	if (type != Variant::STRING) {
		ERR_PRINT("Invalid String object, type = " + String(GuestVariant::type_name(type)));
		throw std::runtime_error("Invalid String object, idx = " + std::to_string(str_idx) + " type = " + GuestVariant::type_name(type));
	}

	const int64_t length = var_str.operator String().length();
	if (start < 0 || start >= length || max_count == 0) {
		machine.set_result(0);
		return;
	}
	int64_t count = std::min<int64_t>(max_count, length - start);

	// The batch is released when it runs out, not once per character, so the
	// loop body allocates against what is left here. Take a quarter of it.
	const Sandbox::CurrentState &st = emu.state();
	const int64_t headroom = int64_t(st.variants.capacity()) - int64_t(st.scoped_variants.size());
	count = std::min(count, std::max<int64_t>(1, headroom / 4));

	// Outside a vmcall the slots are permanent ones, handed out by a scheme that
	// does not promise consecutive indices. One character is always safe.
	if (!emu.is_in_vmcall()) {
		count = 1;
	}

	int32_t base = 0;
	for (int64_t i = 0; i < count; i++) {
		CallResult result;
		GDExtensionBool valid = false;
		GDExtensionBool oob = false;
		internal::gdextension_interface_variant_get_indexed(
				var_str._native_ptr(), start + i, &result.get(), &valid, &oob);
		result.mark_constructed();
		if (UNLIKELY(!valid || oob)) {
			ERR_PRINT("String index out of bounds: " + itos(start + i));
			throw std::runtime_error("String index out of bounds");
		}
		const int32_t idx = int32_t(emu.create_scoped_variant(std::move(result.get())));
		if (i == 0) {
			base = idx;
		} else if (idx != base + int32_t(i)) {
			// Never seen in a vmcall, but the guest indexes by base + n.
			count = i;
			break;
		}
	}
	machine.set_result((uint64_t(uint32_t(base)) << 32) | uint64_t(uint32_t(count)));
}

APICALL(api_string_size) {
	auto [str_idx] = machine.sysargs<unsigned>();
	Sandbox &emu = riscv::emu(machine);
	SYS_TRACE("string_size", str_idx);

	const Variant &var_str = get_scoped_variant_or_throw(emu, str_idx, "String::size");
	// String, StringName and NodePath all answer length().
	const Variant::Type type = variant_type(var_str);
	if (type != Variant::STRING && type != Variant::STRING_NAME && type != Variant::NODE_PATH) {
		ERR_PRINT("Invalid String object, type = " + String(GuestVariant::type_name(type)));
		throw std::runtime_error("Invalid String object, idx = " + std::to_string(str_idx) + " type = " + GuestVariant::type_name(type));
	}
	godot::String str = var_str.operator String();
	machine.set_result(str.length());
}

APICALL(api_string_append) {
	auto [str_idx, strview] = machine.sysargs<unsigned, std::string_view>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("string_append", str_idx, String::utf8(strview.data(), strview.size()));

	Variant &var = emu.get_mutable_scoped_variant(str_idx);

	const Variant::Type type = variant_type(var);
	godot::String str = var.operator String();
	str += String::utf8(strview.data(), strview.size());
	var = string_variant_of_type(type, std::move(str));
}

APICALL(api_timer_periodic) {
	auto [interval, oneshot, callback, capture, vret] = machine.sysargs<double, bool, gaddr_t, std::array<uint8_t, 32> *, GuestVariant *>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(100'000); // Costly Timer node creation.
	SYS_TRACE("timer_periodic", interval, oneshot, callback, capture, vret);

	// This instantiates a node and adds it to the scene tree, so it answers to the same
	// class restriction as any other way of creating one.
	if (UNLIKELY(!emu.is_allowed_class("Timer"))) {
		ERR_PRINT("Class name is not allowed: Timer");
		throw std::runtime_error("Class name is not allowed: Timer");
	}

	Timer *timer = memnew(Timer);
	timer->set_wait_time(interval);
	timer->set_one_shot(oneshot);
	Node *topnode = emu.get_tree_base();
	// Add the timer to the top node, as long as the Sandbox is in a tree.
	if (topnode != nullptr) {
		topnode->add_child(timer);
		timer->set_owner(topnode);
		if (topnode->is_inside_tree()) {
			timer->start();
		} else {
			// A Timer can only be started once it's in the scene tree, so let it
			// start itself when the tree base enters the tree.
			timer->set_autostart(true);
		}
	} else {
		timer->set_autostart(true);
	}
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
	vret->set_object(emu, timer);
}

APICALL(api_timer_stop) {
	throw std::runtime_error("timer_stop: Not implemented");
}

APICALL(api_callable_create) {
	auto [address, vargs, reserved, flags] = machine.sysargs<gaddr_t, GuestVariant *, gaddr_t, gaddr_t>();
	Sandbox &emu = riscv::emu(machine);
	SYS_TRACE("callable_create", address, vargs);
	(void)reserved;

	if (address == 0) {
		auto idx = emu.create_scoped_variant(Variant(Callable()));
		machine.set_result(idx);
		return;
	}

	// Create a new callable object, using emu.vmcallable_address() to get the callable function.
	Array arguments;
	if (vargs->type != Variant::NIL) {
		// The argument idx is a Variant of another type.
		arguments.push_back(vargs->toVariant(emu));
	}
	RiscvCallable *custom = memnew(RiscvCallable);
	custom->init(&emu, address, std::move(arguments),
			(flags & ECALL_CALLABLE_VARIANT_ARGS) != 0);
	Callable callable(custom);

	// Return the callable object to the guest.
	auto idx = emu.create_scoped_variant(Variant(std::move(callable)));
	machine.set_result(idx);
}

APICALL(api_load) {
	Sandbox &emu = riscv::emu(machine);
	const gaddr_t g_path = machine.cpu.reg(10); // A0
	const gaddr_t path_len = machine.cpu.reg(11); // A1
	GuestVariant *g_result = machine.memory.memarray<GuestVariant>(machine.cpu.reg(12), 1); // A2

	// A1 = ECALL_LOAD_PATH_IS_VARIANT: A0 is a Variant, not a character buffer.
	String godot_path;
	if (path_len == ECALL_LOAD_PATH_IS_VARIANT) {
		const GuestVariant *g_var = machine.memory.memarray<GuestVariant>(g_path, 1);
		switch (g_var->type) {
			case Variant::STRING:
			case Variant::STRING_NAME:
			case Variant::NODE_PATH:
				godot_path = g_var->toVariant(emu).operator String();
				break;
			default:
				ERR_PRINT("Resource path is not a string");
				throw std::runtime_error("Resource path is not a string");
		}
	} else {
		const std::string_view path = machine.memory.memview(g_path, path_len);
		godot_path = String::utf8(path.data(), path.size());
	}
	SYS_TRACE("load", godot_path, g_result);

	// Check if the path is allowed.
	if (!emu.is_allowed_resource(godot_path)) {
		ERR_PRINT("Resource path is not allowed: " + godot_path);
		throw std::runtime_error("Resource path is not allowed: " + std::string(godot_path.utf8().get_data()));
	}

	// Preload the resource from the given path.
	ResourceLoader *loader = ResourceLoader::get_singleton();
	Ref<Resource> resource = loader->load(godot_path);
	if (resource.is_null()) {
		ERR_PRINT("Failed to preload resource");
		// TODO: Return a null object instead?
		throw std::runtime_error("Failed to preload resource");
	}

	Variant result(std::move(resource));
	godot::Object *obj = result.operator Object *();

	// Return the result to the guest.
	emu.create_scoped_variant(std::move(result));
	g_result->set_object(emu, obj);
}

// GDScript's subscript operator is a Variant operation. Godot decides whether
// the key is indexed, keyed, or named; duplicating that type matrix here would
// inevitably diverge as built-in types evolve.
APICALL(api_variant_get) {
	auto [g_subject, g_key, vret] =
			machine.sysargs<const GuestVariant *, const GuestVariant *, GuestVariant *>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(150'000);
	SYS_TRACE("variant_get", g_subject, g_key, vret);

	const BorrowedVariant subject(emu, *g_subject);
	const BorrowedVariant key(emu, *g_key);

	// Object subscripting is property access and must retain the same sandbox
	// boundary as the named-property syscall.
	if (UNLIKELY(variant_type(*subject) == Variant::OBJECT)) {
		godot::Object *obj = subject->operator godot::Object *();
		if (UNLIKELY(!emu.is_allowed_property(obj, *key, false))) {
			ERR_PRINT("Banned property accessed through Variant index");
			throw std::runtime_error("Banned property accessed through Variant index");
		}
	}

	CallResult result;
	GDExtensionBool valid = false;
	internal::gdextension_interface_variant_get(
			subject->_native_ptr(), key->_native_ptr(), &result.get(), &valid);
	result.mark_constructed();
	if (UNLIKELY(!valid)) {
		const std::string subject_type = GuestVariant::type_name(variant_type(*subject));
		const std::string key_type = GuestVariant::type_name(variant_type(*key));
		ERR_PRINT(("Invalid indexed access on " + subject_type + " with key type " + key_type).c_str());
		throw std::runtime_error(
				"Invalid indexed access on " + subject_type + " with key type " + key_type);
	}

	vret->create(emu, std::move(result.get()));
}

APICALL(api_sandbox_add) {
	// Add a new sandboxed property or public API method to the sandbox.
	Sandbox &emu = riscv::emu(machine);
	if (!emu.is_initializing()) {
		ERR_PRINT("Sandbox add called outside of initialization");
		throw std::runtime_error("Sandbox add called outside of initialization");
	}
	PENALIZE(100'000); // Costly Sandbox operations.
	// Check which operation it is.
	int method = machine.cpu.reg(10); // A0
	switch (method) {
		case 0: {
			// Add a new sandboxed property.
			auto [method, name, type, setter, getter, defval] = machine.sysargs<int, std::string_view, int32_t, gaddr_t, gaddr_t, GuestVariant *>();
			String utf8_name = String::utf8(name.data(), name.size());
			SYS_TRACE("sandbox_add", "property", utf8_name, int(type), setter, getter, defval->toVariant(emu));
			if (type < 0) {
				ERR_PRINT("Invalid property type for sandbox property: " + itos(type));
				throw std::runtime_error("Invalid property type for sandbox property");
			}
			if (type >= Variant::VARIANT_MAX) {
				ERR_PRINT("Invalid property type for sandbox property: " + itos(type));
				throw std::runtime_error("Invalid property type for sandbox property: " + std::to_string(type));
			}
			// In range as of the two checks above, so it may become the enum here.
			const Variant::Type property_type = static_cast<Variant::Type>(type);
			if (getter == 0 && setter == 0) {
				// Treat as a guest-side variable, where the "default" value is
				// the address of the Variant, and the current value is the default.
				const gaddr_t address = machine.cpu.reg(REG_ARG6);
				emu.add_property(utf8_name, property_type, address, defval->toVariant(emu));
			} else {
				// If the setter is zero, it is a read-only property.
				emu.add_property(utf8_name, property_type, setter, getter, defval->toVariant(emu));
			}
			break;
		}
		case 1: {
			// Add a new sandboxed public API method. Name, address, description, return type and arguments.
			struct GuestFunctionExtra {
				gaddr_t desc;
				gaddr_t desc_len;
				gaddr_t ret;
				gaddr_t ret_len;
				gaddr_t args;
				gaddr_t args_len;
			};
			auto [method, name, address, g_extra] = machine.sysargs<int, std::string_view, gaddr_t, GuestFunctionExtra *>();
			SYS_TRACE("sandbox_add", "method", String::utf8(name.data(), name.size()));
			// Get the description, return type and arguments. We have a limited amount of registers,
			// so we will use zero-terminated strings for the description and return type.
			std::string_view description = machine.memory.memview(g_extra->desc, g_extra->desc_len);
			std::string_view return_type = machine.memory.memview(g_extra->ret, g_extra->ret_len);
			std::string_view arguments = machine.memory.memview(g_extra->args, g_extra->args_len);
			// Sandbox owns the list; ELFScript receives a copy after init.
			Dictionary func = Sandbox::create_public_api_function(name, address, description, return_type, arguments);
			if (func.size() > 0) {
				emu.add_public_api_function(std::move(func));
			} else {
				// Malformed signature; cache the address so the name is still callable.
				emu.add_cached_address(String::utf8(name.data(), name.size()), address);
			}
		} break;
		case 3: {
			auto [method, name, hint, hint_string, usage] =
				machine.sysargs<int, std::string_view, uint32_t, std::string_view, uint32_t>();
			const String utf8_name = String::utf8(name.data(), name.size());
			const String utf8_hint = String::utf8(hint_string.data(), hint_string.size());
			SYS_TRACE("sandbox_add", "hint", utf8_name, int(hint), utf8_hint, int(usage));
			emu.set_property_hint(utf8_name, hint, utf8_hint, usage);
			break;
		}
		case 2: { // Set new exit address.
			SYS_TRACE("sandbox_add", "exit", machine.cpu.reg(11));
			const gaddr_t exit_address = machine.cpu.reg(11); // A1
			if (exit_address == 0 || (exit_address & 0x1) != 0) {
				ERR_PRINT("Invalid program exit address");
				throw std::runtime_error("Invalid program exit address");
			}
			const auto &exec = emu.machine().memory.exec_segment_for(exit_address);
			if (!exec->is_within(exit_address)) {
				ERR_PRINT("Invalid program exit address");
				throw std::runtime_error("Invalid program exit address");
			}
			emu.machine().memory.set_exit_address(exit_address);
			break;
		}
		default:
			WARN_PRINT("Unhandled sandbox add method: " + itos(method));
	}
}

template <typename T, typename PA>
static PA createPackedArrayFromGuestArray(Sandbox &emu, const Array& array)
{
	const size_t size = array.size();
	PA packed_array;
	packed_array.resize(size);
	for (int i = 0; i < size; i++) {
		if constexpr (std::is_same_v<T, godot::String>) {
			packed_array[i] = array[i].operator String();
		} else {
			packed_array[i] = static_cast<T>(array[i]);
		}
	}
	return packed_array;
}

APICALL(api_packed_array_ops)
{
	auto [op] = machine.sysargs<int>();
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(50'000); // Costly PackedArray operations.
	SYS_TRACE("packed_array_ops", int(op), arr_idx, idx, vaddr);

	// CREATE_FROM_ARRAY (op="Any Packed*Array Variant type, starting at 29")
	switch (op) {
	case Variant::PACKED_BYTE_ARRAY:
	case Variant::PACKED_INT32_ARRAY:
	case Variant::PACKED_INT64_ARRAY:
	case Variant::PACKED_FLOAT32_ARRAY:
	case Variant::PACKED_FLOAT64_ARRAY:
	case Variant::PACKED_VECTOR2_ARRAY:
	case Variant::PACKED_VECTOR3_ARRAY:
	case Variant::PACKED_VECTOR4_ARRAY:
	case Variant::PACKED_COLOR_ARRAY:
	case Variant::PACKED_STRING_ARRAY:
	{
		// Create a Packed*Array from an Array.
		auto [unused_op, result_ptr, arr_ptr] = machine.sysargs<int, gaddr_t, int>();
		// This is a scoped Array type GuestVariant.
		GuestVariant *garray = machine.memory.memarray<GuestVariant>(arr_ptr, 1);
		if (garray->type != Variant::ARRAY) {
			ERR_PRINT("Invalid Array object for PackedArray creation");
			throw std::runtime_error("Invalid Array object for PackedArray creation");
		}
		godot::Array array = get_scoped_variant_or_throw(emu, garray->v.i, "PackedArray creation").operator Array();
		GuestVariant *gres = machine.memory.memarray<GuestVariant>(result_ptr, 1);
		switch (op) {
			case Variant::PACKED_BYTE_ARRAY: {
				PackedByteArray packed_array = createPackedArrayFromGuestArray<uint8_t, PackedByteArray>(emu, array);
				gres->create(emu, std::move(packed_array));
				break;
			}
			case Variant::PACKED_INT32_ARRAY: {
				PackedInt32Array packed_array = createPackedArrayFromGuestArray<int32_t, PackedInt32Array>(emu, array);
				gres->create(emu, std::move(packed_array));
				break;
			}
			case Variant::PACKED_INT64_ARRAY: {
				PackedInt64Array packed_array = createPackedArrayFromGuestArray<int64_t, PackedInt64Array>(emu, array);
				gres->create(emu, std::move(packed_array));
				break;
			}
			case Variant::PACKED_FLOAT32_ARRAY: {
				PackedFloat32Array packed_array = createPackedArrayFromGuestArray<float, PackedFloat32Array>(emu, array);
				gres->create(emu, std::move(packed_array));
				break;
			}
			case Variant::PACKED_FLOAT64_ARRAY: {
				PackedFloat64Array packed_array = createPackedArrayFromGuestArray<double, PackedFloat64Array>(emu, array);
				gres->create(emu, std::move(packed_array));
				break;
			}
			case Variant::PACKED_VECTOR2_ARRAY: {
				PackedVector2Array packed_array = createPackedArrayFromGuestArray<godot::Vector2, PackedVector2Array>(emu, array);
				gres->create(emu, std::move(packed_array));
				break;
			}
			case Variant::PACKED_VECTOR3_ARRAY: {
				PackedVector3Array packed_array = createPackedArrayFromGuestArray<godot::Vector3, PackedVector3Array>(emu, array);
				gres->create(emu, std::move(packed_array));
				break;
			}
			case Variant::PACKED_VECTOR4_ARRAY: {
				PackedVector4Array packed_array = createPackedArrayFromGuestArray<godot::Vector4, PackedVector4Array>(emu, array);
				gres->create(emu, std::move(packed_array));
				break;
			}
			case Variant::PACKED_COLOR_ARRAY: {
				PackedColorArray packed_array = createPackedArrayFromGuestArray<godot::Color, PackedColorArray>(emu, array);
				gres->create(emu, std::move(packed_array));
				break;
			}
			case Variant::PACKED_STRING_ARRAY: {
				PackedStringArray packed_array = createPackedArrayFromGuestArray<godot::String, PackedStringArray>(emu, array);
				gres->create(emu, std::move(packed_array));
				break;
			}
			default:
				ERR_PRINT("Invalid PackedArray type for creation: " + itos(gres->type));
				throw std::runtime_error("Invalid PackedArray type for creation: " + std::to_string(gres->type));
		}
		return;
	}
	default:
		// Unknown operation
		ERR_PRINT("Invalid PackedArray operation");
		throw std::runtime_error("Invalid PackedArray operation");
	}
}

} //namespace riscv

void Sandbox::initialize_syscalls_runtime() {
	using namespace riscv;

	// Initialize common Linux system calls
	machine().setup_linux_syscalls(false, false);
	// Initialize POSIX threads
	machine().setup_posix_threads();

	machine().on_unhandled_syscall = [](machine_t &machine, size_t syscall) {
#if defined(__linux__) // We only want to print these kinds of warnings on Linux.
		WARN_PRINT(("Unhandled system call: " + std::to_string(syscall)).c_str());
		auto &emu = riscv::emu(machine);
		PENALIZE(100'000); // Add to the instruction counter due to I/O.
#endif
		machine.set_result(-ENOSYS);
	};
}

void Sandbox::initialize_syscalls() {
	using namespace riscv;

	// Resolve Object::call() once, instead of going through a function-local static
	// (and its thread-safe initialization guard) on every single object call.
	riscv::object_call_mtd = internal::gdextension_interface_classdb_get_method_bind(
			Object::get_class_static()._native_ptr(), StringName("call")._native_ptr(), 3400424181);

	// Add the Godot system calls.
	machine_t::install_syscall_handlers({
			{ ECALL_PRINT, api_print },
			{ ECALL_PRINT_CHANNEL, api_print_channel },
			{ ECALL_VCALL, api_vcall },
			{ ECALL_VEVAL, api_veval },
			{ ECALL_VASSIGN, api_vassign },
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

			{ ECALL_VCREATE, api_vcreate },
			{ ECALL_VCONSTRUCT, api_vconstruct },
			{ ECALL_VSTORE_GLOBAL, api_vstore_global },
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
			{ ECALL_STRING_BATCH, api_string_batch },
			{ ECALL_STRING_APPEND, api_string_append },

			{ ECALL_TIMER_PERIODIC, api_timer_periodic },
			{ ECALL_TIMER_STOP, api_timer_stop },

			{ ECALL_NODE_CREATE, api_node_create },

			{ ECALL_CALLABLE_CREATE, api_callable_create },

			{ ECALL_LOAD, api_load },

			{ ECALL_OBJ_PROP_GET, api_obj_property_get },
			{ ECALL_OBJ_PROP_SET, api_obj_property_set },
			{ ECALL_VARIANT_GET, api_variant_get },

			{ ECALL_SANDBOX_ADD, api_sandbox_add },

			{ ECALL_CALL_GUEST, api_call_guest },

			{ ECALL_VSCOPE, api_vscope },
			{ ECALL_OBJ_RETAIN, api_obj_retain },

			{ ECALL_CLASS_BIND, api_class_bind },
			{ ECALL_OBJ_USES_TRAIT, api_obj_uses_trait },
			{ ECALL_VCALL_SUPER, api_vcall_super },

			{ ECALL_PACKED_ARRAY_OPS, api_packed_array_ops },

			{ ECALL_UTILITY, api_utility },

			{ ECALL_BREAKPOINT, api_breakpoint },

			{ ECALL_AWAIT, api_await },
			{ ECALL_AWAIT_RESTORE, api_await_restore },
	});

	// Add system calls from other modules.
	Sandbox::initialize_syscalls_2d();
	Sandbox::initialize_syscalls_3d();

	using namespace riscv;
	static const Instruction<RISCV_ARCH> validated_syscall_instruction {
		[](CPU<RISCV_ARCH>& cpu, rv32i_instruction instr) {
			Machine<RISCV_ARCH>::syscall_handlers[instr.Itype.imm](cpu.machine());
		},
		[](char* buffer, size_t len, const CPU<RISCV_ARCH>&, rv32i_instruction instr) -> int {
			return snprintf(buffer, len,
				"DYNCALL: 4-byte idx=0x%X (inline, 0x%X)",
				uint32_t(instr.Itype.imm),
				instr.whole
			);
		}};
	// Override the machines unimplemented instruction handling,
	// in order to use the custom instruction instead.
	CPU<RISCV_ARCH>::on_unimplemented_instruction
		= [](rv32i_instruction instr) -> const Instruction<RISCV_ARCH>& {
		if (instr.opcode() == 0b1011011 && instr.Itype.rs1 == 0 && instr.Itype.rd == 0) {
			if (instr.Itype.imm < Machine<RISCV_ARCH>::syscall_handlers.size()) {
				return validated_syscall_instruction;
			}
		}
		return CPU<RISCV_ARCH>::get_unimplemented_instruction();
	};
}
