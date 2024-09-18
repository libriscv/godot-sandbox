#include "sandbox.h"

#include "guest_datatypes.h"
#include "sandbox_project_settings.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

static const int HEAP_SYSCALLS_BASE = 480;
static const int MEMORY_SYSCALLS_BASE = 485;
static const std::vector<std::string> program_arguments = { "program" };

String Sandbox::_to_string() const {
	return "[ GDExtension::Sandbox <--> Instance ID:" + uitos(get_instance_id()) + " ]";
}

void Sandbox::_bind_methods() {
	// Methods.
	ClassDB::bind_method(D_METHOD("get_functions"), &Sandbox::get_functions);
	ClassDB::bind_method(D_METHOD("set_program", "program"), &Sandbox::set_program);
	{
		MethodInfo mi;
		mi.arguments.push_back(PropertyInfo(Variant::STRING, "function"));
		mi.name = "vmcall";
		mi.return_val = PropertyInfo(Variant::OBJECT, "result");
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "vmcall", &Sandbox::vmcall, mi, DEFVAL(std::vector<Variant>{}));
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "vmcallv", &Sandbox::vmcallv, mi, DEFVAL(std::vector<Variant>{}));
	}
	ClassDB::bind_method(D_METHOD("vmcallable", "function", "args"), &Sandbox::vmcallable, DEFVAL(Array{}));

	// Internal testing.
	ClassDB::bind_method(D_METHOD("assault", "test", "iterations"), &Sandbox::assault);
	ClassDB::bind_method(D_METHOD("has_function", "function"), &Sandbox::has_function);

	// Properties.
	ClassDB::bind_method(D_METHOD("set_max_refs", "max"), &Sandbox::set_max_refs);
	ClassDB::bind_method(D_METHOD("get_max_refs"), &Sandbox::get_max_refs);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_references", PROPERTY_HINT_NONE, "Maximum objects and variants referenced by a sandbox call"), "set_max_refs", "get_max_refs");

	ClassDB::bind_method(D_METHOD("set_memory_max", "max"), &Sandbox::set_memory_max);
	ClassDB::bind_method(D_METHOD("get_memory_max"), &Sandbox::get_memory_max);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "memory_max", PROPERTY_HINT_NONE, "Maximum memory (in MiB) used by the sandboxed program"), "set_memory_max", "get_memory_max");

	ClassDB::bind_method(D_METHOD("set_instructions_max", "max"), &Sandbox::set_instructions_max);
	ClassDB::bind_method(D_METHOD("get_instructions_max"), &Sandbox::get_instructions_max);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "execution_timeout", PROPERTY_HINT_NONE, "Maximum billions of instructions executed before cancelling execution"), "set_instructions_max", "get_instructions_max");

	ClassDB::bind_method(D_METHOD("set_use_unboxed_arguments", "use_unboxed_arguments"), &Sandbox::set_use_unboxed_arguments);
	ClassDB::bind_method(D_METHOD("get_use_unboxed_arguments"), &Sandbox::get_use_unboxed_arguments);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_unboxed_arguments", PROPERTY_HINT_NONE, "Use unboxed arguments for VM function calls"), "set_use_unboxed_arguments", "get_use_unboxed_arguments");

	// Group for monitored Sandbox health.
	ADD_GROUP("Sandbox Monitoring", "monitor_");

	ClassDB::bind_method(D_METHOD("get_heap_usage"), &Sandbox::get_heap_usage);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_heap_usage", PROPERTY_HINT_NONE, "Current arena usage"), "", "get_heap_usage");

	ClassDB::bind_method(D_METHOD("get_budget_overruns"), &Sandbox::get_budget_overruns);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_execution_timeouts", PROPERTY_HINT_NONE, "Number of execution timeouts"), "", "get_budget_overruns");

	ClassDB::bind_method(D_METHOD("get_calls_made"), &Sandbox::get_calls_made);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_calls_made", PROPERTY_HINT_NONE, "Number of calls made"), "", "get_calls_made");

	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_calls_made"), &Sandbox::get_global_calls_made);
	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_exceptions"), &Sandbox::get_global_exceptions);
	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_budget_overruns"), &Sandbox::get_global_budget_overruns);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_calls_made", PROPERTY_HINT_NONE, "Number of calls made"), "", "get_global_calls_made");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_exceptions", PROPERTY_HINT_NONE, "Number of exceptions thrown"), "", "get_global_exceptions");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_execution_timeouts", PROPERTY_HINT_NONE, "Number of execution timeouts"), "", "get_global_budget_overruns");

	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_instance_count"), &Sandbox::get_global_instance_count);
	ClassDB::bind_static_method("Sandbox", D_METHOD("get_accumulated_startup_time"), &Sandbox::get_accumulated_startup_time);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_instance_count", PROPERTY_HINT_NONE, "Number of active sandbox instances"), "", "get_global_instance_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "monitor_accumulated_startup_time", PROPERTY_HINT_NONE, "Accumulated startup time of all sandbox instantiations"), "", "get_accumulated_startup_time");

	// Group for sandboxed properties.
	ADD_GROUP("Sandboxed Properties", "custom_");
}

