#pragma once
#include <algorithm>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <libriscv/machine.hpp>
#include <memory>
#include <optional>
#include <typeinfo>
#include <unordered_set>

using namespace godot;
#define RISCV_ARCH riscv::RISCV64
using gaddr_t = riscv::address_type<RISCV_ARCH>;
using machine_t = riscv::Machine<RISCV_ARCH>;
#include "elf/script_elf.h"
#include "gdscript/compiler/gdsmeta.h"
#include "syscalls.h"
#include "stringname_id.hpp"
#include "vmcallable.h"
#include "vmproperty.h"
#include "sandbox_function_state.h"

/**
 * @brief The Sandbox class is a Godot node that provides a safe environment for running untrusted code.
 *
 * The sandbox is constructed with a program, which is a 64-bit RISC-V ELF executable file that contains functions and code to be executed.
 * Programs are loaded into the sandbox using the `set_program` method.
 * Upon setting a program, the sandbox will load the program into memory and initialize the RISC-V machine in several steps:
 * 1. Remove old machine instance, if any.
 * 2. Create a new machine instance with the given program.
 * 3. Set up system calls, native heap and native memory syscalls.
 * 4. Set up the Linux environment for the program.
 * 5. Run the program through to its main() function.
 * 6. Read the program's properties. These will be visible to the Godot editor.
 * 7. Pre-cache some public functions. These will be available to call from GDScript.
 **/
class Sandbox : public Node {
	GDCLASS(Sandbox, Node);

protected:
	static void _bind_methods();

	String _to_string() const;

public:
	static constexpr unsigned MAX_INSTRUCTIONS = 8000; // Millions
	static constexpr unsigned MAX_HEAP = 20ul; // MBs
	// Power of two: the binary translator can only use the AND-masked arena on a
	// Po2 arena, and that mask is what the default relies on for its guard.
	static constexpr unsigned MAX_VMEM = 32ul; // MBs
	static constexpr unsigned MAX_HEAP_ALLOCS = 4000; // Max guest heap allocations
	static constexpr unsigned MAX_LEVEL = 4; // Maximum call recursion depth
	// Shared across MAX_LEVEL recursion levels.
	static constexpr unsigned GUEST_STACK_SIZE = 2u << 20; // 2MB
	static constexpr unsigned MAX_REFS = 100; // Default maximum number of references
	static constexpr unsigned MAX_UNRESTRICTED_REFS = 65536;
	static constexpr unsigned EDITOR_THROTTLE = 8; // Throttle VM calls from the editor
	static constexpr unsigned MAX_PROPERTIES = 256; // Maximum number of sandboxed properties
	static constexpr unsigned MAX_GUEST_PROPERTY_SLOTS = 32; // ABI-fixed guest property array length
	static constexpr unsigned MAX_COROUTINES = 32; // Default cap on live suspended frames
	static constexpr unsigned MAX_COROUTINE_LIMIT = 65536; // Ceiling for the setter, not a budget
	static constexpr unsigned MAX_PUBLIC_FUNCTIONS = 128; // Maximum number of public functions

	struct CurrentState {
		std::vector<Variant> variants;
		std::vector<const Variant *> scoped_variants;
		/// @brief An object the guest may refer to during this call, and the godot-cpp
		/// binding for it once someone has needed one.
		struct ScopedObject {
			uint64_t object_id;
			uintptr_t engine_object;
			/// Resolving a binding locks a per-object mutex in Godot, so it is done at most
			/// once per object per call: on the way in when the caller already had one, and
			/// otherwise the first time the guest actually uses the handle.
			godot::Object *binding;
		};
		std::vector<ScopedObject> scoped_objects;
		/// @brief Holds a reference to every RefCounted handed to the guest during this
		/// call. Without it a Ref returned by value dies with the temporary Variant it
		/// arrived in, and the guest is left with a pointer to freed memory.
		std::vector<Ref<RefCounted>> scoped_refs;
		// Lossy dedup: eviction costs a harmless duplicate ref, never a missed one.
		static constexpr unsigned REF_DEDUP_SIZE = 64; // Must be a power of two
		uint64_t ref_dedup[REF_DEDUP_SIZE] = {};

		bool mark_referenced(uint64_t object_id) noexcept {
			uint64_t &slot = ref_dedup[object_id & (REF_DEDUP_SIZE - 1)];
			if (slot == object_id)
				return false;
			slot = object_id;
			return true;
		}
		void clear_referenced() noexcept {
			for (uint64_t &slot : ref_dedup)
				slot = 0;
		}

		void append(Variant &&value);
		/// Reserve for max_refs and drop what the previous program scoped. No reserve-only
		/// variant: growing the reservation reallocates variants, dangling scoped_variants.
		void reinitialize(unsigned level, unsigned max_refs);
		void reset();
		bool is_mutable_variant(const Variant &var) const;
	};
	struct LookupEntry {
		String name;
		gaddr_t address;
	};
	/// @brief A restriction callback, paired with a cached copy of its validity.
	/// @note Callable::is_valid() crosses into Godot and looks the target up in the object
	/// database, which is far more work than a check guarding every single API call can
	/// afford. These callables are only ever replaced through their setters, so the flag
	/// is simply refreshed there.
	struct RestrictionCallback {
		Callable callable;
		bool valid = false;

		RestrictionCallback &operator=(const Callable &p_callable) {
			callable = p_callable;
			valid = callable.is_valid();
			return *this;
		}
		bool is_valid() const noexcept { return valid; }
		template <typename... Args>
		Variant call(Args &&...args) const { return callable.call(std::forward<Args>(args)...); }
	};

	/// @brief A method or property name from guest memory, in both forms the API needs.
	struct CachedName {
		StringName sname;
		Variant variant; // Holds sname

		// Native object calls are overwhelmingly made from a fixed call site to one
		// runtime class. Keep the resolved MethodBind beside that call site's name.
		static constexpr unsigned METHOD_CACHE_SIZE = 4; // Must be a power of two
		struct MethodEntry {
			const std::type_info *object_type = nullptr;
			GDExtensionMethodBindPtr bind = nullptr;
			bool resolved = false;
		};
		MethodEntry methods[METHOD_CACHE_SIZE];
	};
	// Direct-mapped cache of guest method and property names, see cached_guest_name().
	struct GuestNameCache {
		static constexpr unsigned SIZE = 32; // Must be a power of two
		struct Entry {
			gaddr_t address = 0;
			bool terminated = false;
			std::string text;
			CachedName name;
			unsigned pins = 0;
		};
		Entry entries[SIZE];

		void clear() {
			for (Entry &entry : entries)
				entry = Entry{};
		}
	};

	/// @brief A pinned view of a cached guest name.
	///
	/// Object calls and property access can re-enter this Sandbox. Pinning prevents a
	/// colliding nested lookup from replacing the StringName while Godot still uses it;
	/// only that rare collision allocates a temporary uncached name.
	class CachedNameRef {
	public:
		explicit CachedNameRef(GuestNameCache::Entry &entry) noexcept :
				m_name(&entry.name), m_entry(&entry) {
			entry.pins++;
		}
		explicit CachedNameRef(std::unique_ptr<CachedName> owned) noexcept :
				m_name(owned.get()), m_owned(std::move(owned)) {}
		CachedNameRef(const CachedNameRef &) = delete;
		CachedNameRef &operator=(const CachedNameRef &) = delete;
		CachedNameRef(CachedNameRef &&other) noexcept :
				m_name(other.m_name), m_entry(other.m_entry), m_owned(std::move(other.m_owned)) {
			other.m_name = nullptr;
			other.m_entry = nullptr;
		}
		~CachedNameRef() {
			if (m_entry != nullptr)
				m_entry->pins--;
		}

