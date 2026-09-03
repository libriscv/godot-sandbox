#include "../guest_datatypes.h"
#include "../syscalls.h"

#include <godot_cpp/classes/engine.hpp>
#include <cstring>
#include <iterator>
#include <map>
#include <random>

/**
 * @brief A light-weight fuzzer for the host side of the sandbox API.
 *
 * The guest can put anything it likes in a0-a7 and fa0-fa7 before an ecall, so every
 * system call handler is really a parser of eight attacker-controlled machine words.
 * This drives those handlers directly, with argument values chosen to get past the
 * first bounds check rather than uniformly at random, and reports how it went.
 *
 * The sandbox is fuzzed with *all* restrictions enabled and no user callbacks
 * installed, which means every allowed-class/method/property/resource question is
 * answered "no". Anything that still reaches Godot from here is a restriction bypass.
 */

namespace {

// A handler handed eight uniformly random 64-bit words bails out on its first bounds
// check essentially every time. These are the values that get further: enum-sized
// integers, the edges of every integer width, and the indices the scoped-Variant
// tables actually use.
static constexpr uint64_t INTERESTING[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
	23, 24, 29, 31, 32, 38, 39, 40, 63, 64,
	0x7F, 0x80, 0xFF, 0x100, 0x3FF, 0x400, 0x7FF, 0x800, 0xFFFF, 0x10000,
	0xFFFFFF, 0x1000000, 0x1000001,
	0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000ull,
	0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull,
	uint64_t(int64_t(-1)), uint64_t(int64_t(-2)), uint64_t(int64_t(-3)), uint64_t(int64_t(-16)),
	uint64_t(int64_t(INT32_MIN)), uint64_t(int64_t(INT32_MIN) + 1),
};

// Bit patterns, not values: the point is to reach the classification branches inside
// Godot's math, which NaN and the infinities take differently from ordinary floats.
static constexpr uint64_t INTERESTING_FLOATS[] = {
	0x0000000000000000ull, // +0.0
	0x8000000000000000ull, // -0.0
	0x3FF0000000000000ull, // 1.0
	0xBFF0000000000000ull, // -1.0
	0x7FF0000000000000ull, // +inf
	0xFFF0000000000000ull, // -inf
	0x7FF8000000000000ull, // quiet NaN
	0x7FEFFFFFFFFFFFFFull, // DBL_MAX
	0x0010000000000000ull, // DBL_MIN
	0x0000000000000001ull, // smallest denormal
	0x4630000000000000ull, // 2^100
	0xC630000000000000ull, // -2^100
};

/// @brief What a system call expects to find in one argument register.
///
/// Every handler validates its arguments before it does anything, and several of them
/// validate three at once: api_vcall wants a Variant pointer, a name, and the length of
/// that name, all agreeing. Registers filled independently satisfy that combination
/// essentially never, which is why a run with no shape at all reports thousands of
/// invocations of api_vcall and not one that got past the argument decoding.
///
/// A shape says what each register would hold if the guest were calling the system call
/// correctly. The generator starts from that and then corrupts part of it, so the code
/// past the validation is reached while the validation itself still gets hostile input.
enum class Arg : uint8_t {
	ANY = 0, // Whatever argument() makes of it.
	OP, // A small operation selector.
	SMALL, // A count, size or element index.
	VPTR, // Pointer to a well-formed GuestVariant.
	OUT, // Pointer to writable guest memory, to receive a result.
	NAME, // Pointer to a null-terminated name.
	NAMELEN, // The length of the name NAME picked this iteration.
	ADDR, // The scoped object handle.
	OP_PACKED, // A Variant type tag in the Packed*Array range, which is how
	// api_packed_array_ops spells its operation.
	IDX_ANY, // Index of a scoped Variant, any type.
	IDX_ARRAY,
	IDX_DICT,
	IDX_STRING,
	IDX_PACKED,
	IDX_T2D,
	IDX_T3D,
	IDX_BASIS,
	IDX_QUAT,
	FRAME, // Base of a coroutine frame: an aligned run of GuestVariants.
	FRAME_SIZE, // Its length, mostly a whole number of Variant slots.
	FUNC,
	ARGC,
	NOARGS,
};

/// @brief The shape of a0-a7 for one system call, in register order.
struct Shape {
	int syscall;
	Arg args[8];
	int sub_op = -1;
};

// Only the system calls whose arguments have to agree with each other are listed. One
// left out is driven with hostile registers throughout, which is all it needs: a handler
// taking a single index or a single pointer is reached often enough by chance.
static constexpr Shape SHAPES[] = {
	{ ECALL_PRINT, { Arg::VPTR, Arg::SMALL } },
	{ ECALL_PRINT_CHANNEL, { Arg::VPTR, Arg::SMALL, Arg::SMALL } },
	{ ECALL_UTILITY, { Arg::OP, Arg::OUT, Arg::VPTR, Arg::SMALL } },
	{ ECALL_VCALL, { Arg::VPTR, Arg::NAME, Arg::NAMELEN, Arg::VPTR, Arg::SMALL, Arg::OUT } },
	// Pins argc=0 so nullary names (hash, size, duplicate) reach the call itself.
	{ ECALL_VCALL, { Arg::VPTR, Arg::NAME, Arg::NAMELEN, Arg::VPTR, Arg::NOARGS, Arg::OUT } },
	{ ECALL_VEVAL, { Arg::OP, Arg::VPTR, Arg::VPTR, Arg::OUT } },
	{ ECALL_VCREATE, { Arg::OUT, Arg::OP, Arg::OP, Arg::VPTR } },
	// Every Variant type has a zero-argument construction path. Pin argc to zero
	// so VCONSTRUCT reaches Godot's constructor dispatcher instead of relying on
	// four independent random registers to describe a coherent argument array.
	{ ECALL_VCONSTRUCT, { Arg::OUT, Arg::OP, Arg::NOARGS, Arg::NOARGS } },
	{ ECALL_VFETCH, { Arg::IDX_ANY, Arg::OUT, Arg::OP } },
	{ ECALL_VCLONE, { Arg::VPTR, Arg::OUT } },
	{ ECALL_VSTORE, { Arg::OUT, Arg::OP, Arg::VPTR, Arg::SMALL } },
	{ ECALL_VASSIGN, { Arg::IDX_ANY, Arg::IDX_ANY } },
	{ ECALL_OBJ, { Arg::OP, Arg::ADDR, Arg::VPTR } },
	{ ECALL_OBJ_PROP_GET, { Arg::ADDR, Arg::NAME, Arg::NAMELEN, Arg::OUT } },
	{ ECALL_OBJ_PROP_SET, { Arg::ADDR, Arg::NAME, Arg::NAMELEN, Arg::VPTR } },
	{ ECALL_VARIANT_GET, { Arg::VPTR, Arg::VPTR, Arg::OUT } },
	{ ECALL_OBJ_CALLP, { Arg::ADDR, Arg::NAME, Arg::NAMELEN, Arg::OP, Arg::OUT, Arg::VPTR, Arg::SMALL } },
	{ ECALL_OBJ_USES_TRAIT, { Arg::ADDR, Arg::NAME, Arg::NAMELEN, Arg::NAME, Arg::NAMELEN } },
	{ ECALL_GET_NODE, { Arg::ADDR, Arg::NAME, Arg::NAMELEN } },
	{ ECALL_NODE_CREATE, { Arg::OP, Arg::NAME, Arg::NAMELEN, Arg::NAME, Arg::NAMELEN } },
	{ ECALL_NODE, { Arg::OP, Arg::ADDR, Arg::VPTR } },
	{ ECALL_NODE2D, { Arg::OP, Arg::ADDR, Arg::VPTR } },
	{ ECALL_NODE3D, { Arg::OP, Arg::ADDR, Arg::VPTR } },
	{ ECALL_THROW, { Arg::NAME, Arg::NAMELEN, Arg::NAME, Arg::NAMELEN, Arg::VPTR, Arg::VPTR } },
	{ ECALL_ARRAY_OPS, { Arg::OP, Arg::IDX_ARRAY, Arg::SMALL, Arg::VPTR } },
	{ ECALL_ARRAY_AT, { Arg::IDX_ARRAY, Arg::SMALL, Arg::OUT } },
	{ ECALL_ARRAY_SIZE, { Arg::IDX_ARRAY } },
	{ ECALL_ARRAY_BATCH, { Arg::IDX_ARRAY, Arg::SMALL, Arg::SMALL, Arg::OUT } },
	{ ECALL_DICTIONARY_OPS, { Arg::OP, Arg::IDX_DICT, Arg::VPTR, Arg::VPTR, Arg::VPTR } },
	{ ECALL_STRING_CREATE, { Arg::NAME, Arg::NAMELEN } },
	{ ECALL_STRING_OPS, { Arg::OP, Arg::IDX_STRING, Arg::SMALL, Arg::VPTR } },
	{ ECALL_STRING_AT, { Arg::IDX_STRING, Arg::SMALL } },
	{ ECALL_STRING_SIZE, { Arg::IDX_STRING } },
	{ ECALL_STRING_CODEPOINT_BATCH, { Arg::IDX_STRING, Arg::SMALL, Arg::SMALL, Arg::OUT } },
	{ ECALL_STRING_APPEND, { Arg::IDX_STRING, Arg::NAME, Arg::NAMELEN } },
	{ ECALL_TIMER_PERIODIC, { Arg::ANY, Arg::OP, Arg::OUT, Arg::OUT, Arg::OUT } },
	{ ECALL_CALLABLE_CREATE, { Arg::OUT, Arg::VPTR } },
	{ ECALL_LOAD, { Arg::NAME, Arg::NAMELEN, Arg::OUT } },
	{ ECALL_SANDBOX_ADD, { Arg::OP, Arg::NAME, Arg::NAMELEN, Arg::OP, Arg::OUT, Arg::OUT, Arg::VPTR } },
	{ ECALL_SANDBOX_ADD, { Arg::OP, Arg::NAME, Arg::NAMELEN, Arg::OP, Arg::NAME, Arg::NAMELEN, Arg::OP },
		SANDBOX_ADD_PROPERTY_HINT },
	{ ECALL_PACKED_ARRAY_OPS, { Arg::OP_PACKED, Arg::OUT, Arg::VPTR } },
	{ ECALL_TRANSFORM_2D_OPS, { Arg::IDX_T2D, Arg::OP, Arg::VPTR, Arg::VPTR } },
	{ ECALL_TRANSFORM_3D_OPS, { Arg::IDX_T3D, Arg::OP, Arg::VPTR, Arg::VPTR } },
	{ ECALL_BASIS_OPS, { Arg::IDX_BASIS, Arg::OP, Arg::VPTR, Arg::VPTR } },
	{ ECALL_QUAT_OPS, { Arg::IDX_QUAT, Arg::OP, Arg::VPTR, Arg::VPTR } },
	{ ECALL_VEC2_OPS, { Arg::OP, Arg::OUT } },
	{ ECALL_VEC3_OPS, { Arg::OUT, Arg::OUT, Arg::OP } },
	{ ECALL_GET_OBJ, { Arg::NAME, Arg::NAMELEN } },
	// A suspension hands the host a frame of GuestVariants the guest wrote, and the host
	// walks it promoting every handle in it. That walk is the one place in the API where
	// getting it wrong is a hole rather than a bug, so it is worth reaching: the operand
	// has to be a Signal, and the frame has to be a plausible run of slots.
	{ ECALL_AWAIT, { Arg::VPTR, Arg::FRAME, Arg::FRAME_SIZE, Arg::SMALL, Arg::ANY, Arg::SMALL } },
	{ ECALL_AWAIT_RESTORE, { Arg::FRAME, Arg::FRAME_SIZE } },
	{ ECALL_CALL_GUEST, { Arg::FUNC, Arg::VPTR, Arg::ARGC, Arg::OUT } },
};

struct SyscallFuzzer {
	Sandbox &emu;
	machine_t &machine;
	std::mt19937_64 rng;

