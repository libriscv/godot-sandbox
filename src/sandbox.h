#pragma once

#include "config.h"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <libriscv/machine.hpp>
#include <optional>

GODOT_NAMESPACE
#define RISCV_ARCH riscv::RISCV64
using gaddr_t = riscv::address_type<RISCV_ARCH>;
using machine_t = riscv::Machine<RISCV_ARCH>;
#include "elf/script_elf.h"
#include "vmcallable.h"
#include "vmproperty.h"

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
	static constexpr unsigned MAX_HEAP = 16ul; // MBs
	static constexpr unsigned MAX_VMEM = 16ul; // MBs
	static constexpr unsigned MAX_LEVEL = 4; // Maximum call recursion depth
	static constexpr unsigned MAX_REFS = 100; // Default maximum number of references
	static constexpr unsigned EDITOR_THROTTLE = 8; // Throttle VM calls from the editor
	static constexpr unsigned MAX_PROPERTIES = 32; // Maximum number of sandboxed properties

	struct CurrentState {
		std::vector<Variant> variants;
		std::vector<const Variant *> scoped_variants;
		std::vector<uintptr_t> scoped_objects;

		void append(Variant &&value);
		void initialize(unsigned level, unsigned max_refs);
		void reinitialize(unsigned level, unsigned max_refs);
		void reset();
		bool is_mutable_variant(const Variant &var) const;
	};

	Sandbox();
	Sandbox(const PackedByteArray &buffer);
	Sandbox(Ref<ELFScript> program);
	~Sandbox();

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
	Variant vmcall_fn(const StringName &function, const Variant **args, GDExtensionInt arg_count);
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
	void set_use_unboxed_arguments(bool use_unboxed_arguments) { m_use_unboxed_arguments = use_unboxed_arguments; }
	/// @brief Get whether to prefer register values for VM function calls.
	/// @return True if register values are preferred, false if Variant values are preferred.
	bool get_use_unboxed_arguments() const { return m_use_unboxed_arguments; }

	/// @brief Set whether to use precise simulation for VM execution.
	/// @param use_precise_simulation True to use precise simulation, false to use fast simulation.
	void set_use_precise_simulation(bool use_precise_simulation) { m_precise_simulation = use_precise_simulation; }

	/// @brief Get whether to use precise simulation for VM execution.
	/// @return True if precise simulation is used, false otherwise.
	bool get_use_precise_simulation() const { return m_precise_simulation; }

	/// @brief Set whether or not to enable profiling of the guest program.
	/// @param enable True to enable profiling, false to disable it.
	void set_profiling(bool enable);

	/// @brief Get whether profiling of the guest program is enabled.
	/// @return True if profiling is enabled, false otherwise.
	bool get_profiling() const { return m_profiling_data != nullptr; }

	// -= Sandbox Properties =-

	uint32_t get_max_refs() const { return m_max_refs; }
	void set_max_refs(uint32_t max);
	void set_memory_max(uint32_t max) { m_memory_max = max; }
	uint32_t get_memory_max() const { return m_memory_max; }
	void set_instructions_max(int64_t max) { m_insn_max = max; }
	int64_t get_instructions_max() const { return m_insn_max; }
	void set_heap_usage(int64_t) {} // Do nothing (it's a read-only property)
	int64_t get_heap_usage() const;
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
	static uint64_t get_global_instance_count() { return m_global_instance_count; }

	/// @brief Get the globally accumulated startup time of all sandbox instantiations.
	/// @return The accumulated startup time.
	static double get_accumulated_startup_time() { return m_accumulated_startup_time; }

	// -= Address Lookup =-

	gaddr_t address_of(std::string_view name) const;
	gaddr_t cached_address_of(int64_t hash, const String &name) const;

	/// @brief Check if a function exists in the guest program.
	/// @param p_function The name of the function to check.
	/// @return True if the function exists, false otherwise.
	bool has_function(const StringName &p_function) const;

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
	void set_tree_base(Node *tree_base) { this->m_tree_base = tree_base; }
	Node *get_tree_base() const { return this->m_tree_base; }

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
	std::optional<const Variant *> get_scoped_variant(int32_t idx) const noexcept;

	/// @brief Get a mutable scoped variant by its index.
	/// @param idx The index of the variant to get.
	/// @return The variant.
	Variant &get_mutable_scoped_variant(int32_t idx);

	/// @brief Create a new permanent variant, storing it in the current state.
	/// @param idx The index of the variant to duplicate or move.
	/// @return The index of the new permanent variant, passed to and used by the guest.
	unsigned create_permanent_variant(unsigned idx);

	/// @brief Check if a variant index is a permanent variant.
	/// @param idx The index of the variant to check.
	/// @return True if the variant is permanent, false otherwise.
	static bool is_permanent_variant(int32_t idx) noexcept { return idx < 0 && idx != INT32_MIN; }

	/// @brief Assign a permanent variant index with a new variant.
	/// @param idx The index of the permanent variant to assign.
	/// @param var The new variant to move-assign.
	void assign_permanent_variant(int32_t idx, Variant &&var);

	/// @brief Try to reuse a variant index for a new variant.
	/// If the index is permanent, assign the new variant directly to it.
	/// If the index is scoped, check if it is mutable (local to the current state) and assign the new variant to it.
	/// If the index immutable, unknown or invalid, create a new scoped variant.
	/// @param idx The index of the variant to assign.
	/// @param var The new variant to move-assign.
	/// @return The index of the assigned variant, passed to and used by the guest.
	unsigned try_reuse_assign_variant(int32_t src_idx, const Variant &src_var, int32_t assign_to_idx, const Variant &var);

	/// @brief Add a scoped object to the current state.
	/// @param ptr The pointer to the object to add.
	void add_scoped_object(const void *ptr);

	/// @brief Remove a scoped object from the current state.
	/// @param ptr The pointer to the object to remove.
	void rem_scoped_object(const void *ptr) { state().scoped_objects.erase(std::remove(state().scoped_objects.begin(), state().scoped_objects.end(), reinterpret_cast<uintptr_t>(ptr)), state().scoped_objects.end()); }

	/// @brief Check if an object is scoped in the current state.
	/// @param ptr The pointer to the object to check.
	/// @return True if the object is scoped, false otherwise.
	bool is_scoped_object(const void *ptr) const noexcept { return state().scoped_objects.end() != std::find(state().scoped_objects.begin(), state().scoped_objects.end(), reinterpret_cast<uintptr_t>(ptr)); }

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
	void add_allowed_object(Object *obj);

	/// @brief Remove an object from the list of allowed objects.
	/// @param obj The object to remove.
	/// @note If the list becomes empty, all objects are allowed.
	void remove_allowed_object(Object *obj);

	/// @brief Clear the list of allowed objects.
	void clear_allowed_objects();

	/// @brief Check if an object is allowed in the sandbox.
	bool is_allowed_object(Object *obj) const;

	/// @brief Set a callback to check if an object is allowed in the sandbox.
	/// @param callback The callable to check if an object is allowed.
	void set_object_allowed_callback(const Callable &callback);

	/// @brief Check if a class name is allowed in the sandbox.
	bool is_allowed_class(const String &name) const;

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
	bool is_allowed_method(Object *obj, const Variant &method) const;

	/// @brief Set a callback to check if a method is allowed in the sandbox.
	/// @param callback The callable to check if a method is allowed.
	void set_method_allowed_callback(const Callable &callback);

	/// @brief Check if accessing a property on an object is allowed in the sandbox.
	/// @param obj The object to check.
	/// @param property The name of the property to check.
	/// @return True if the property is allowed, false otherwise.
	bool is_allowed_property(Object *obj, const Variant &property, bool is_set) const;

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
	void add_property(const String &name, Variant::Type vtype, uint64_t setter, uint64_t getter, const Variant &def = "") const;

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
	std::vector<PropertyInfo> create_sandbox_property_list() const;

	// -= Program management & public functions =-

	/// @brief Check if a program has been loaded into the sandbox.
	/// @return True if a program has been loaded, false otherwise.
	bool has_program_loaded() const;
	/// @brief Set the program to run in the sandbox.
	/// @param program The program to load and run.
	void set_program(Ref<ELFScript> program);
	/// @brief Get the program loaded into the sandbox.
	/// @return The program loaded into the sandbox.
	Ref<ELFScript> get_program();

	/// @brief Load a program from a buffer into the sandbox.
	/// @param buffer The buffer containing the program.
	void load_buffer(const PackedByteArray &buffer);

	/// @brief Get the public functions available to call in the guest program.
	/// @return Array of public callable functions.
	PackedStringArray get_functions() const;
	struct BinaryInfo {
		String language;
		PackedStringArray functions;
		int version = 0;
	};
	/// @brief Get information about the program from the binary.
	/// @param binary The binary data.
	/// @return An array of public callable functions and programming language.
	static BinaryInfo get_program_info_from_binary(const PackedByteArray &binary);

	/// @brief Get a list of Sandbox methods, including VM exported functions, Sandbox
	/// methods, and Godot methods.
	/// @return The list of Sandbox methods.
	/// TODO: Implement this
	//Array get_method_list() const;

	// -= Self-testing, inspection and internal functions =-

	/// @brief Get the current Callable set for redirecting stdout.
	/// @return The current Callable set for redirecting stdout.
	const Callable &get_redirect_stdout() const { return m_redirect_stdout; }

	/// @brief Set a Callable to redirect stdout from the guest program to.
	/// @param callback The callable to redirect stdout.
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
	String emit_binary_translation(bool ignore_instruction_limit = true, bool automatic_nbit_as = false) const;

	/// @brief  Check if the program has found and loaded binary translation.
	/// @return True if binary translation is loaded, false otherwise.
	bool is_binary_translated() const;

	void assault(const String &test, int64_t iterations);
	Variant vmcall_internal(gaddr_t address, const Variant **args, int argc);
	machine_t &machine() { return *m_machine; }
	const machine_t &machine() const { return *m_machine; }
	void print(const Variant &v);
	static String generate_api(String language = "cpp", String header_extra = "", bool use_argument_names = false);
	void enable_profiling(bool enable, uint32_t interval = 20000);
	String get_hotspots(const String &elf, int total = 10) const;
	void clear_hotspots() const;