		const CachedName &get() const noexcept { return *m_name; }
		CachedName &get() noexcept { return *m_name; }
		const CachedName *operator->() const noexcept { return m_name; }
		CachedName *operator->() noexcept { return m_name; }

	private:
		CachedName *m_name = nullptr;
		GuestNameCache::Entry *m_entry = nullptr;
		std::unique_ptr<CachedName> m_owned;
	};
	struct ObjectBindingCache {
		static constexpr unsigned SIZE = 32; // Must be a power of two
		struct Entry {
			uint64_t object_id = 0;
			godot::Object *binding = nullptr;
		};
		Entry entries[SIZE];

		Entry &slot(uint64_t object_id) noexcept { return entries[object_id & (SIZE - 1)]; }

		void clear() {
			for (Entry &entry : entries)
				entry = Entry{};
		}
	};
	/// @brief Direct-mapped cache of a function-name String to its guest address.
	/// @note Keyed by the string's own buffer, which a GDScript call site reuses for its
	/// constant argument every time, so a hit costs no engine calls at all. Names built
	/// fresh per call simply miss and fall back to the hash lookup.
	struct NameAddressCache {
		static constexpr unsigned SIZE = 8; // Must be a power of two
		struct Entry {
			// Holding on to the String keeps its buffer alive, so no later string can be
			// handed the address this entry is keyed by.
			String name;
			gaddr_t address = 0;
			bool valid = false;
		};
		Entry entries[SIZE];

		void clear() {
			for (Entry &entry : entries)
				entry = Entry{};
		}
	};
	struct ProfilingState {
		std::unordered_map<gaddr_t, int> hotspots;
		std::vector<LookupEntry> lookup;
	};

	Sandbox();
	Sandbox(const PackedByteArray &buffer);
	Sandbox(Ref<ELFScript> program);
	~Sandbox();
	static void Initialize();
	/// @brief Join every background translation thread.
	/// @note Background translations run code that lives inside this extension,
	/// so they must all be finished before the extension can be unloaded.
	static void Deinitialize();

	static Sandbox *FromBuffer(const PackedByteArray &buffer) { return memnew(Sandbox(buffer)); }
	static Sandbox *FromProgram(Ref<ELFScript> program) { return memnew(Sandbox(std::move(program))); }

	// -= VM function calls =-

	/// @brief Make a function call to a function in the guest by its name.
	/// @param args The arguments to pass to the function, where the first argument is the name of the function.
	/// @param arg_count The number of arguments.
	/// @param error The error code, if any.
	/// @return The return value of the function call.
	Variant vmcall(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);
	/// @brief Make a function call to a function in the guest by its name. Always use Variant values for arguments.
	/// @param args The arguments to pass to the function, where the first argument is the name of the function.
	/// @param arg_count The number of arguments.
	/// @param error The error code, if any.
	/// @return The return value of the function call.
	Variant vmcallv(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);
	/// @brief Make a function call to a function in the guest by its name.
	/// @param function The name of the function to call.
	/// @param args The arguments to pass to the function.
	/// @param arg_count The number of arguments.
	/// @return The return value of the function call.
	Variant vmcall_fn(const StringName &function, const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);
	/// @brief Make a function call to a function in the guest by its guest address.
	/// @param address The address of the function to call.
	/// @param args The arguments to pass to the function.
	/// @param arg_count The number of arguments.
	/// @param error The error code, if any.
	/// @return The return value of the function call.
	Variant vmcall_address(gaddr_t address, const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);

	/// @brief Make a function call to a function in the guest by its name.
	/// @param function The name of the function to call.
	/// @param args The arguments to pass to the function.
	/// @return The return value of the function call.
	/// @note The extra arguments are saved in the callable object, and will be passed to the function when it is called
	/// in front of the arguments passed to the call() method. So, as an example, if you have a function that takes 3 arguments,
	/// and you call it with 2 arguments, you can later call the callable object with one argument, which turns into the 3rd argument.
	Variant vmcallable(String function, Array args);
	Variant vmcallable_address(uint64_t address, Array args);

	/// @brief Set whether to prefer register values for VM function calls.
	/// @param use_unboxed_arguments True to prefer register values, false to prefer Variant values.
	void set_unboxed_arguments(bool use_unboxed_arguments) { m_use_unboxed_arguments = use_unboxed_arguments; }
	/// @brief Get whether to prefer register values for VM function calls.
	/// @return True if register values are preferred, false if Variant values are preferred.
	bool get_unboxed_arguments() const { return m_use_unboxed_arguments; }

	/// @brief Set whether to use precise simulation for VM execution.
	/// @param use_precise_simulation True to use precise simulation, false to use fast simulation.
	void set_precise_simulation(bool use_precise_simulation) { m_precise_simulation = use_precise_simulation; }

	/// @brief Get whether to use precise simulation for VM execution.
	/// @return True if precise simulation is used, false otherwise.
	bool get_precise_simulation() const { return m_precise_simulation; }

	/// @brief Set whether or not to enable profiling of the guest program.
	/// @param enable True to enable profiling, false to disable it.
	void set_profiling(bool enable);

	/// @brief Get whether profiling of the guest program is enabled.
	/// @return True if profiling is enabled, false otherwise.
	bool get_profiling() const { return m_profiling_enabled; }

	/// @brief  Check if the sandbox is currently initializing (running through main()).
	/// @return True if the sandbox is initializing, false otherwise.
	/// @note This is used to enforce or prevent certain operations from being performed during initialization.
	/// For example, it's only possible to add properties or public API functions to the sandbox during initialization.
	bool is_initializing() const { return m_is_initialization; }

	// -= Sandbox Properties =-

	uint32_t get_max_refs() const { return m_max_refs; }
	void set_max_refs(uint32_t max);
	void set_memory_max(uint32_t max);
	uint32_t get_memory_max() const { return m_memory_max; }
	void set_instructions_max(int64_t max) { m_insn_max = max; }
	int64_t get_instructions_max() const { return m_insn_max; }
	void set_allocations_max(int64_t max);
	int64_t get_allocations_max() const { return m_allocations_max; }
	int64_t get_heap_usage() const;
	int64_t get_heap_chunk_count() const;
	int64_t get_heap_allocation_counter() const;
	int64_t get_heap_deallocation_counter() const;
	void set_exceptions(unsigned exceptions) {} // Do nothing (it's a read-only property)
	unsigned get_exceptions() const { return m_exceptions; }
	void set_timeouts(unsigned budget) {} // Do nothing (it's a read-only property)
	unsigned get_timeouts() const { return m_timeouts; }
	void set_calls_made(unsigned calls) {} // Do nothing (it's a read-only property)
	unsigned get_calls_made() const { return m_calls_made; }

	static uint64_t get_global_timeouts() { return m_global_timeouts; }
	static uint64_t get_global_exceptions() { return m_global_exceptions; }
	static uint64_t get_global_calls_made() { return m_global_calls_made; }

	/// @brief Get the global instance count of all sandbox instances.
	/// @return The global instance count.
	static uint64_t get_global_instance_count() { return m_global_instances_current; }