Sandbox::Sandbox() {
	this->m_use_unboxed_arguments = SandboxProjectSettings::use_native_types();
	this->m_global_instance_count += 1;
	// In order to reduce checks we guarantee that this
	// class is well-formed at all times.
	try {
		this->m_machine = new machine_t{};
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
	}
}

Sandbox::~Sandbox() {
	this->m_global_instance_count -= 1;
	try {
		delete this->m_machine;
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
	}
}

void Sandbox::set_program(Ref<ELFScript> program) {
	m_program_data = program;
	if (m_program_data.is_null()) {
		// TODO unload program
		return;
	}
	this->load(&m_program_data->get_content());
}
Ref<ELFScript> Sandbox::get_program() {
	return m_program_data;
}
bool Sandbox::has_program_loaded() const {
	return this->m_binary != nullptr;
}
void Sandbox::load(const PackedByteArray *buffer, const std::vector<std::string> *argv_ptr) {
	if (buffer == nullptr || buffer->is_empty()) {
		ERR_PRINT("Empty binary, cannot load program.");
		return;
	}
	// If the binary sizes match, let's see if the binary is the exact same
	if (buffer->size() == this->m_machine->memory.binary().size()) {
		if (std::memcmp(buffer->ptr(), this->m_machine->memory.binary().data(), buffer->size()) == 0) {
			// Binary is the same, no need to reload
			return;
		}
	}
	this->m_binary = buffer;
	const std::string_view binary_view = std::string_view{ (const char *)buffer->ptr(), static_cast<size_t>(buffer->size()) };

	// Get t0 for the startup time
	const uint64_t startup_t0 = Time::get_singleton()->get_ticks_usec();

	/** We can't handle exceptions until the Machine is fully constructed. Two steps.  */
	try {
		delete this->m_machine;

		const riscv::MachineOptions<RISCV_ARCH> options{
			.memory_max = uint64_t(get_memory_max()) << 20, // in MiB
			//.verbose_loader = true,
			.default_exit_function = "fast_exit",
		};

		this->m_machine = new machine_t{ binary_view, options };
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox construction exception: " + std::string(e.what())).c_str());
		this->m_machine = new machine_t{};
		this->m_binary = nullptr;
		return;
	}

	/** Now we can process symbols, backtraces etc. */
	try {
		machine_t &m = machine();

		m.set_userdata(this);
		this->m_current_state = &this->m_states[0]; // Set the current state to the first state

		this->initialize_syscalls();

		const gaddr_t heap_size = MAX_HEAP << 20; // in MiB
		const gaddr_t heap_area = machine().memory.mmap_allocate(heap_size);

		// Add native system call interfaces
		machine().setup_native_heap(HEAP_SYSCALLS_BASE, heap_area, heap_size);
		machine().setup_native_memory(MEMORY_SYSCALLS_BASE);

		// Set up a Linux environment for the program
		const std::vector<std::string> *argv = argv_ptr ? argv_ptr : &program_arguments;
		m.setup_linux(*argv);

		// Run the program through to its main() function
		m.simulate(get_instructions_max() << 30);
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
		this->handle_exception(machine().cpu.pc());
	}

	// Read the program's custom properties, if any
	this->read_program_properties(true);

	// Pre-cache some functions
	PackedStringArray functions = this->get_functions();
	for (int i = 0; i < functions.size(); i++) {
		this->cached_address_of(functions[i].hash(), functions[i]);
	}

	// Accumulate startup time
	const uint64_t startup_t1 = Time::get_singleton()->get_ticks_usec();
	m_accumulated_startup_time += (startup_t1 - startup_t0) / 1e6;
}