	gaddr_t scratch = 0x0;
	gaddr_t scratch_size = 0x0;
	std::vector<uint8_t> scratch_bytes;

	// The scratch buffer is laid out in zones rather than filled with noise end to end.
	// Random bytes almost never make a valid Variant type tag or a null-terminated method
	// name, and a handler that rejects its arguments in the first three lines exercises
	// nothing. The zones give the argument generator something well-formed to point at,
	// so the interesting code past the validation runs too.
	gaddr_t variants_zone = 0x0; // An array of plausible GuestVariants
	unsigned variant_slots = 0;
	std::vector<gaddr_t> name_addresses; // Null-terminated method and property names
	std::vector<unsigned> name_lengths; // Parallel to name_addresses, so NAMELEN agrees
	gaddr_t exec_begin = 0x0;
	gaddr_t exec_end = 0x0;
	std::vector<gaddr_t> function_addresses;
	unsigned scoped_variant_count = 0;
	// Scoped-Variant indices grouped by the type of the Variant they refer to. A handler
	// keyed on "the index of a Dictionary" leaves its switch immediately unless the index
	// really is a Dictionary's, and an index drawn out of the whole table is one about
	// one time in twenty-five.
	std::map<int, std::vector<unsigned>> indices_by_type;
	// api_sandbox_add refuses every call made outside program initialization, which is
	// precisely when a hostile ELF is running. Part of the run is spent with it set.
	bool *initialization_flag = nullptr;
	// The name this iteration's NAME registers point at, so that NAMELEN can be its length.
	unsigned current_name = 0;
	/// @brief The guest-visible handle of the one object the fuzzer scopes.
	uint64_t scoped_object_handle = 0;