	/// @brief Get the globally accumulated startup time of all sandbox instantiations.
	/// @return The accumulated startup time.
	static double get_accumulated_startup_time() { return m_accumulated_startup_time; }

	// -= Address Lookup =-

	gaddr_t address_of(const String &symbol) const;

	gaddr_t cached_address_of(int64_t hash, const String &name) const;

	/// @brief Look up a guest function address by StringName, without touching the engine.
	/// @note The String-keyed cache needs a String to hash, and building one from a
	/// StringName allocates. Method names arrive as StringNames on every script call, so
	/// they get their own cache keyed by the StringName's own identity.
	gaddr_t cached_address_of(const StringName &name) const;

	/// @brief Look up a guest function address from a name held in a Variant.
	/// @note vmcall() and friends receive the name as a Variant, and reaching the
	/// String-keyed cache from there costs a String construction plus a hash that re-walks
	/// the characters on every call.
	gaddr_t cached_address_of_variant(const Variant &name) const;

	String lookup_address(gaddr_t address) const;

	/// @brief Check if a function exists in the guest program.
	/// @param p_function The name of the function to check.
	/// @return True if the function exists, false otherwise.
	bool has_function(const StringName &p_function) const;

	/// @brief Add a hash to address mapping to the cache.
	/// @param name The name of the function or symbol.
	/// @param address The address of the function or symbol.
	void add_cached_address(const String &name, gaddr_t address) const;

	// -= Public API =-

	/// @brief Register a guest-published API function.
	/// @param func MethodInfo dictionary from create_public_api_function().
	void add_public_api_function(Dictionary &&func);

	/// @brief Deep copy of the guest's public API (Array of MethodInfo dictionaries).
	Array get_public_api() const { return m_public_api_functions.duplicate(true); }

	/// @brief Names of the guest's registered public API functions.
	PackedStringArray get_functions() const;

	// -= Guest Name Cache =-

	/// @brief Turn a method or property name from guest memory into a Godot name.
	/// @param address The guest address the name was read from, used as the cache key.
	/// @param name The name as it currently reads in guest memory, without any terminator.
	/// @param terminated True if the byte following the name in guest memory is a NUL.
	/// @return The name, owned by the cache and valid only until the next lookup that
	/// lands in the same cache slot. Callers that can re-enter the sandbox in between
	/// (an allowed-method callback, a call that reaches a script) must take a copy.
	/// @note Building a StringName means hashing the text and taking a global lock in
	/// Godot's string-name table, which is far too expensive to repeat on every single
	/// object call. Guests overwhelmingly pass a pointer to a string literal, so the
	/// address is a stable key, and the text is stored alongside it and re-compared to
	/// stay correct for guests that build names at run-time in a reused buffer.
	CachedNameRef cached_guest_name(gaddr_t address, std::string_view name, bool terminated) const;

	// -= Call State Management =-

	/// @brief Get the current call state.
	/// @return The current call state.
	/// @note The call state is a stack of states, with the current state stored in m_current_state.
	auto &state() const { return *m_current_state; }
	auto &state() { return *m_current_state; }

	/// @brief Set the current tree base, which is the node that the sandbox will use for accessing the node tree.
	/// @param tree_base The tree base node.
	/// @note The tree base is the owner node that the sandbox will use to access the node tree. When scripts
	/// try to access the node path ".", they will be accessing this node, and navigating relative to it.
	void set_tree_base(godot::Node *tree_base);
	godot::Node *get_tree_base() const;

	godot::ObjectID get_tree_base_id() const noexcept { return this->m_tree_base; }
	void set_tree_base_id(godot::ObjectID tree_base) noexcept { this->m_tree_base = tree_base; }

	/// @brief The SafeGDScript this machine was made for, when one was. Zero for
	/// a plain Sandbox node running an ELF: nothing there owns Script resources.
	godot::ObjectID get_script_owner_id() const noexcept { return this->m_script_owner; }
	void set_script_owner_id(godot::ObjectID script) noexcept { this->m_script_owner = script; }

	bool has_instance_records() const noexcept { return m_instance_record_size != 0; }
	gaddr_t get_instance_record_size() const noexcept { return m_instance_record_size; }
	gaddr_t get_default_instance_base() const noexcept { return m_default_instance_base; }

	gaddr_t get_instance_base() const noexcept { return m_instance_base; }
	void set_instance_base(gaddr_t base) noexcept { this->m_instance_base = base; }

	uint64_t get_program_generation() const noexcept { return m_program_generation; }

	gaddr_t create_instance_record();

	void destroy_instance_record(gaddr_t base);

	bool is_live_instance_base(gaddr_t base) const noexcept {
		return base == 0 || base == m_default_instance_base ||
				m_live_instance_records.count(base) != 0;
	}

	gaddr_t rebase_instance_address(gaddr_t address) const noexcept;

	// -= Scoped objects and variants =-

	/// @brief Add a scoped variant to the current state.
	/// @param var The variant to add.
	/// @return The index of the added variant, passed to and used by the guest.
	unsigned add_scoped_variant(const Variant *var) const;

	/// @brief Create a new scoped variant, storing it in the current state.
	/// @param var The variant to add.
	/// @return The index of the added variant, passed to and used by the guest.
	unsigned create_scoped_variant(Variant &&var) const;

	/// @brief Get a scoped variant by its index.
	/// @param idx The index of the variant to get.
	/// @return The variant, or an empty optional if the index is invalid.
	inline std::optional<const Variant *> get_scoped_variant(int32_t idx) const noexcept {
		if (LIKELY(idx >= 0 && size_t(idx) < state().scoped_variants.size()))
			return state().scoped_variants[idx];
		return get_scoped_variant_uncommon(idx);
	}
	std::optional<const Variant *> get_scoped_variant_uncommon(int32_t idx) const noexcept;

	/// @brief Get a mutable scoped variant by its index.
	/// @param idx The index of the variant to get.
	/// @return The variant.
	Variant &get_mutable_scoped_variant(int32_t idx);

	/// @brief Create a new permanent variant, storing it in the current state.
	/// @param idx The index of the variant to duplicate or move.
	/// @return The index of the new permanent variant, passed to and used by the guest.
	unsigned create_permanent_variant(unsigned idx);

	/// Create a permanent variant from a value. Returns 0 when full.
	int32_t create_permanent_variant_from(Variant &&var);

	/// Release a permanent variant, recycling its slot.
	void release_permanent_variant(int32_t idx);

	/// @brief Check if a variant index is a permanent variant.
	/// @param idx The index of the variant to check.
	/// @return True if the variant is permanent, false otherwise.
	static bool is_permanent_variant(int32_t idx) noexcept { return idx < 0 && idx != INT32_MIN; }

	/// @brief Assign a permanent variant index with a new variant.
	/// @param idx The index of the permanent variant to assign.
	/// @param var The new variant to move-assign.
	void assign_permanent_variant(int32_t idx, Variant &&var);

	// -= Coroutines =-

