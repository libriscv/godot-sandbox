#include "../guest_datatypes.h"
#include "../syscalls.h"

#include <godot_cpp/classes/engine.hpp>
#include <cstring>
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
	unsigned scoped_variant_count = 0;

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
	void seed_state() {
		emu.state().reset();

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

		emu.create_scoped_variant(Variant());
		emu.create_scoped_variant(Variant(true));
		emu.create_scoped_variant(Variant(int64_t(42)));
		emu.create_scoped_variant(Variant(3.14));
		emu.create_scoped_variant(Variant(String("fuzzing string")));
		emu.create_scoped_variant(Variant(StringName("fuzz")));
		emu.create_scoped_variant(Variant(NodePath("../fuzz")));
		emu.create_scoped_variant(Variant(std::move(array)));
		emu.create_scoped_variant(Variant(std::move(dict)));
		emu.create_scoped_variant(Variant(Transform2D()));
		emu.create_scoped_variant(Variant(Transform3D()));
		emu.create_scoped_variant(Variant(Basis()));
		emu.create_scoped_variant(Variant(Quaternion()));
		emu.create_scoped_variant(Variant(AABB()));
		emu.create_scoped_variant(Variant(Projection()));
		emu.create_scoped_variant(Variant(Plane()));
		emu.create_scoped_variant(Variant(Color(1, 1, 1, 1)));
		emu.create_scoped_variant(Variant(RID()));
		emu.create_scoped_variant(Variant(std::move(bytes)));
		emu.create_scoped_variant(Variant(std::move(f32)));
		emu.create_scoped_variant(Variant(std::move(i32)));
		emu.create_scoped_variant(Variant(std::move(strings)));
		emu.create_scoped_variant(Variant(std::move(v3)));
		emu.create_scoped_variant(Variant(Callable()));
		emu.create_scoped_variant(Variant(&emu));

		// One real, scoped object to aim the Object/Node system calls at. With
		// restrictions on, every operation on it is supposed to be refused.
		emu.add_scoped_object(&emu);

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
			gv.type = static_cast<Variant::Type>(rng() % (Variant::VARIANT_MAX + 4));
			if (gv.type == Variant::OBJECT) {
				// The one object the fuzzer has scoped. Everything done to it should be
				// refused, which is exactly what the run is checking.
				gv.v.i = int64_t(uintptr_t(&emu));
			} else if (scoped_variant_count != 0 && pick(4) != 0) {
				// An index that resolves, so the handler gets to its real work.
				gv.v.i = int64_t(rng() % scoped_variant_count);
			} else {
				gv.v.i = int64_t(argument());
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

		for (const char *name : NAMES) {
			const size_t len = std::strlen(name) + 1;
			if (at + len > (scratch_bytes.size() / 4) * 3)
				break;
			std::memcpy(&scratch_bytes[at], name, len);
			name_addresses.push_back(scratch + at);
			at += len;
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

			for (int reg = riscv::REG_ARG0; reg < riscv::REG_ARG0 + 8; reg++)
				machine.cpu.reg(reg) = argument();
			for (int freg = 10; freg < 18; freg++)
				machine.cpu.registers().getfl(freg).load_u64(float_bits());

			const int syscall = (only >= 0) ? only : first_syscall + int(rng() % range);
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
			// Guest memory for the handlers to read structs out of. Taken from the
			// guest heap so that it is real, mapped, writable memory.
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

			this->m_states[1].reset();
			this->m_current_state = saved_state;
			machine().cpu.registers() = saved_registers;
		}
	}

	if (!had_restrictions)
		this->set_restrictions(false);

	result["iterations"] = int64_t(completed);
	result["exceptions"] = int64_t(exceptions);
	return result;
}