	// How pointer-heavy the current iteration's registers are, out of 10. See argument().
	unsigned pointer_weight = 5;

	uint64_t iterations = 0;
	uint64_t exceptions = 0;
	// Per system call: how many times it was invoked, and how many of those returned
	// instead of throwing. A syscall that never returns is one the fuzzer is not really
	// reaching, and the argument generator needs to learn something about it.
	std::map<int, std::pair<uint64_t, uint64_t>> coverage;

	SyscallFuzzer(Sandbox &p_emu, uint64_t seed) :
			emu(p_emu), machine(p_emu.machine()), rng(seed) {}

	unsigned pick(unsigned n) { return unsigned(rng() % n); }

	/// @brief A guest address that really is inside the scratch buffer, so that the
	/// handler gets past memarray() and on to the code that interprets what it read.
	gaddr_t scratch_address() {
		const gaddr_t offset = scratch_size ? gaddr_t(rng() % scratch_size) : 0;
		// Two thirds aligned: an unaligned address is rejected by memarray() before
		// the handler sees it, which is a path worth taking but not most of the time.
		return scratch + (pick(3) ? (offset & ~gaddr_t(0x7)) : offset);
	}

	/// @brief The address of a well-formed GuestVariant, so that handlers which read one
	/// out of guest memory get a real type tag and a real scoped index.
	gaddr_t variant_address() {
		if (variant_slots == 0)
			return scratch_address();
		// Sometimes point part-way into the array, so that handlers reading several in a
		// row (call arguments, key/value pairs) run off the end of it.
		const unsigned slot = pick(variant_slots + variant_slots / 4);
		return variants_zone + slot * sizeof(GuestVariant);
	}

	/// @brief The address of a null-terminated name, for the method and property calls.
	gaddr_t name_address() {
		if (name_addresses.empty())
			return scratch_address();
		return name_addresses[pick(name_addresses.size())];
	}