	/// A suspended guest coroutine. Level-independent.
	struct Coroutine {
		uint64_t id = 0;
		uint64_t generation = 0; // Stale generation → drop on resume.
		gaddr_t resume_address = 0;
		godot::ObjectID owner;
		gaddr_t instance_base = 0;
		std::vector<uint8_t> frame; // Variant slot array, copied out of guest memory.
		std::vector<int32_t> promoted; // Permanent Variant indices, released on completion.
		struct FrameObject {
			uint32_t offset = 0;
			uint64_t object_id = 0; // ObjectID; re-resolved on resume.
		};
		std::vector<FrameObject> objects;
		std::vector<Ref<RefCounted>> refs;
		int32_t state_index = 0;
		int32_t result_offset = -1;
		Variant sent;
		bool running = false; // Guards against self-resume.
		uint64_t awaited_object_id = 0;
		StringName awaited_signal;
		Ref<SandboxFunctionState> state_object;
	};

	bool coroutine_resume(uint64_t id, const Variant &sent);
	Coroutine *find_coroutine(uint64_t id) noexcept;
	int64_t get_coroutine_count() const noexcept { return int64_t(m_coroutines.size()); }
	/// Clamped, not truncated: 1 << 32 would narrow to 0 and fail every await.
	void set_max_coroutines(int64_t max) { m_max_coroutines = uint32_t(std::clamp<int64_t>(max, 0, MAX_COROUTINE_LIMIT)); }
	int64_t get_max_coroutines() const noexcept { return int64_t(m_max_coroutines); }
	/// Reap all coroutines. Called on reset, program load, and node teardown.
	void reap_coroutines() { this->reap_coroutines_internal(true); }

	void reap_coroutines_for_instance(gaddr_t instance_base);

	/// ECALL_AWAIT handler. Returns true if suspended.
	bool coroutine_suspend(gaddr_t operand_addr, gaddr_t frame_base, uint32_t frame_size,
			int32_t state_index, gaddr_t resume_address, int32_t result_offset);

	/// ECALL_AWAIT_RESTORE handler. Returns state index.
	int32_t coroutine_restore(gaddr_t frame_base, uint32_t frame_size);

	/// @brief Assign a value to the guest's Variant slot, reusing it when owned,
	/// allocating a new scoped Variant otherwise. Owned = permanent state or
	/// current state's vector. Non-owned slots (eg. caller arguments) are never
	/// written through.
	/// @return The index the guest uses from here on (assign_to_idx when reused).
	unsigned try_reuse_assign_variant(int32_t assign_to_idx, Variant &&var);

	/// @brief Read-then-write overload: reuses the slot only when assign_to_idx
	/// is src_idx and the Variant is owned. Falls through to the overload above.
	/// @return The index the guest uses from here on.
	unsigned try_reuse_assign_variant(int32_t src_idx, const Variant &src_var, int32_t assign_to_idx, const Variant &var);

	static uintptr_t engine_ptr(const godot::Object *obj) noexcept { return obj != nullptr ? uintptr_t(obj->_owner) : 0u; }

	/// @brief The ObjectID Godot assigned to an object. Unlike an address it is never
	/// handed out twice, which is what makes it usable as a long-lived key.
	/// @note Taken straight from the engine rather than through Object::get_instance_id(),
	/// which is a bound method call and cannot be asked about an object that is already gone.
	static uint64_t engine_object_id(const godot::Object *obj) noexcept;

	static uint64_t engine_ptr_object_id(uintptr_t engine_object) noexcept;

	// Bit 62 tag distinguishes handles from variant indices. Change if Godot claims it.
	static constexpr uint64_t OBJECT_HANDLE_TAG = uint64_t(1) << 62;

	static constexpr uint64_t object_handle_from_id(uint64_t object_id) noexcept {
		return object_id != 0 ? (object_id | OBJECT_HANDLE_TAG) : 0u;
	}

	static constexpr uint64_t object_id_from_handle(uint64_t handle) noexcept {
		return (handle & OBJECT_HANDLE_TAG) != 0 ? (handle & ~OBJECT_HANDLE_TAG) : 0u;
	}

	/// @brief Differentiate a 64-bit guest handle vs an object handle.
	/// Variants are int32 values sign-extended to 64 bits, and permanent variants
	/// are negative int32 values. Hence, the weird-looking check.
	static constexpr bool is_variant_index_handle(uint64_t handle) noexcept {
		return uint64_t(int64_t(int32_t(handle))) == handle;
	}

	/// @brief Scope an object for the duration of the current call, keeping it alive if
	/// it is RefCounted.
	/// @param obj The object to scope.
	/// @return The handle the guest uses to refer to it.
	uint64_t add_scoped_object(godot::Object *obj);

	/// @brief Scope an object the caller already knows outlives the call, by its engine
	/// pointer, skipping the binding lookup add_scoped_object() would need. The binding is
	/// resolved later, and only if the guest uses the handle.
	/// @return The handle the guest uses to refer to it.
	uint64_t add_scoped_engine_object(uintptr_t engine_object);

	/// @brief Remove a scoped object from the current state.
	void rem_scoped_object(const godot::Object *obj);

	/// @brief Find a scoped object in the current state.
	CurrentState::ScopedObject *find_scoped_object(uint64_t object_id) const noexcept {
		for (CurrentState::ScopedObject &so : state().scoped_objects)
			if (so.object_id == object_id)
				return &so;
		return nullptr;
	}

	/// @brief Check if an object is scoped in the current state.
	bool is_scoped_object(const godot::Object *obj) const noexcept { return find_scoped_object(engine_object_id(obj)) != nullptr; }

	/// @brief Resolve a guest handle to a usable object, for a handle already known to be
	/// scoped by this call.
	/// @return The godot-cpp object, resolving and remembering the binding if needed.
	static godot::Object *resolve_scoped_object(CurrentState::ScopedObject &so);

	void retain_global_object(gaddr_t slot_address);
	void store_into_guest_slot(gaddr_t slot_address, godot::Variant &&value);
	void release_retained_objects(gaddr_t base, gaddr_t size);
	godot::Object *resolve_live_object(uint64_t object_id) const noexcept;

	// -= Sandbox Restrictions =-

	/// @brief Enable *all* restrictions on the sandbox, restricting access to
	/// external classes, objects, object methods, object properties, and resources.
	/// In effect, all external access is disabled.
	void set_restrictions(bool enabled);

	/// @brief Check if restrictions are enabled on the sandbox.
	/// @return True if *all* restrictions are enabled, false otherwise.
	bool get_restrictions() const;

	/// @brief Add an object to the list of allowed objects.
	/// @param obj The object to add.
	/// @note The entry is keyed by ObjectID, and a RefCounted is kept alive by it, so
	/// that a freed object cannot hand its address to an unrelated one still on the list.
	void add_allowed_object(godot::Object *obj);

	/// @brief Remove an object from the list of allowed objects.
	/// @param obj The object to remove.
	/// @note If the list becomes empty, all objects are allowed.
	void remove_allowed_object(godot::Object *obj);

	/// @brief Clear the list of allowed objects.
	void clear_allowed_objects();

	/// @brief Check if an object is allowed in the sandbox.
	/// @note An empty allowed-objects list with no callback set means unrestricted, so
	/// this answers true for anything. Only use it on an object the host handed us; for
	/// an id that came from the guest, see is_explicitly_allowed_object().
	bool is_allowed_object(godot::Object *obj) const;

	bool is_object_access_unrestricted() const noexcept {
		return m_allowed_objects.empty() && !m_just_in_time_allowed_objects.is_valid();
	}

	bool is_fully_unrestricted() const noexcept {
		return !m_just_in_time_allowed_classes.is_valid()
				&& !m_just_in_time_allowed_methods.is_valid()
				&& !m_just_in_time_allowed_properties.is_valid()
				&& !m_just_in_time_allowed_resources.is_valid()
				&& is_object_access_unrestricted();
	}