Variant Sandbox::vmcall_address(gaddr_t address, const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	error.error = GDEXTENSION_CALL_OK;
	return this->vmcall_internal(address, args, arg_count);
}
Variant Sandbox::vmcall(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	if (arg_count < 1) {
		error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		error.argument = -1;
		return Variant();
	}
	error.error = GDEXTENSION_CALL_OK;

	const Variant &function = *args[0];
	args += 1;
	arg_count -= 1;
	const String function_name = function.operator String();
	return this->vmcall_internal(cached_address_of(function_name.hash(), function_name), args, arg_count);
}
Variant Sandbox::vmcallv(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	if (arg_count < 1) {
		error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		error.argument = -1;
		return Variant();
	}
	error.error = GDEXTENSION_CALL_OK;

	const Variant &function = *args[0];
	args += 1;
	arg_count -= 1;
	const String function_name = function.operator String();
	// Store use_unboxed_arguments state and restore it after the call
	Variant result;
	auto old_use_unboxed_arguments = this->m_use_unboxed_arguments;
	this->m_use_unboxed_arguments = false;
	result = this->vmcall_internal(cached_address_of(function_name.hash(), function_name), args, arg_count);
	this->m_use_unboxed_arguments = old_use_unboxed_arguments;
	return result;
}
Variant Sandbox::vmcall_fn(const StringName &function, const Variant **args, GDExtensionInt arg_count) {
	if (this->m_throttled > 0) {
		this->m_throttled--;
		return Variant();
	}
	Variant result = this->vmcall_internal(cached_address_of(function.hash()), args, arg_count);
	return result;
}
void Sandbox::setup_arguments_native(gaddr_t arrayDataPtr, GuestVariant *v, const Variant **args, int argc) {
	// In this mode we will try to use registers when possible
	// The stack is already set up from setup_arguments(), so we just need to set up the registers
	machine_t &machine = this->machine();
	int index = 11;
	int flindex = 10;

	for (size_t i = 0; i < argc; i++) {
		const Variant &arg = *args[i];
		const GDNativeVariant *inner = (const GDNativeVariant *)arg._native_ptr();

		// Incoming arguments are implicitly trusted, as they are provided by the host
		// They also have have the guaranteed lifetime of the function call
		switch (arg.get_type()) {
			case Variant::Type::BOOL:
				machine.cpu.reg(index++) = inner->value;
				break;
			case Variant::Type::INT:
				//printf("Type: %u Value: %ld\n", inner->type, inner->value);
				machine.cpu.reg(index++) = inner->value;
				break;
			case Variant::Type::FLOAT: // Variant floats are always 64-bit
				//printf("Type: %u Value: %f\n", inner->type, inner->flt);
				machine.cpu.registers().getfl(flindex++).set_double(inner->flt);
				break;
			case Variant::VECTOR2: { // 8- or 16-byte structs can be passed in registers
				machine.cpu.registers().getfl(flindex++).set_float(inner->vec2_flt[0]);
				machine.cpu.registers().getfl(flindex++).set_float(inner->vec2_flt[1]);
				break;
			}
			case Variant::VECTOR2I: { // 8- or 16-byte structs can be passed in registers
				machine.cpu.reg(index++) = inner->value; // 64-bit packed integers
				break;
			}
			case Variant::COLOR: { // 16-byte struct (must use integer registers)
				// RVG calling convention:
				// Unions and arrays containing floats are passed in integer registers
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->color_flt[0];
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->color_flt[2];
				break;
			}
			case Variant::OBJECT: { // Objects are represented as uintptr_t
				godot::Object *obj = inner->to_object();
				this->add_scoped_object(obj);
				machine.cpu.reg(index++) = uintptr_t(obj); // Fits in a single register
				break;
			}
			case Variant::ARRAY:
			case Variant::DICTIONARY:
			case Variant::STRING:
			case Variant::STRING_NAME:
			case Variant::NODE_PATH:
			case Variant::PACKED_BYTE_ARRAY:
			case Variant::PACKED_FLOAT32_ARRAY:
			case Variant::PACKED_FLOAT64_ARRAY:
			case Variant::PACKED_INT32_ARRAY:
			case Variant::PACKED_INT64_ARRAY: { // Uses Variant index to reference the object
				unsigned idx = this->add_scoped_variant(&arg);
				machine.cpu.reg(index++) = idx;
				break;
			}
			default: { // Complex types are passed byref, pushed onto the stack as GuestVariant
				GuestVariant &g_arg = v[i + 1];
				g_arg.set(*this, arg, true);
				machine.cpu.reg(index++) = arrayDataPtr + (i + 1) * sizeof(GuestVariant);
			}
		}
	}
}
GuestVariant *Sandbox::setup_arguments(gaddr_t &sp, const Variant **args, int argc) {
	sp -= sizeof(GuestVariant) * (argc + 1);
	sp &= ~gaddr_t(0xF); // re-align stack pointer
	const gaddr_t arrayDataPtr = sp;
	const int arrayElements = argc + 1;

	GuestVariant *v = m_machine->memory.memarray<GuestVariant>(arrayDataPtr, arrayElements);
	if (argc > 7)
		throw std::runtime_error("Sandbox: Too many arguments for VM function call");

	// Set up first argument (return value, also a Variant)
	m_machine->cpu.reg(10) = arrayDataPtr;
	//v[0].type = Variant::Type::NIL;

	if (this->m_use_unboxed_arguments) {
		setup_arguments_native(arrayDataPtr, v, args, argc);
		// A0 is the return value (Variant) of the function
		return &v[0];
	}

	for (size_t i = 0; i < argc; i++) {
		const Variant &arg = *args[i];
		GuestVariant &g_arg = v[i + 1];
		// Fast-path for simple types
		GDNativeVariant *inner = (GDNativeVariant *)arg._native_ptr();
		// Incoming arguments are implicitly trusted, as they are provided by the host
		// They also have have the guaranteed lifetime of the function call
		switch (arg.get_type()) {
			case Variant::Type::NIL:
				g_arg.type = Variant::Type::NIL;
				break;
			case Variant::Type::BOOL:
				g_arg.type = Variant::Type::BOOL;
				g_arg.v.b = inner->value;
				break;
			case Variant::Type::INT:
				g_arg.type = Variant::Type::INT;
				g_arg.v.i = inner->value;
				break;
			case Variant::Type::FLOAT:
				g_arg.type = Variant::Type::FLOAT;
				g_arg.v.f = inner->flt;
				break;
			default:
				g_arg.set(*this, *args[i], true);
		}
		m_machine->cpu.reg(11 + i) = arrayDataPtr + (i + 1) * sizeof(GuestVariant);
	}
	// A0 is the return value (Variant) of the function
	return &v[0];
}
Variant Sandbox::vmcall_internal(gaddr_t address, const Variant **args, int argc) {
	CurrentState &state = this->m_states[m_level];
	const bool is_reentrant_call = m_level > 1;
	state.reset(this->m_level, this->m_max_refs);
	m_level++;

	// Scoped objects and owning tree node
	CurrentState *old_state = this->m_current_state;
	this->m_current_state = &state;
	// Call statistics
	this->m_calls_made++;
	Sandbox::m_global_calls_made++;

	try {
		GuestVariant *retvar = nullptr;
		riscv::CPU<RISCV_ARCH> &cpu = m_machine->cpu;
		auto &sp = cpu.reg(riscv::REG_SP);
		// execute guest function
		if (!is_reentrant_call) {
			cpu.reg(riscv::REG_RA) = m_machine->memory.exit_address();
			// reset the stack pointer to its initial location
			sp = m_machine->memory.stack_initial();
			// set up each argument, and return value
			retvar = this->setup_arguments(sp, args, argc);
			// execute!
			m_machine->simulate_with(get_instructions_max() << 30, 0u, address);
		} else if (m_level < MAX_LEVEL) {
			riscv::Registers<RISCV_ARCH> regs;
			regs = cpu.registers();
			// we are in a recursive call, so wait before setting exit address
			cpu.reg(riscv::REG_RA) = m_machine->memory.exit_address();
			// we need to make some stack room
			sp -= 16u;
			// set up each argument, and return value
			retvar = this->setup_arguments(sp, args, argc);
			// execute!
			cpu.preempt_internal(regs, true, address, get_instructions_max() << 30);
		} else {
			throw std::runtime_error("Recursion level exceeded");
		}

		// Treat return value as pointer to Variant
		Variant result = retvar->toVariant(*this);
		// Restore the previous state
		this->m_level--;
		this->m_current_state = old_state;
		return result;

	} catch (const std::exception &e) {
		this->m_level--;
		if (Engine::get_singleton()->is_editor_hint()) {
			// Throttle exceptions in the sandbox when calling from the editor
			this->m_throttled += EDITOR_THROTTLE;
		}
		this->handle_exception(address);
		// TODO: Free the function arguments and return value? Will help keep guest memory clean

		this->m_current_state = old_state;
		return Variant();
	}
}
Variant Sandbox::vmcallable(String function, Array args) {
	const gaddr_t address = cached_address_of(function.hash(), function);
	if (address == 0x0) {
		ERR_PRINT("Function not found in the guest: " + function);
		return Variant();
	}

	RiscvCallable *call = memnew(RiscvCallable);
	call->init(this, address, std::move(args));
	return Callable(call);
}
Variant Sandbox::vmcallable_address(gaddr_t address, Array args) {
	RiscvCallable *call = memnew(RiscvCallable);
	call->init(this, address, std::move(args));
	return Callable(call);
}
void RiscvCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	if (m_varargs_base_count > 0) {
		// We may be receiving extra arguments, so we will fill at the end of m_varargs_ptrs array
		const int total_args = m_varargs_base_count + p_argcount;
		if (size_t(total_args) > m_varargs_ptrs.size()) {
			ERR_PRINT("Too many arguments for VM function call");
			r_call_error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
			r_call_error.argument = p_argcount;
			return;
		}

		for (int i = 0; i < p_argcount; i++) {
			m_varargs_ptrs[m_varargs_base_count + i] = p_arguments[i];
		}
		r_return_value = self->vmcall_internal(address, m_varargs_ptrs.data(), total_args);
	} else {
		r_return_value = self->vmcall_internal(address, p_arguments, p_argcount);
	}
	r_call_error.error = GDEXTENSION_CALL_OK;
}