	/// @brief One argument register's worth of hostility.
	uint64_t argument() {
		// Pointer or not is decided by this iteration's weight rather than per register.
		// Choosing independently means a handler needing three valid pointers at once
		// (veval, the Transform operations) essentially never gets them, and the code past
		// its argument decoding is never reached at all.
		if (pick(10) < pointer_weight) {
			switch (pick(8)) {
				case 0:
				case 1:
				case 2: // Shaped like the GuestVariant the handler expects to read.
					return variant_address();
				case 3: // A null-terminated method or property name.
					return name_address();
				case 4: // A pointer that mostly does not survive the bounds check.
					return rng() % (machine.memory.memory_arena_size() * 2);
				case 5: // The one scoped object, so the Object/Node calls get past their
					// address check and into the work the run is actually aimed at.
					return scoped_object_handle;
				default: // Somewhere in the scratch buffer.
					return scratch_address();
			}
		}
		switch (pick(10)) {
			case 0:
			case 1:
			case 2:
			case 3:
			case 4:
			case 5: // Operation selectors, sizes, scoped Variant indices.
				return rng() % 48;
			case 6:
			case 7:
			case 8:
				return INTERESTING[pick(std::size(INTERESTING))];
			default:
				return rng();
		}
	}

	uint64_t float_bits() {
		if (pick(2))
			return INTERESTING_FLOATS[pick(std::size(INTERESTING_FLOATS))];
		return rng();
	}

	/// @brief Give the scoped-Variant table one of every type the API can hand back, so
	/// that index-based handlers reach their real work instead of stopping at "no such
	/// index". Also scopes the sandbox node itself, as the one object to aim at.
	/// @brief Scope a Variant and remember its index under its type.
	void scope(Variant &&v) {
		const int type = int(v.get_type());
		indices_by_type[type].push_back(emu.create_scoped_variant(std::move(v)));
	}

	/// @brief An index whose scoped Variant really is of this type, or a hostile integer
	/// when the table has none. Handlers reject the latter, which is a path worth taking.
	uint64_t index_of_type(int type) {
		auto it = indices_by_type.find(type);
		if (it == indices_by_type.end() || it->second.empty())
			return argument();
		return it->second[pick(it->second.size())];
	}

	/// @brief An index of any of these types; the packed arrays share one handler.
	uint64_t index_of_types(const int *types, size_t count) {
		// Collect only the ones actually present, so a miss is not silently a hostile value
		// most of the time.
		unsigned candidates[16];
		size_t n = 0;
		for (size_t i = 0; i < count && n < std::size(candidates); i++) {
			auto it = indices_by_type.find(types[i]);
			if (it != indices_by_type.end())
				for (unsigned idx : it->second)
					if (n < std::size(candidates))
						candidates[n++] = idx;
		}
		if (n == 0)
			return argument();
		return candidates[pick(n)];
	}

	void seed_state() {
		// Every suspension that got through pins a frame and its promoted Variants, and
		// the cap would otherwise turn the rest of the run into one refusal after another.
		emu.reap_coroutines();
		emu.state().reset();
		indices_by_type.clear();

		Array array;
		array.push_back(1);
		array.push_back(String("fuzz"));
		array.push_back(Vector2(1, 2));
		Dictionary dict;
		dict["key"] = 1;
		dict[2] = String("value");

		PackedByteArray bytes;
		bytes.resize(16);
		PackedFloat32Array f32;
		f32.resize(4);
		PackedInt32Array i32;
		i32.resize(4);
		PackedStringArray strings;
		strings.push_back("fuzz");
		PackedVector3Array v3;
		v3.resize(4);

		// The shape the modding advice produces: everything restricted, and the whole API
		// handed over as one Dictionary of Callables. That Dictionary and the Callables in
		// it are the only host-side objects a mod can reach, so they are what the run has
		// to spend its time on.
		Dictionary modding_api;
		modding_api["get_name"] = Callable(&emu, "get_name");
		modding_api["has_function"] = Callable(&emu, "has_function");
		modding_api["vmcall"] = Callable(&emu, "vmcall");
		modding_api["nested"] = dict.duplicate();
		modding_api["values"] = array.duplicate();

		scope(Variant());
		scope(Variant(true));
		scope(Variant(int64_t(42)));
		scope(Variant(3.14));
		scope(Variant(String("fuzzing string")));
		// A second String, so an index that resolved once is not the only one that does.
		scope(Variant(String("")));
		scope(Variant(StringName("fuzz")));
		scope(Variant(NodePath("../fuzz")));
		scope(Variant(array.duplicate()));
		scope(Variant(std::move(array)));
		scope(Variant(dict.duplicate()));
		scope(Variant(std::move(dict)));
		scope(Variant(std::move(modding_api)));
		scope(Variant(Transform2D()));
		scope(Variant(Transform3D()));
		scope(Variant(Basis()));
		scope(Variant(Quaternion()));
		scope(Variant(AABB()));
		scope(Variant(Projection()));
		scope(Variant(Plane()));
		scope(Variant(Color(1, 1, 1, 1)));
		scope(Variant(RID()));
		scope(Variant(std::move(bytes)));
		scope(Variant(std::move(f32)));
		scope(Variant(std::move(i32)));
		scope(Variant(std::move(strings)));
		scope(Variant(std::move(v3)));
		scope(Variant(Callable()));
		// A Callable that resolves, so calling one through the API is not only ever the
		// null case. This is the edge a mod is actually handed.
		scope(Variant(Callable(&emu, "get_name")));
		// A Signal that resolves: without one, every await stops at "not awaitable" and
		// the frame walk behind it is never reached.
		scope(Variant(Signal(&emu, "tree_entered")));
		scope(Variant(&emu));

		// One real, scoped object to aim the Object/Node system calls at. With
		// restrictions on, every operation on it is supposed to be refused.
		scoped_object_handle = emu.add_scoped_object(&emu);

		scoped_variant_count = unsigned(emu.state().scoped_variants.size());
	}