private:
	static void generate_runtime_cpp_api(bool use_argument_names = false);
	bool is_in_vmcall() const noexcept { return m_current_state != &m_states[0]; }
	void constructor_initialize();
	void full_reset();
	void reset_machine();
	void set_program_data_internal(Ref<ELFScript> program);
	bool load(const PackedByteArray *vbuf, const std::vector<std::string> *argv = nullptr);
	void read_program_properties(bool editor) const;
	void handle_exception(gaddr_t);
	void handle_timeout(gaddr_t);
	void print_backtrace(gaddr_t);
	void initialize_syscalls();
	void initialize_syscalls_2d();
	void initialize_syscalls_3d();
	GuestVariant *setup_arguments(gaddr_t &sp, const Variant **args, int argc);
	void setup_arguments_native(gaddr_t arrayDataPtr, GuestVariant *v, const Variant **args, int argc);

	machine_t *m_machine = nullptr;
	Node *m_tree_base = nullptr;
	uint32_t m_max_refs = MAX_REFS;
	uint32_t m_memory_max = MAX_VMEM;
	int64_t m_insn_max = MAX_INSTRUCTIONS;

	uint8_t m_throttled = 0;
	bool m_use_unboxed_arguments = false;
	bool m_resumable_mode = false; // If enabled, allow running startup in small increments
	bool m_precise_simulation = false; // Run simulation in the slower, precise mode

	CurrentState *m_current_state = nullptr;
	// State stack, with the permanent (initial) state at index 0.
	// That means eg. static Variant values are held stored in the state at index 0,
	// so that they can be accessed by future VM calls, and not lost when a call ends.
	std::array<CurrentState, MAX_LEVEL> m_states;

	// Properties
	mutable std::vector<SandboxProperty> m_properties;
	mutable std::unordered_map<int64_t, gaddr_t> m_lookup;

	// Restrictions
	std::unordered_set<Object *> m_allowed_objects;
	// If an object is not in the allowed list, and a callable is set for the
	// just-in-time allowed objects, it will be called to check if the object is allowed.
	Callable m_just_in_time_allowed_objects;
	// If a class is not in the allowed list, and a callable is set for the
	// just-in-time allowed classes, it will be called to check if the class is allowed.
	Callable m_just_in_time_allowed_classes;
	// If a callable is set for the just-in-time allowed resources,
	// it will be called to check if access to a resource is allowed.
	Callable m_just_in_time_allowed_resources;
	// If a callable is set for allowed methods, it will be called when an object method
	// call is attemped, to check if the method is allowed.
	Callable m_just_in_time_allowed_methods;
	// If a callable is set for allowed properties, it will be called when an object property
	// access is attemped, to check if the property is allowed.
	Callable m_just_in_time_allowed_properties;

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
		std::unordered_map<gaddr_t, int> visited;
		uint32_t profiling_interval = 2000;
		uint32_t profiler_icounter_accumulator = 0;
	};
	mutable std::unique_ptr<ProfilingData> m_profiling_data = nullptr;

	// Global statistics
	static inline uint64_t m_global_timeouts = 0;
	static inline uint64_t m_global_exceptions = 0;
	static inline uint64_t m_global_calls_made = 0;
	static inline uint32_t m_global_instance_count = 0;
	static inline double m_accumulated_startup_time = 0.0;
};

inline void Sandbox::CurrentState::append(Variant &&value) {
	variants.push_back(std::move(value));
	scoped_variants.push_back(&variants.back());
}

inline void Sandbox::CurrentState::reset() {
	variants.clear();
	scoped_variants.clear();
	scoped_objects.clear();
}

inline bool Sandbox::is_allowed_object(Object *obj) const {
	// If the allowed list is empty, and the allowed-object callback is not set, all objects are allowed
	if (m_allowed_objects.empty() && !m_just_in_time_allowed_objects.is_valid())
		return true;
	// Otherwise, check if the object is in the allowed list
	if (m_allowed_objects.find(obj) != m_allowed_objects.end())
		return true;

	// If the object-allowed callable is set, call it
	if (m_just_in_time_allowed_objects.is_valid())
		return m_just_in_time_allowed_objects.call(this, obj);
	return false;
}