void Sandbox::print(std::string_view text) {
	String str(static_cast<std::string>(text).c_str());
	if (this->m_last_newline) {
		UtilityFunctions::print("[", get_name(), "] says: ", str);
	} else {
		UtilityFunctions::print(str);
	}
	this->m_last_newline = (text.back() == '\n');
}

gaddr_t Sandbox::cached_address_of(int64_t hash, const String &function) const {
	gaddr_t address = 0x0;
	auto it = m_lookup.find(hash);
	if (it == m_lookup.end()) {
		const CharString ascii = function.ascii();
		const std::string_view str{ ascii.get_data(), (size_t)ascii.length() };
		address = address_of(str);
		if (address != 0x0) {
			// Cheating a bit here, as we are in a const function
			// But this does not functionally change the Machine, it only boosts performance a bit
			const_cast<machine_t *>(m_machine)->cpu.create_fast_path_function(address);
		}
		m_lookup.insert_or_assign(hash, address);
	} else {
		address = it->second;
	}

	return address;
}

gaddr_t Sandbox::cached_address_of(int64_t hash) const {
	auto it = m_lookup.find(hash);
	if (it != m_lookup.end()) {
		return it->second;
	}
	return 0;
}

gaddr_t Sandbox::address_of(std::string_view name) const {
	return machine().address_of(name);
}