	/// @brief Rebuild the scratch buffer: random bytes, then a zone of well-formed
	/// GuestVariants and a zone of null-terminated names. Called often enough that
	/// handlers see fresh hostile lengths and pointers rather than the same ones.
	void randomize_scratch() {
		for (size_t i = 0; i < scratch_bytes.size(); i += 8) {
			const uint64_t word = pick(4) ? rng() : INTERESTING[pick(std::size(INTERESTING))];
			std::memcpy(&scratch_bytes[i], &word, std::min<size_t>(8, scratch_bytes.size() - i));
		}
		// Sprinkle in addresses that point back into the scratch buffer, so that a
		// guest-side pointer-and-length struct occasionally passes validation.
		for (unsigned i = 0; i < scratch_bytes.size() / 256; i++) {
			const size_t at = (rng() % (scratch_bytes.size() / 8)) * 8;
			const gaddr_t addr = scratch_address();
			std::memcpy(&scratch_bytes[at], &addr, sizeof(addr));
		}
		// Whole {pointer, length} descriptors, the shape api_vcreate and the CppString
		// paths read out of guest memory. The length beside a random address is a random
		// 64-bit number, which memview() refuses every time, so the struct has to be laid
		// down as a pair or those paths stop at their first read.
		for (unsigned i = 0; i < scratch_bytes.size() / 128; i++) {
			const size_t at = (rng() % (scratch_bytes.size() / 16)) * 16;
			const gaddr_t addr = scratch + gaddr_t((rng() % (scratch_size / 2)) & ~gaddr_t(7));
			const gaddr_t len = pick(8) ? gaddr_t(rng() % 64) : gaddr_t(argument());
			std::memcpy(&scratch_bytes[at], &addr, sizeof(addr));
			std::memcpy(&scratch_bytes[at + sizeof(addr)], &len, sizeof(len));
		}

		build_variant_zone();
		build_name_zone();

		machine.memory.memcpy(scratch, scratch_bytes.data(), scratch_bytes.size());
	}

	/// @brief Fill the last quarter of the buffer with GuestVariants a guest could
	/// plausibly have built: a real type tag, and an index that mostly refers to a
	/// scoped Variant that exists.
	void build_variant_zone() {
		const size_t zone_offset = (scratch_bytes.size() / 4) * 3;
		variants_zone = scratch + zone_offset;
		variant_slots = unsigned((scratch_bytes.size() - zone_offset) / sizeof(GuestVariant));

		for (unsigned slot = 0; slot < variant_slots; slot++) {
			GuestVariant gv;
			// Half of them coherent: the type tag and the scoped Variant the index refers
			// to are the same type, which is the only shape a guest that is not lying ever
			// produces. A tag drawn independently of the index disagrees with it about
			// twenty-four times in twenty-five, and a handler that checks one against the
			// other then never gets past the check.
			if (!indices_by_type.empty() && pick(2)) {
				auto it = indices_by_type.begin();
				std::advance(it, pick(indices_by_type.size()));
				gv.type = static_cast<Variant::Type>(it->first);
				gv.v.i = int64_t(it->second[pick(it->second.size())]);
				if (gv.type == Variant::OBJECT)
					gv.v.i = int64_t(scoped_object_handle);
			} else {
				gv.type = static_cast<Variant::Type>(rng() % (Variant::VARIANT_MAX + 4));
				if (gv.type == Variant::OBJECT) {
					// The one object the fuzzer has scoped. Everything done to it should be
					// refused, which is exactly what the run is checking.
					gv.v.i = int64_t(scoped_object_handle);
				} else if (scoped_variant_count != 0 && pick(4) != 0) {
					// An index that resolves, so the handler gets to its real work.
					gv.v.i = int64_t(rng() % scoped_variant_count);
				} else {
					gv.v.i = int64_t(argument());
				}
			}
			std::memcpy(&scratch_bytes[zone_offset + slot * sizeof(GuestVariant)], &gv, sizeof(gv));
		}
	}

	/// @brief Lay down null-terminated names just before the Variant zone. Handlers that
	/// take a method or property name only reach their allowed-method check when the name
	/// they read is terminated; random bytes rarely are.
	void build_name_zone() {
		// A mix of names that exist on the scoped object, names that do not, and the
		// empty string, which is its own edge case in Godot's name lookup.
		static constexpr const char *NAMES[] = {
			"", "call", "free", "get_class", "queue_free", "get_parent", "duplicate",
			"name", "position", "size", "length", "hash", "get", "set", "connect",
			"emit_signal", "notification", "get_child", "add_child", "to_string",
			"vmcall", "set_program", "has_function", "no_such_method_at_all",
		};
		const size_t zone_offset = scratch_bytes.size() / 2;
		size_t at = zone_offset;
		name_addresses.clear();
		name_lengths.clear();

		for (const char *name : NAMES) {
			const size_t len = std::strlen(name) + 1;
			if (at + len > (scratch_bytes.size() / 4) * 3)
				break;
			std::memcpy(&scratch_bytes[at], name, len);
			name_addresses.push_back(scratch + at);
			name_lengths.push_back(unsigned(len - 1));
			at += len;
		}
	}