	/// @brief Check if an ObjectID is on the allowed-objects list.
	bool is_allowed_object_id(uint64_t object_id) const noexcept { return m_allowed_objects.find(object_id) != m_allowed_objects.end(); }

	bool is_explicitly_allowed_object(godot::Object *obj) const;
	godot::Object *get_explicitly_allowed_object(uint64_t object_id) const;

	/// @brief Set a callback to check if an object is allowed in the sandbox.
	/// @param callback The callable to check if an object is allowed.
	void set_object_allowed_callback(const Callable &callback);

	/// @brief Check if a class name is allowed in the sandbox.
	bool is_allowed_class(const String &name) const;

	// True when set_restrictions()'s blanket refusal is in effect (not a per-name callback).
	bool is_class_access_restricted() const;

	/// @brief Set a callback to check if a class is allowed in the sandbox.
	/// @param callback The callable to check if a class is allowed.
	void set_class_allowed_callback(const Callable &callback);

	/// @brief Check if a resource is allowed in the sandbox.
	bool is_allowed_resource(const String &path) const;

	/// @brief Set a callback to check if a resource is allowed in the sandbox.
	/// @param callback The callable to check if a resource is allowed.
	void set_resource_allowed_callback(const Callable &callback);

	/// @brief Check if accessing a method on an object is allowed in the sandbox.
	/// @param method The name of the method to check.
	/// @return True if the method is allowed, false otherwise.
	/// @note Inline, and kept down to a single load in the common case: this guards
	/// every object call the guest makes.
	bool is_allowed_method(godot::Object *obj, const Variant &method) const {
		// If the callable is not set, all methods are allowed
		if (LIKELY(!m_just_in_time_allowed_methods.is_valid()))
			return true;
		return m_just_in_time_allowed_methods.call(this, obj, method);
	}

	/// @brief Overload for the API call sites that name a method with a string literal.
	/// @note Taking the name as a Variant would build a heap-allocated String on every
	/// single call, only to throw it away unused whenever no callback is installed --
	/// which is the overwhelmingly common case. Deferring construction keeps the guard
	/// down to the same single load as the Variant overload.
	bool is_allowed_method(godot::Object *obj, const char *method) const {
		if (LIKELY(!m_just_in_time_allowed_methods.is_valid()))
			return true;
		return m_just_in_time_allowed_methods.call(this, obj, String(method));
	}

	/// @brief Set a callback to check if a method is allowed in the sandbox.
	/// @param callback The callable to check if a method is allowed.
	void set_method_allowed_callback(const Callable &callback);

	/// @brief Check if accessing a property on an object is allowed in the sandbox.
	/// @param obj The object to check.
	/// @param property The name of the property to check.
	/// @return True if the property is allowed, false otherwise.
	/// @note Inline for the same reason as is_allowed_method().
	bool is_allowed_property(godot::Object *obj, const Variant &property, bool is_set) const {
		// If the callable is not set, all properties are allowed
		if (LIKELY(!m_just_in_time_allowed_properties.is_valid()))
			return true;
		return m_just_in_time_allowed_properties.call(this, obj, property, is_set);
	}

	/// @brief Overload for the API call sites that name a property with a string literal.
	/// @note Inline for the same reason as the is_allowed_method() overload above.
	bool is_allowed_property(godot::Object *obj, const char *property, bool is_set) const {
		if (LIKELY(!m_just_in_time_allowed_properties.is_valid()))
			return true;
		return m_just_in_time_allowed_properties.call(this, obj, String(property), is_set);
	}

	/// @brief Set a callback to check if a property is allowed in the sandbox.
	/// @param callback The callable to check if a property is allowed.
	void set_property_allowed_callback(const Callable &callback);

	/// @brief A falsy function used when restrictions are enabled.
	/// @return Always returns false.
	static bool restrictive_callback_function(Variant) { return false; }

	// -= Sandboxed Properties =-
	// These are properties that are exposed to the Godot editor, provided by the guest program.

	/// @brief Add a property to the sandbox.
	/// @param name The name of the property.
	/// @param vtype The type of the property.
	/// @param setter The guest address of the setter function.
	/// @param getter The guest address of the getter function.
	/// @param def The default value of the property.
	void add_property(const String &name, Variant::Type vtype, gaddr_t setter, gaddr_t getter, const Variant &def = "") const;
	void add_property(const String &name, Variant::Type vtype, gaddr_t address, const Variant &def = "") const;
	void set_property_hint(const String &name, uint32_t hint, const String &hint_string, uint32_t usage) const;

	/// @brief Set a property in the sandbox.
	/// @param name The name of the property.
	/// @param value The new value to set.
	bool set_property(const StringName &name, const Variant &value);

	/// @brief Get a property from the sandbox.
	/// @param name The name of the property.
	/// @param r_ret The current value of the property.
	bool get_property(const StringName &name, Variant &r_ret);

	/// @brief Get a property from the sandbox.
	/// @param name The name of the property.
	/// @return The current value of the property.
	Variant get(const StringName &name);

	/// @brief Set a property in the sandbox.
	/// @param name The name of the property.
	/// @param value The new value to set.
	void set(const StringName &name, const Variant &value);

	/// @brief Get a list of properties.
	/// @return The list of properties.
	Array get_property_list() const;

	/// @brief Find a property in the sandbox, or return null if it does not exist.
	/// @param name The name of the property.
	/// @return The property, or null if it does not exist.
	const SandboxProperty *find_property_or_null(const StringName &name) const;

	/// @brief Get all sandboxed properties.
	/// @return The array of sandboxed properties.
	const std::vector<SandboxProperty> &get_properties() const { return m_properties; }

	/// @brief Get the list of sandbox properties as a dictionary.
	/// @note These are unrelated to SandboxProperty objects. It's all the properties that are exposed to the Godot editor.
	/// @return The dictionary of sandbox properties.
	static std::vector<PropertyInfo> create_sandbox_property_list();

	// -= Program management & public functions =-

	/// @brief Check if a program has been loaded into the sandbox.
	/// @return True if a program has been loaded, false otherwise.
	bool has_program_loaded() const;
	/// @brief Set the program to run in the sandbox.
	/// @param program The program to load and run.
	void set_program(Ref<ELFScript> program);
	/// Clear the generated `is Trait` object-result caches after changing scripts
	/// on objects that a running program may test again.
	void clear_trait_caches();
	/// @brief Get the program loaded into the sandbox.
	/// @return The program loaded into the sandbox.
	Ref<ELFScript> get_program();

	/// @brief Load a program from a buffer into the sandbox.
	/// @param buffer The buffer containing the program.
	void load_buffer(const PackedByteArray &buffer);

	/// @brief Reset the sandbox, clearing all state and reloads the program.
	void reset(bool unload = false);

	struct BinaryInfo {
		String language;
		PackedStringArray functions;
		int version = 0;
		bool has_script_metadata = false;
		gdscript::ScriptMetadata script_metadata;
	};
	/// @brief Get information about the program from the binary.
	/// @param binary The binary data.
	/// @return An array of public callable functions and programming language.
	static BinaryInfo get_program_info_from_binary(const PackedByteArray &binary);

	/// @brief Decode the ".gdsmeta" section of a program binary into a Dictionary.
	static Dictionary get_program_metadata(const PackedByteArray &binary);