bool Sandbox::has_function(const StringName &p_function) const {
	const gaddr_t address = cached_address_of(p_function.hash(), p_function);
	return address != 0x0;
}

int64_t Sandbox::get_heap_usage() const {
	if (machine().has_arena()) {
		return machine().arena().bytes_used();
	}
	return 0;
}

//-- Scoped objects and variants --//

unsigned Sandbox::add_scoped_variant(const Variant *value) const {
	if (state().scoped_variants.size() >= this->m_max_refs) {
		ERR_PRINT("Maximum number of scoped variants reached.");
		throw std::runtime_error("Maximum number of scoped variants reached.");
	}
	state().scoped_variants.push_back(value);
	return state().scoped_variants.size() - 1 + 1; // 1-based index
}
unsigned Sandbox::create_scoped_variant(Variant &&value) const {
	if (state().scoped_variants.size() >= this->m_max_refs) {
		ERR_PRINT("Maximum number of scoped variants reached.");
		throw std::runtime_error("Maximum number of scoped variants reached.");
	}
	state().variants.emplace_back(std::move(value));
	state().scoped_variants.push_back(&state().variants.back());
	return state().scoped_variants.size() - 1 + 1; // 1-based index
}
std::optional<const Variant *> Sandbox::get_scoped_variant(unsigned index) const noexcept {
	if (index == 0 || index > state().scoped_variants.size()) {
		ERR_PRINT("Invalid scoped variant index.");
		return std::nullopt;
	}
	return state().scoped_variants[index - 1];
}
Variant &Sandbox::get_mutable_scoped_variant(unsigned index) {
	std::optional<const Variant *> var_opt = get_scoped_variant(index);
	if (!var_opt.has_value()) {
		ERR_PRINT("Invalid scoped variant index.");
		throw std::runtime_error("Invalid scoped variant index.");
	}
	const Variant *var = var_opt.value();
	// Find the variant in the variants list
	auto it = std::find_if(state().variants.begin(), state().variants.end(), [var](const Variant &v) {
		return &v == var;
	});
	if (it == state().variants.end()) {
		// Create a new variant in the list using the existing one, and return it
		if (state().variants.size() >= this->m_max_refs) {
			ERR_PRINT("Maximum number of scoped variants reached.");
			throw std::runtime_error("Maximum number of scoped variants reached.");
		}
		state().variants.emplace_back(*var);
		return state().variants.back();
	}
	return *it;
}