	const Shape *shape_for(int syscall) {
		const Shape *found[4];
		size_t n = 0;
		for (const Shape &s : SHAPES)
			if (s.syscall == syscall && n < std::size(found))
				found[n++] = &s;
		if (n == 0)
			return nullptr;
		return found[pick(unsigned(n))];
	}

	/// @brief One register filled the way a correct guest would fill it.
	uint64_t shaped_argument(Arg kind) {
		// Packed arrays all reach the same handler, and any of them gets it past the check.
		static constexpr int PACKED[] = {
			Variant::PACKED_BYTE_ARRAY, Variant::PACKED_INT32_ARRAY, Variant::PACKED_INT64_ARRAY,
			Variant::PACKED_FLOAT32_ARRAY, Variant::PACKED_FLOAT64_ARRAY,
			Variant::PACKED_STRING_ARRAY, Variant::PACKED_VECTOR2_ARRAY,
			Variant::PACKED_VECTOR3_ARRAY, Variant::PACKED_COLOR_ARRAY,
		};
		switch (kind) {
			case Arg::ANY:
				return argument();
			case Arg::OP:
				// Mostly in range for every operation enum the API has, sometimes past the
				// end of one: an operation selector the handler does not know is its own case.
				return pick(8) ? rng() % 24 : rng() % 64;
			case Arg::SMALL:
				return pick(4) ? rng() % 16 : argument();
			case Arg::VPTR:
				return variant_address();
			case Arg::OUT:
				// memarray() rejects an unaligned or out-of-range address before the handler
				// runs, so a result pointer is mostly a real slot. The rejection itself is
				// worth reaching, which is what the other quarter is for.
				return pick(4) ? variant_address() : scratch_address();
			case Arg::NAME:
				return name_addresses.empty() ? scratch_address() : name_addresses[current_name];
			case Arg::NAMELEN:
				return name_lengths.empty() ? argument() : name_lengths[current_name];
			case Arg::ADDR:
				return scoped_object_handle;
			case Arg::OP_PACKED:
				// api_packed_array_ops takes a Variant type tag as its operation, so the
				// range that means anything to it starts at PACKED_BYTE_ARRAY.
				return pick(8) ? uint64_t(Variant::PACKED_BYTE_ARRAY) + pick(10) : rng() % 64;
			case Arg::IDX_ANY:
				return scoped_variant_count ? rng() % scoped_variant_count : argument();
			case Arg::IDX_ARRAY:
				return index_of_type(Variant::ARRAY);
			case Arg::IDX_DICT:
				return index_of_type(Variant::DICTIONARY);
			case Arg::IDX_STRING:
				return index_of_type(Variant::STRING);
			case Arg::IDX_PACKED:
				return index_of_types(PACKED, std::size(PACKED));
			case Arg::IDX_T2D:
				return index_of_type(Variant::TRANSFORM2D);
			case Arg::IDX_T3D:
				return index_of_type(Variant::TRANSFORM3D);
			case Arg::IDX_BASIS:
				return index_of_type(Variant::BASIS);
			case Arg::IDX_QUAT:
				return index_of_type(Variant::QUATERNION);
			case Arg::FRAME:
				// The Variant zone itself, slot-aligned: what the guest hands over is
				// supposed to be its own array of Variant slots.
				if (variant_slots == 0)
					return scratch_address();
				return variants_zone + gaddr_t(pick(std::max(1u, variant_slots / 2))) * sizeof(GuestVariant);
			case Arg::FRAME_SIZE:
				// Mostly a whole number of slots, which is the only thing the host accepts;
				// the rest reaches the rejection.
				return pick(4) ? uint64_t(1 + pick(8)) * sizeof(GuestVariant) : argument();
			case Arg::FUNC: {
				if (pick(4) == 0)
					return argument();
				if (!function_addresses.empty() && pick(2))
					return function_addresses[pick(function_addresses.size())];
				if (exec_end <= exec_begin)
					return argument();
				return (exec_begin + gaddr_t(rng() % (exec_end - exec_begin))) & ~gaddr_t(1);
			}
			case Arg::ARGC:
				return pick(4) ? pick(9) : argument();
			case Arg::NOARGS:
				return 0;
		}
		return argument();
	}

	/// @brief Fill a0-a7 for one system call.
	///
	/// Three shapes of register file, and the run needs all three. Unshaped is what a
	/// handler sees from a guest that is not even trying, and is the only one that reaches
	/// the early bounds checks. Shaped-and-corrupted is the interesting one: the arguments
	/// agree well enough to get past the validation, and then one of them is a lie. Shaped
	/// alone is what proves the handler can be reached at all, and is what the coverage
	/// counters are measuring.
	void build_registers(int syscall) {
		if (!name_addresses.empty())
			current_name = pick(name_addresses.size());

		const Shape *shape = shape_for(syscall);
		if (shape == nullptr || pick(4) == 0) {
			for (int reg = riscv::REG_ARG0; reg < riscv::REG_ARG0 + 8; reg++)
				machine.cpu.reg(reg) = argument();
			return;
		}

		for (unsigned i = 0; i < 8; i++)
			machine.cpu.reg(riscv::REG_ARG0 + i) = shaped_argument(shape->args[i]);
		if (shape->sub_op >= 0)
			machine.cpu.reg(riscv::REG_ARG0) = uint64_t(shape->sub_op);

		// Corrupt part of it. A handler that only ever sees arguments that agree is being
		// tested for what it does, not for what it does when lied to.
		if (pick(4) != 0) {
			const unsigned corrupt = 1 + pick(2);
			for (unsigned c = 0; c < corrupt; c++)
				machine.cpu.reg(riscv::REG_ARG0 + pick(8)) = argument();
		}
	}