	/// @brief Check if a function is Sandbox-specific (and public API).
	/// @param p_function The name of the function to check.
	/// @return True if the function is Sandbox-specific, false otherwise.
	bool is_sandbox_function(const StringName &p_function) const;

	// -= Profiling & Hotspots =-

	/// @brief Generate the top N hotspots from profiling recorded so far.
	/// @param total The maximum number of hotspots to generate.
	/// @param callable A callback that must resolve an address of an unknown program, given elf_hint and an address as arguments.
	/// @return The top hotspots recorded globally so far, sorted by the number of hits.
	static Array get_hotspots(unsigned total = 10, const Callable &callable = {});

	/// @brief Clear all recorded hotspots.
	static void clear_hotspots();

	/// @brief Enable or disable profiling of the guest program.
	/// @param enable True to enable profiling, false to disable it.
	/// @param interval The interval in instructions between each profiling update. This interval
	/// is accumulated so that even if a function returns early, the interval is still counted.
	void enable_profiling(bool enable, uint32_t interval = 500);

	// Called from the profiling property setter to rebuild instrumented programs.
	using ProfilingToggle = void (*)(Sandbox &sandbox, bool enabled);
	static void set_profiling_toggle_callback(ProfilingToggle callback) { m_profiling_toggle = callback; }

	// True when the loaded program exports its own profiling data area.
	bool has_self_instrumentation() const;

	// -= Self-testing, inspection and internal functions =-

	/// @brief Get the current Callable set for redirecting stdout.
	/// @return The current Callable set for redirecting stdout.
	const Callable &get_redirect_stdout() const { return m_redirect_stdout; }

	/// @brief Set a Callable to redirect stdout from the guest program to.
	/// @param callback The callable to redirect stdout. It receives one String
	/// per guest print() call, already concatenated the way Godot's print()
	/// concatenates its arguments.
	void set_redirect_stdout(const Callable &callback) { m_redirect_stdout = callback; }

	/// @brief Get the 32 integer registers of the RISC-V machine.
	/// @return An array of 32 registers.
	Array get_general_registers() const;

	/// @brief Get the 32 floating-point registers of the RISC-V machine.
	/// @return An array of 32 registers.
	Array get_floating_point_registers() const;

	/// @brief Set the 8 argument registers of the RISC-V machine, A0-A7.
	/// @param args The arguments to set.
	void set_argument_registers(Array args);

	/// @brief Get the current instruction being executed, as a string.
	/// @return The current instruction.
	String get_current_instruction() const;

	/// @brief Enable resuming the program execution after a timeout.
	/// @note Must be called before the program is run. Not available for VM calls.
	void make_resumable();

	/// @brief Resume execution of the program. Loses the current call state.
	bool resume(uint64_t max_instructions);

	/// @brief Binary translate the program and produce embeddable code
	/// @param ignore_instruction_limit If true, ignore the instruction limit. Infinite loops are possible.
	/// @param automatic_nbit_as If true, use and-masking on all memory accesses based on the rounded-down Po2 arena size.
	/// @return The binary translation code.
	/// @note This is only available if the RISCV_BINARY_TRANSLATION flag is set.
	/// @warning Do *NOT* enable automatic_nbit_as unless you are sure the program is compatible with it.
	String emit_binary_translation(bool ignore_instruction_limit = false, bool automatic_nbit_as = false) const;

	/// @brief Hash of the current execute segment and every ABI-affecting translator option.
	/// @return Zero when no program/translator is available.
	int64_t get_translation_hash() const;

	/// @brief Build the cache-loadable shared-library variant for this machine.
	/// @param out_dir Destination directory, or empty for the configured cache.
	/// @return Absolute hash-named library path, or an empty String on failure.
	String bake_binary_translation(const String &out_dir = "") const;

	/// @brief Whether the configured cache contains this machine's translation.
	bool is_translation_baked() const;

	/// @brief Queue a release SafeGDScript ELF for background baking.
	/// @note C++ integration helper; the copy is independent of any live machine.
	static void queue_binary_translation_bake(PackedByteArray binary, uint32_t memory_max);
	/// @brief Return background auto-bake counters for editor integration tests.
	static Dictionary _get_auto_bake_stats();

	/// @brief Open a shared library, which should self-register its functions.
	/// @param shared_library_path The path to the shared library.
	/// @param allow_insecure If true, allow loading shared libraries after other Sandbox instances have been created.
	/// @note This is not a general-purpose function for loading shared libraries. It is only a
	/// convenience helper function for loading shared libraries that self-register their functions.
	static bool load_binary_translation(const String &shared_library_path, bool allow_insecure = false);

	/// @brief Try to emit the binary translation code, and then compile it. Does not load the binary translation.
	/// @note For security reasons, the binary translation is not loaded automatically. A game restart is required,
	/// as binary translations can only be loaded before any Sandbox instances are created.
	/// @return True if the binary translation was emitted and compiled successfully, false otherwise.
	bool try_compile_binary_translation(String shared_library_path = "res://bintr", const String &cc = "cc", const String &extra_cflags = "", bool ignore_instruction_limit = false, bool automatic_nbit_as = false);

	/// @brief  Check if the program has found and loaded binary translation.
	/// @return True if binary translation is loaded, false otherwise.
	bool is_binary_translated() const;

	/// @brief Check if the program has a binary translation produced by a JIT compiler.
	/// @note is_binary_translated() will return true if the program has a binary translation,
	/// regardless of whether it was produced by a JIT- or a system-compiler.
	/// @return True if the program has a JIT-compiled binary translation, false otherwise.
	bool is_jit() const;

	/// @brief Set whether to automatically use nbit-as for binary translation.
	/// @param automatic_nbit_as If true, use nbit-as for binary translation.
	/// @note Ignored when the active binary translation backend has no such option.
	/// @warning Do *NOT* enable this unless you are sure the program is compatible with it.
	void set_binary_translation_automatic_nbit_as(bool automatic_nbit_as) {
		this->m_bintr_automatic_nbit_as = automatic_nbit_as;
	}
	bool get_binary_translation_automatic_nbit_as() const {
		return this->m_bintr_automatic_nbit_as;
	}

	/// @brief Set whether to use register caching for binary translation.
	/// @param register_caching If true, use register caching for binary translation.
	/// @note Ignored when the active binary translation backend has no such option.
	/// The asmjit backend always caches registers.
	void set_binary_translation_register_caching(bool register_caching) {
		this->m_bintr_register_caching = register_caching;
	}
	bool get_binary_translation_register_caching() const {
		return this->m_bintr_register_caching;
	}

	/// @brief Set whether to perform binary translation in the background.
	/// @param bg_compilation If true, perform binary translation in the background.
	/// @note Ignored when the active binary translation backend has no such option.
	/// The asmjit backend translates synchronously.
	void set_binary_translation_bg_compilation(bool bg_compilation) {
		this->m_bintr_bg_compilation = bg_compilation;
	}
	bool get_binary_translation_bg_compilation() const {
		return this->m_bintr_bg_compilation;
	}