void Sandbox::add_scoped_object(const void *ptr) {
	if (state().scoped_objects.size() >= this->m_max_refs) {
		ERR_PRINT("Maximum number of scoped objects reached.");
		throw std::runtime_error("Maximum number of scoped objects reached.");
	}
	state().scoped_objects.push_back(reinterpret_cast<uintptr_t>(ptr));
}

//-- Properties --//

void Sandbox::read_program_properties(bool editor) const {
	try {
		// Properties is an array named properties, that ends with an invalid property
		auto prop_addr = machine().address_of("properties");
		if (prop_addr == 0x0)
			return;

		struct GuestProperty {
			gaddr_t g_name;
			unsigned size;
			Variant::Type type;
			gaddr_t getter;
			gaddr_t setter;
			GuestVariant def_val;
		};
		auto *props = machine().memory.memarray<GuestProperty>(prop_addr, MAX_PROPERTIES);

		for (int i = 0; i < MAX_PROPERTIES; i++) {
			const GuestProperty *prop = &props[i];
			// Invalid property: stop reading
			if (prop->g_name == 0)
				break;
			// Check if the property is valid by checking its size
			if (prop->size != sizeof(GuestProperty)) {
				ERR_PRINT("Sandbox: Invalid property size");
				break;
			}
			const std::string c_name = machine().memory.memstring(prop->g_name);
			Variant def_val = prop->def_val.toVariant(*this);

			this->add_property(String::utf8(c_name.c_str(), c_name.size()), prop->type, prop->setter, prop->getter, def_val);
		}
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
	}
}

void Sandbox::add_property(const String &name, Variant::Type vtype, uint64_t setter, uint64_t getter, const Variant &def) const {
	if (setter == 0 || getter == 0) {
		ERR_PRINT("Sandbox: Setter and getter not found for property: " + name);
		return;
	} else if (m_properties.size() >= MAX_PROPERTIES) {
		ERR_PRINT("Sandbox: Maximum number of properties reached");
		return;
	}
	for (const SandboxProperty &prop : m_properties) {
		if (prop.name() == name) {
			// TODO: Allow overriding properties?
			//ERR_PRINT("Sandbox: Property already exists: " + name);
			return;
		}
	}
	m_properties.push_back(SandboxProperty(name, vtype, setter, getter, def));
}