	/// @param only A single system call number to fuzz, or -1 for all of them.
	void run(int64_t count, int first_syscall, int last_syscall, int only) {
		const int range = last_syscall - first_syscall;
		if (range <= 0)
			return;

		for (int64_t i = 0; i < count; i++) {
			// The scoped-Variant tables fill up as handlers create Variants, and the
			// scratch buffer goes stale. Both are refreshed often enough that most
			// iterations start from a state a real guest could be in.
			if ((i % 64) == 0) {
				seed_state(); // Before the scratch: the Variant zone indexes into it.
				randomize_scratch();
			}

			// Some handlers want a register file that is almost all pointers, others one
			// that is almost all small integers. Committing to one shape per iteration
			// reaches both, where an even mix reaches neither.
			static constexpr unsigned WEIGHTS[] = { 1, 3, 5, 8, 9 };
			pointer_weight = WEIGHTS[pick(std::size(WEIGHTS))];

			const int syscall = (only >= 0) ? only : first_syscall + int(rng() % range);

			// api_sandbox_add throws on everything outside initialization, so a run that is
			// never initializing never sees the property and method registration it parses
			// -- which is the one part of the API a program reaches before it has done
			// anything else. Part of the run is spent there.
			if (initialization_flag != nullptr)
				*initialization_flag = (pick(3) == 0);

			build_registers(syscall);
			for (int freg = 10; freg < 18; freg++)
				machine.cpu.registers().getfl(freg).load_u64(float_bits());

			machine.cpu.reg(riscv::REG_ECALL) = syscall;

			auto &covered = coverage[syscall];
			covered.first++;
			try {
				machine_t::syscall_handlers[syscall](machine);
				covered.second++;
			} catch (const std::exception &) {
				// A handler that refuses hostile arguments by throwing is working as
				// intended: the exception unwinds into the guest as a VM exception.
				exceptions++;
			} catch (...) {
				exceptions++;
			}
			iterations++;
		}
		if (initialization_flag != nullptr)
			*initialization_flag = false;
	}
};

/// @brief The original assault: hand GuestVariant::toVariant() structs made of random
/// bytes, with only the type tag forced into range.
static uint64_t fuzz_guest_variants(Sandbox &emu, uint64_t seed, int64_t iterations) {
	std::mt19937_64 rng(seed);
	uint64_t exceptions = 0;

	for (int64_t i = 0; i < iterations; i++) {
		GuestVariant v;
		uint8_t *data = reinterpret_cast<uint8_t *>(&v);
		for (size_t b = 0; b < sizeof(GuestVariant); b += 8) {
			const uint64_t word = rng();
			std::memcpy(&data[b], &word, std::min<size_t>(8, sizeof(GuestVariant) - b));
		}
		// Make the type tag valid, which is the only field a guest cannot lie about.
		v.type = static_cast<Variant::Type>(rng() % (Variant::VARIANT_MAX + 1));

		try {
			v.toVariant(emu);
		} catch (const std::exception &) {
			exceptions++;
		} catch (...) {
			exceptions++;
		}
	}
	return exceptions;
}

/// @brief Break the reference cycles a run leaves behind.
///
/// Godot's Arrays and Dictionaries are reference counted with nothing to collect a cycle,
/// so a container the fuzzer stored inside itself keeps itself alive after the scoped
/// table has let go of it -- exactly the leak `var d = {}; d.self = d` produces in
/// ordinary GDScript, and not something the sandbox can prevent. Clearing them on the way
/// out keeps a fuzzing run from leaving the process reporting leaked Variant pages at exit.
static void break_container_cycles(Variant &value, int depth = 0) {
	if (depth > 8)
		return;
	if (value.get_type() == Variant::ARRAY) {
		Array array = value.operator Array();
		for (int i = 0; i < array.size(); i++) {
			Variant element = array[i];
			break_container_cycles(element, depth + 1);
		}
		array.clear();
	} else if (value.get_type() == Variant::DICTIONARY) {
		Dictionary dict = value.operator Dictionary();
		Array keys = dict.keys();
		for (int i = 0; i < keys.size(); i++) {
			Variant element = dict[keys[i]];
			break_container_cycles(element, depth + 1);
		}
		dict.clear();
	}
}

/// @brief Silence ERR_PRINT for as long as it is in scope.
struct SilenceErrors {
	Engine *engine = Engine::get_singleton();
	bool previous = true;

	SilenceErrors() {
		if (engine) {
			previous = engine->is_printing_error_messages();
			engine->set_print_error_messages(false);
		}
	}
	~SilenceErrors() {
		if (engine)
			engine->set_print_error_messages(previous);
	}
};

} // namespace

/**
 * @brief Fuzz the host side of the sandbox API.
 *
 * @param test Which target to fuzz, in the form "target[/syscall][:seed]":
 *             - "syscalls" drives every system call handler,
 *             - "syscalls/517" drives only that one, for narrowing a failure down,
 *             - "variants" drives GuestVariant conversion,
 *             - "" or "all" does both.
 *             The ":seed" suffix replays an earlier run, eg. "syscalls:1234".
 * @param iterations How many system calls (or GuestVariants) to throw at it.
 * @return A Dictionary with the seed, the number of iterations completed, how many of
 *         them were refused by throwing, and per-system-call counts under "coverage"
 *         (syscall number -> [invoked, returned]). A crash is the failure signal; the
 *         counts are there to show the run actually reached the handlers.
 *
 * @warning This leaves guest memory and the sandbox's Variant state arbitrary. Reset
 * the sandbox afterwards before using it for anything else.
 */