	/// @brief Opt an unrestricted JIT-enabled sandbox back into bounds-checked memory.
	/// @note Unrestricted guests are unchecked by default, this is the opt-out.
	/// Takes effect at next load. Not script-bound.
	void set_unchecked_memory(bool unchecked) {
		this->m_unchecked_memory = unchecked;
	}
	bool get_unchecked_memory() const noexcept {
		return this->m_unchecked_memory_active;
	}
	// An unrestricted guest runs without bounds checks, when JIT is enabled.
	bool unchecked_memory_wanted() const noexcept {
		return this->m_unchecked_memory && this->is_fully_unrestricted();
	}
	bool unchecked_memory_is_stale() const noexcept {
		return this->m_unchecked_memory_active != this->unchecked_memory_wanted();
	}

	/// @brief Enable or disable the use of JIT-compilation.
	/// @param enable If true, enable JIT-compilation, false to disable it.
	/// @note Ignored when no JIT backend is compiled in. See has_feature_jit().
	static void set_jit_enabled(bool enable) { m_bintr_jit = enable; }

	/// @brief Check if JIT-compilation is enabled.
	/// @return True if JIT-compilation is enabled, false otherwise.
	static bool is_jit_enabled() { return has_feature_jit() && m_bintr_jit; }

	/// @brief Check if any JIT-compiling backend was compiled into the extension.
	static bool has_feature_jit() {
		return riscv::libtcc_enabled || riscv::asmjit_enabled;
	}

	/// @brief Check if the binary translator was compiled into the extension.
	/// @note Only the binary translator can emit, compile and load translations.
	static bool has_feature_binary_translation() {
		return riscv::binary_translation_enabled;
	}

	/// @brief Fuzz the host side of the sandbox API with hostile arguments.
	/// @param test Fuzzing target: "syscalls", "variants" or "all", optionally
	/// suffixed with ":<seed>" to reproduce an earlier run.
	/// @param iterations Number of system calls (or GuestVariants) to throw at it.
	/// @return A Dictionary with the seed used, iterations completed and how many
	/// were refused by throwing.
	/// @warning Leaves guest memory and the Variant state arbitrary. Reset afterwards.
	Dictionary assault(const String &test, int64_t iterations);
	Variant vmcall_internal(gaddr_t address, const Variant **args, int argc);
	// False on level-0 state: vmcall_internal there would reset SP/RA under the initializer.
	bool is_in_vmcall() const noexcept { return m_current_state != &m_states[0]; }
	machine_t &machine() { return *m_machine; }
	const machine_t &machine() const { return *m_machine; }
	/// @brief Print one line to the console, or to the redirect callback.
	/// @param args The Variants making up the line.
	/// @param count How many there are.
	/// The arguments are concatenated with no separator, the way Godot's own
	/// print() concatenates its arguments, and emitted as a single line.
	void print(const Variant *const *args, unsigned count, Print_Channel channel = Print_Channel::PRINT);
	void print(const Variant &v);

	/// @brief Generate the run-time API for the guest program, by iterating through all loaded classes.
	/// @param language The language to generate the API for.
	/// @param header_extra Extra header code to add to the generated API.
	/// @param use_argument_names If true, use argument names with default values in the generated API. Increases the size of the generated API and the compilation time.
	/// @return The generated API code as a string.
	static String generate_api(String language = "cpp", String header_extra = "", bool use_argument_names = false);

	/// @brief Create a MethodInfo dictionary for a public API function.
	/// @param name The name of the function.
	/// @param address The address of the function.
	/// @param description The description of the function.
	/// @param return_type The return type of the function.
	/// @param args The arguments of the function.
	/// @return The MethodInfo dictionary.
	static Dictionary create_public_api_function(std::string_view name, gaddr_t address, std::string_view description, std::string_view return_type, std::string_view args);

	/// @brief Download a named program from the Godot Sandbox programs repository.
	/// @param program_name The name of the program to download. Must be a program built in the Godot Sandbox programs repository.
	/// @return The downloaded program as a byte array.
	static PackedByteArray download_program(String program_name);

private:
	struct BakeOptions {
		bool ignore_limit = false;
		bool nbit_as = false;
		bool unchecked = false;
	};
	BakeOptions current_bake_options() const;
	String emit_binary_translation(const BakeOptions &options, bool shared_library,
			uint32_t *r_hash = nullptr) const;
	static String bake_binary_translation_from_buffer(const PackedByteArray &binary,
			uint32_t memory_max, const BakeOptions &options, const String &out_dir,
			const String &compiler, const String &extra_cflags, bool quiet,
			bool *out_new_file = nullptr);
	static String binary_translation_cache_dir(bool create);
	static String binary_translation_path(uint32_t hash, const String &out_dir = "");
	static bool bintr_lookup_enabled();
	static void start_background_translation(std::function<void()> &&step);
	static void generate_runtime_cpp_api(bool use_argument_names = false);

	void read_instance_layout();
	void run_instance_initializer(gaddr_t address, gaddr_t base);
	void release_instance_record(gaddr_t base);
	void drain_deferred_instance_records();
	void constructor_initialize();
	void full_reset();
	void reset_machine();
	void set_program_data_internal(Ref<ELFScript> program);
	bool load(const PackedByteArray *vbuf, const std::vector<std::string> *argv = nullptr);
	static PackedStringArray get_public_functions(const machine_t &);
	void read_program_properties(bool editor) const;
	void handle_exception(gaddr_t);
	void handle_timeout(gaddr_t);
	void print_backtrace(gaddr_t);
	void initialize_syscalls_runtime();
	static void initialize_syscalls();
	static void initialize_syscalls_2d();
	static void initialize_syscalls_3d();
	GuestVariant *setup_arguments(gaddr_t &sp, const Variant **args, int argc);
	void setup_arguments_native(gaddr_t arrayDataPtr, GuestVariant *v, const Variant **args, int argc);

	machine_t *m_machine = nullptr;
	godot::ObjectID m_tree_base;
	godot::ObjectID m_script_owner;
	uint32_t m_max_refs = MAX_REFS;
	uint32_t m_memory_max = MAX_VMEM;
	gaddr_t m_instance_base = 0;
	gaddr_t m_default_instance_base = 0;
	gaddr_t m_instance_record_size = 0;
	gaddr_t m_instance_init_address = 0;
	std::unordered_set<gaddr_t> m_live_instance_records;
	std::vector<gaddr_t> m_deferred_instance_records;
	int64_t m_insn_max = MAX_INSTRUCTIONS;
	uint32_t m_allocations_max = MAX_HEAP_ALLOCS;

	uint8_t m_throttled = 0;
	bool m_use_unboxed_arguments = false;
	bool m_resumable_mode = false; // If enabled, allow running startup in small increments
	bool m_precise_simulation = false; // Run simulation in the slower, precise mode
	bool m_is_initialization = false; // If true, the program is in the initialization phase
	// These are always present, even when the active binary translation backend
	// has no equivalent option, so that scripts can set them without having to
	// know how the extension was built.
	// On by default: an AND-masked arena confines a stray guest address instead of
	// letting it reach the host, which is what keeps a mistake in a project under
	// development from taking the editor down with it. Unrestricted, it costs ~24%
	// against a wholly unchecked arena; restricted, it is a ~65% speedup over the
	// bounds check it replaces. Requires a Po2 memory_max (see MAX_VMEM).
	bool m_bintr_automatic_nbit_as = true; // Automatic n-bit address space for binary translation
	bool m_bintr_register_caching = true; // Use register caching for binary translation
	bool m_bintr_bg_compilation = true; // Perform binary translation in the background
	bool m_unchecked_memory = true; // Only ever reached while fully unrestricted
	bool m_unchecked_memory_active = false;