bool Sandbox::set_property(const StringName &name, const Variant &value) {
	if (m_properties.empty()) {
		this->read_program_properties(false);
	}
	for (SandboxProperty &prop : m_properties) {
		if (prop.name() == name) {
			prop.set(*this, value);
			//ERR_PRINT("Sandbox: SetProperty *found*: " + name);
			return true;
		}
	}
	// Not the most efficient way to do this, but it's (currently) a small list
	if (name == StringName("max_references")) {
		set_max_refs(value);
		return true;
	} else if (name == StringName("memory_max")) {
		set_memory_max(value);
		return true;
	} else if (name == StringName("execution_timeout")) {
		set_instructions_max(value);
		return true;
	} else if (name == StringName("use_unboxed_arguments")) {
		set_use_unboxed_arguments(value);
		return true;
	}
	return false;
}

bool Sandbox::get_property(const StringName &name, Variant &r_ret) {
	if (m_properties.empty()) {
		this->read_program_properties(false);
	}
	for (const SandboxProperty &prop : m_properties) {
		if (prop.name() == name) {
			r_ret = prop.get(*this);
			//ERR_PRINT("Sandbox: GetProperty *found*: " + name);
			return true;
		}
	}
	// Not the most efficient way to do this, but it's (currently) a small list
	if (name == StringName("max_references")) {
		r_ret = get_max_refs();
		return true;
	} else if (name == StringName("memory_max")) {
		r_ret = get_memory_max();
		return true;
	} else if (name == StringName("execution_timeout")) {
		r_ret = get_instructions_max();
		return true;
	} else if (name == StringName("use_unboxed_arguments")) {
		r_ret = get_use_unboxed_arguments();
		return true;
	} else if (name == StringName("monitor_heap_usage")) {
		r_ret = get_heap_usage();
		return true;
	} else if (name == StringName("monitor_execution_timeouts")) {
		r_ret = get_budget_overruns();
		return true;
	} else if (name == StringName("monitor_calls_made")) {
		r_ret = get_calls_made();
		return true;
	} else if (name == StringName("global_calls_made")) {
		r_ret = get_global_calls_made();
		return true;
	} else if (name == StringName("global_exceptions")) {
		r_ret = get_global_exceptions();
		return true;
	} else if (name == StringName("global_budget_overruns")) {
		r_ret = get_global_budget_overruns();
		return true;
	} else if (name == StringName("monitor_accumulated_startup_time")) {
		r_ret = get_accumulated_startup_time();
		return true;
	} else if (name == StringName("monitor_global_instance_count")) {
		r_ret = get_global_instance_count();
		return true;
	}
	return false;
}

const SandboxProperty *Sandbox::find_property_or_null(const StringName &name) const {
	for (const SandboxProperty &prop : m_properties) {
		if (prop.name() == name) {
			return &prop;
		}
	}
	return nullptr;
}

void SandboxProperty::set(Sandbox &sandbox, const Variant &value) {
	if (m_setter_address == 0) {
		ERR_PRINT("Sandbox: Setter was invalid for property: " + m_name);
		return;
	}
	const Variant *args[] = { &value };
	// Store use_unboxed_arguments state and restore it after the call
	// It's much more convenient to use Variant arguments for properties
	auto old_use_unboxed_arguments = sandbox.get_use_unboxed_arguments();
	sandbox.set_use_unboxed_arguments(false);
	sandbox.vmcall_internal(m_setter_address, args, 1);
	sandbox.set_use_unboxed_arguments(old_use_unboxed_arguments);
}

Variant SandboxProperty::get(const Sandbox &sandbox) const {
	if (m_getter_address == 0) {
		ERR_PRINT("Sandbox: Getter was invalid for property: " + m_name);
		return Variant();
	}
	return const_cast<Sandbox &>(sandbox).vmcall_internal(m_getter_address, nullptr, 0);
}