Dictionary Sandbox::assault(const String &test, int64_t iterations) {
	Dictionary result;
	if (iterations <= 0)
		return result;

	// "target:seed" makes a failing run reproducible; without a seed we pick one and
	// report it back, so a crash can be replayed.
	String target = test;
	uint64_t seed = std::random_device{}();
	const int colon = test.find(":");
	if (colon >= 0) {
		target = test.substr(0, colon);
		seed = uint64_t(test.substr(colon + 1).to_int());
	}
	// "target/517" narrows the run down to a single system call.
	int only_syscall = -1;
	const int slash = target.find("/");
	if (slash >= 0) {
		only_syscall = int(target.substr(slash + 1).to_int());
		target = target.substr(0, slash);
		if (only_syscall < GAME_API_BASE || only_syscall >= ECALL_LAST) {
			ERR_PRINT("assault(): System call out of range: " + itos(only_syscall));
			return result;
		}
	}
	if (target.is_empty())
		target = "all";

	const bool do_syscalls = (target == "all" || target == "syscalls");
	const bool do_variants = (target == "all" || target == "variants");
	if (!do_syscalls && !do_variants) {
		ERR_PRINT("assault(): Unknown fuzzing target: " + target);
		return result;
	}

	result["seed"] = int64_t(seed);
	result["target"] = target;

	// Everything is denied for the duration of the run, and put back afterwards. A
	// system call that still reaches Godot from here is a restriction bypass.
	const bool had_restrictions = this->get_restrictions();
	this->set_restrictions(true);

	// api_call_guest can land inside a loop; cap instructions to avoid hangs.
	const int64_t had_instructions_max = this->get_instructions_max();
	this->set_instructions_max(1);

	uint64_t exceptions = 0;
	uint64_t completed = 0;

	if (do_variants) {
		// Silenced only around the loop itself, so that the harness can still report a
		// problem of its own below.
		SilenceErrors silence;
		exceptions += fuzz_guest_variants(*this, seed, iterations);
		completed += iterations;
	}

	if (do_syscalls) {
		if (!this->has_program_loaded()) {
			ERR_PRINT("assault(): Fuzzing system calls needs a loaded program for its guest memory.");
		} else {
			// Run one level down, the way a VM call does, so that the permanent Variants
			// at level 0 (the program's properties) are not in the blast radius.
			CurrentState *const saved_state = this->m_current_state;
			riscv::Registers<RISCV_ARCH> saved_registers = machine().cpu.registers();
			this->m_current_state = &this->m_states[1];

			SyscallFuzzer fuzzer(*this, seed);
			fuzzer.initialization_flag = &this->m_is_initialization;
			// Guest memory for the handlers to read structs out of. Taken from the
			// guest heap so that it is real, mapped, writable memory.
			const auto &exec = machine().memory.exec_segment_for(machine().memory.start_address());
			fuzzer.exec_begin = exec->exec_begin();
			fuzzer.exec_end = exec->exec_end();
			fuzzer.function_addresses.push_back(machine().memory.start_address());
			for (const String &name : this->get_functions()) {
				const gaddr_t address = this->address_of(name);
				if (address != 0x0) {
					fuzzer.function_addresses.push_back(address);
				}
			}
			fuzzer.scratch_size = 64 * 1024;
			fuzzer.scratch = machine().arena().malloc(fuzzer.scratch_size);
			if (fuzzer.scratch == 0x0) {
				ERR_PRINT("assault(): Could not allocate guest scratch memory.");
			} else {
				fuzzer.scratch_bytes.resize(fuzzer.scratch_size);
				try {
					// Every refused system call prints an error, and a hundred thousand of
					// them is not a useful log. Silenced only around the loop.
					SilenceErrors silence;
					fuzzer.run(iterations, GAME_API_BASE, ECALL_LAST, only_syscall);
				} catch (const std::exception &e) {
					// Only reachable if the harness itself throws outside a handler.
					ERR_PRINT("assault(): Fuzzer harness exception: " + String(e.what()));
				}
				machine().arena().free(fuzzer.scratch);
			}

			exceptions += fuzzer.exceptions;
			completed += fuzzer.iterations;

			Dictionary coverage;
			for (const auto &[syscall, counts] : fuzzer.coverage) {
				Array pair;
				pair.push_back(int64_t(counts.first));
				pair.push_back(int64_t(counts.second));
				coverage[syscall] = pair;
			}
			result["coverage"] = coverage;

			// api_call_guest re-enters, so all levels may hold scoped Variants.
			for (size_t level = 1; level < this->m_states.size(); level++) {
				for (Variant &v : this->m_states[level].variants)
					break_container_cycles(v);
				this->m_states[level].reset();
			}
			this->m_current_state = saved_state;
			machine().cpu.registers() = saved_registers;
		}
	}

	this->set_instructions_max(had_instructions_max);
	if (!had_restrictions)
		this->set_restrictions(false);

	result["iterations"] = int64_t(completed);
	result["exceptions"] = int64_t(exceptions);
	return result;
}