	bool add_scoped_entry(uint64_t object_id, uintptr_t engine_object, godot::Object *binding);
	bool hold_unrestricted_object(uint64_t object_id, godot::Object *obj);

	CurrentState *m_current_state = nullptr;
	// State stack, with the permanent (initial) state at index 0.
	// That means eg. static Variant values are held stored in the state at index 0,
	// so that they can be accessed by future VM calls, and not lost when a call ends.
	std::array<CurrentState, MAX_LEVEL> m_states;

	// -= Permanent Variant slots =-
	// Per-slot generation enables recycling; stale indices are refused.
	struct PermanentSlot {
		uint32_t generation = 0;
		int32_t variant_index = -1; // Into m_states[0].variants; -1 = non-owned.
		bool free = false;
	};
	std::vector<PermanentSlot> m_perm_slots;
	std::vector<uint32_t> m_perm_free_slots;

	static constexpr uint32_t PERM_SLOT_BITS = 16;
	static constexpr uint32_t PERM_SLOT_MASK = (1u << PERM_SLOT_BITS) - 1u;
	static constexpr uint32_t PERM_MAX_SLOTS = PERM_SLOT_MASK - 1u;

	// Generation 0 encodes as the old -(slot + 1).
	static constexpr int32_t encode_permanent_index(uint32_t slot, uint32_t generation) noexcept {
		return -int32_t(((generation & 0x7FFFu) << PERM_SLOT_BITS) | ((slot + 1u) & PERM_SLOT_MASK));
	}
	static constexpr uint32_t decode_permanent_slot(int32_t idx) noexcept {
		return (uint32_t(-idx) & PERM_SLOT_MASK) - 1u;
	}
	static constexpr uint32_t decode_permanent_generation(int32_t idx) noexcept {
		return uint32_t(-idx) >> PERM_SLOT_BITS;
	}
	bool permanent_index_valid(int32_t idx) const noexcept;
	int32_t track_permanent_slot(int32_t variant_index);
	void reserve_permanent_state(uint32_t max_refs);

	void promote_frame_handles(Coroutine &co);
	void disconnect_coroutine_signal(Coroutine &co);
	void retire_coroutine(uint64_t id, bool invalidate_state);
	// notify=false suppresses completed emission (used by ~Sandbox).
	void reap_coroutines_internal(bool notify);

	// -= Coroutines =-
	std::vector<std::unique_ptr<Coroutine>> m_coroutines;
	uint64_t m_next_coroutine_id = 1;
	uint32_t m_max_coroutines = MAX_COROUTINES;
	uint64_t m_program_generation = 1; // Bumped on machine replacement.
	uint64_t m_pending_suspend = 0; // Set by ECALL_AWAIT, consumed by vmcall_internal.
	uint64_t m_resuming_coroutine_id = 0; // Id, not pointer: survives reap_coroutines().
	uint64_t m_resume_entry_id = 0; // Names which frame the next vmcall may adopt.

	// Properties
	mutable std::vector<SandboxProperty> m_properties;
	// Guest-published API; authoritative even without an ELFScript resource.
	Array m_public_api_functions;
	mutable std::unordered_map<int64_t, LookupEntry> m_lookup;
	mutable StringNameMap<gaddr_t> m_sname_lookup;
	mutable NameAddressCache m_name_addresses;
	mutable GuestNameCache m_guest_names;
	mutable ObjectBindingCache m_object_bindings;
	std::unordered_map<gaddr_t, Ref<RefCounted>> m_retained_objects;

	// Restrictions
	std::unordered_set<uint64_t> m_allowed_objects;
	// A RefCounted on the allowed list is held here for as long as it is on the list.
	// Nothing else keeps it alive, and once freed its address is free to be reused.
	std::unordered_map<uint64_t, Ref<RefCounted>> m_allowed_object_refs;
	// If an object is not in the allowed list, and a callable is set for the
	// just-in-time allowed objects, it will be called to check if the object is allowed.
	RestrictionCallback m_just_in_time_allowed_objects;
	// If a class is not in the allowed list, and a callable is set for the
	// just-in-time allowed classes, it will be called to check if the class is allowed.
	RestrictionCallback m_just_in_time_allowed_classes;
	// If a callable is set for the just-in-time allowed resources,
	// it will be called to check if access to a resource is allowed.
	RestrictionCallback m_just_in_time_allowed_resources;
	// If a callable is set for allowed methods, it will be called when an object method
	// call is attemped, to check if the method is allowed.
	RestrictionCallback m_just_in_time_allowed_methods;
	// If a callable is set for allowed properties, it will be called when an object property
	// access is attemped, to check if the property is allowed.
	RestrictionCallback m_just_in_time_allowed_properties;

	// Redirections
	Callable m_redirect_stdout;

	Ref<ELFScript> m_program_data;
	PackedByteArray m_program_bytes;
	int m_source_version = -1;

	// Stats
	unsigned m_timeouts = 0;
	unsigned m_exceptions = 0;
	unsigned m_calls_made = 0;

	struct ProfilingData {
		// ELF path -> Address -> Count
		// Anonymous sandboxes are stored as ""
		std::unordered_map<std::string_view, ProfilingState> state;
	};
	static inline std::unique_ptr<ProfilingData> m_profiling_data = nullptr;
	struct LocalProfilingData {
		std::vector<gaddr_t> visited;
		uint32_t profiling_interval = 500;
		uint32_t profiler_icounter_accumulator = 0;
	};
	std::unique_ptr<LocalProfilingData> m_local_profiling_data = nullptr;
	bool m_profiling_enabled = false;
	void update_profiling_sampler(uint32_t interval);
	static inline ProfilingToggle m_profiling_toggle = nullptr;
	static inline std::mutex profiling_mutex;
	static inline std::mutex generate_hotspots_mutex;

	// Global statistics
	static inline uint64_t m_global_timeouts = 0;
	static inline uint64_t m_global_exceptions = 0;
	static inline uint64_t m_global_calls_made = 0;
	static inline uint32_t m_global_instances_current = 0; // Counts the number of current instances
	static inline uint32_t m_global_instances_seen = 0; // Incremented for each instance created
	static inline double m_accumulated_startup_time = 0.0;
	static inline bool m_bintr_jit = riscv::libtcc_enabled || riscv::asmjit_enabled; // JIT compilation enabled
};

inline void Sandbox::CurrentState::append(Variant &&value) {
	variants.push_back(std::move(value));
	scoped_variants.push_back(&variants.back());
}

inline void Sandbox::CurrentState::reset() {
	variants.clear();
	scoped_variants.clear();
	scoped_objects.clear();
	scoped_refs.clear();
	clear_referenced();
}

inline bool Sandbox::is_explicitly_allowed_object(godot::Object *obj) const {
	if (obj == nullptr)
		return false;
	// Check if the object is in the allowed list
	if (is_allowed_object_id(engine_object_id(obj)))
		return true;

	// If the object-allowed callable is set, call it
	if (m_just_in_time_allowed_objects.is_valid())
		return m_just_in_time_allowed_objects.call(this, obj);
	return false;
}

inline bool Sandbox::is_allowed_object(godot::Object *obj) const {
	// If the allowed list is empty, and the allowed-object callback is not set, all objects are allowed
	if (is_object_access_unrestricted())
		return true;
	return is_explicitly_allowed_object(obj);
}
