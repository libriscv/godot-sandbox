#include "sandbox.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <charconv>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

static const int HEAP_SYSCALLS_BASE = 480;
static const int MEMORY_SYSCALLS_BASE = 485;
static constexpr bool VERBOSE_EXCEPTIONS = false;

static inline String to_hex(gaddr_t value) {
	char str[20] = { 0 };
	char *end = std::to_chars(std::begin(str), std::end(str), value, 16).ptr;
	return String::utf8(str, int64_t(end - str));
}

String Sandbox::_to_string() const {
	return "[ GDExtension::Sandbox <--> Instance ID:" + uitos(get_instance_id()) + " ]";
}

void Sandbox::_bind_methods() {
	// Methods.
	ClassDB::bind_method(D_METHOD("get_functions"), &Sandbox::get_functions);
	{
		MethodInfo mi;
		mi.arguments.push_back(PropertyInfo(Variant::STRING, "function"));
		mi.name = "vmcall";
		mi.return_val = PropertyInfo(Variant::OBJECT, "result");
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "vmcall", &Sandbox::vmcall, mi, DEFVAL(std::vector<Variant>{}));
	}
	ClassDB::bind_method(D_METHOD("vmcallable"), &Sandbox::vmcallable);
	// Internal testing.
	ClassDB::bind_method(D_METHOD("assault", "test", "iterations"), &Sandbox::assault);

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

	// Group for sandboxed properties.
	ADD_GROUP("Sandboxed Properties", "custom_");
}

Sandbox::Sandbox() {
	// In order to reduce checks we guarantee that this
	// class is well-formed at all times.
	try {
		this->m_machine = new machine_t{};
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
	}
}

Sandbox::~Sandbox() {
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
	PackedByteArray data = m_program_data->get_content();
	this->load(std::move(data));
}
Ref<ELFScript> Sandbox::get_program() {
	return m_program_data;
}
bool Sandbox::has_program_loaded() const {
	return !this->machine().memory.binary().empty();
}
void Sandbox::load(PackedByteArray &&buffer, const std::vector<std::string> *argv_ptr) {
	if (buffer.is_empty()) {
		ERR_PRINT("Empty binary, cannot load program.");
		return;
	}
	this->m_binary = std::move(buffer);
	const auto binary_view = std::string_view{ (const char *)this->m_binary.ptr(), static_cast<size_t>(this->m_binary.size()) };

	try {
		delete this->m_machine;

		const riscv::MachineOptions<RISCV_ARCH> options{
			.memory_max = uint64_t(get_memory_max()) << 20, // in MiB
			//.verbose_loader = true,
			.default_exit_function = "fast_exit",
		};

		this->m_machine = new machine_t{ binary_view, options };
		machine_t &m = machine();

		m.set_userdata(this);

		this->initialize_syscalls();

		const uint64_t heap_size = MAX_HEAP << 20; // in MiB
		const auto heap_area = machine().memory.mmap_allocate(heap_size);

		// Add native system call interfaces
		machine().setup_native_heap(HEAP_SYSCALLS_BASE, heap_area, heap_size);
		machine().setup_native_memory(MEMORY_SYSCALLS_BASE);

		static const std::vector<std::string> program_arguments = { "program" };
		const auto *argv = argv_ptr ? argv_ptr : &program_arguments;
		m.setup_linux(*argv);

		m.simulate(get_instructions_max() << 30);
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
		this->handle_exception(machine().cpu.pc());
		// TODO: Program failed to load.
	}

	this->read_program_properties(true);

	// Pre-cache some functions
	auto functions = this->get_functions();
	for (int i = 0; i < functions.size(); i++) {
		this->cached_address_of(functions[i].hash(), functions[i]);
	}
}

Variant Sandbox::vmcall_address(gaddr_t address, const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	return this->vmcall_internal(address, args, arg_count, error);
}
Variant Sandbox::vmcall(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	if (arg_count < 1) {
		error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		error.argument = -1;
		return Variant();
	}
	auto &function = *args[0];
	args += 1;
	arg_count -= 1;
	const String function_name = function.operator String();
	return this->vmcall_internal(cached_address_of(function_name.hash(), function_name), args, arg_count, error);
}
Variant Sandbox::vmcall_fn(const StringName &function, const Variant **args, GDExtensionInt arg_count) {
	if (this->m_throttled > 0) {
		this->m_throttled--;
		return Variant();
	}
	GDExtensionCallError error;
	auto result = this->vmcall_internal(cached_address_of(function.hash()), args, arg_count, error);
	return result;
}
GuestVariant *Sandbox::setup_arguments(gaddr_t &sp, const Variant **args, int argc) {
	sp -= sizeof(GuestVariant) * (argc + 1);
	sp &= ~gaddr_t(0xF); // re-align stack pointer
	const auto arrayDataPtr = sp;
	const auto arrayElements = argc + 1;

	GuestVariant *v = m_machine->memory.memarray<GuestVariant>(arrayDataPtr, arrayElements);
	if (argc > 7)
		argc = 7; // Limit to argument registers (a0-a7)

	// Set up first argument (return value, also a Variant)
	m_machine->cpu.reg(10) = arrayDataPtr;
	v[0].type = Variant::Type::NIL;

	for (size_t i = 0; i < argc; i++) {
		//printf("args[%zu] = type %d\n", i, int(args[i]->get_type()));
		v[i + 1].set(*this, *args[i]);
		m_machine->cpu.reg(11 + i) = arrayDataPtr + (i + 1) * sizeof(GuestVariant);
	}
	// A0 is the return value (Variant) of the function
	return &v[0];
}
Variant Sandbox::vmcall_internal(gaddr_t address, const Variant **args, int argc, GDExtensionCallError &error) {
	auto &state = this->m_states[m_level];
	state.tree_base = this->get_tree_base();
	state.scoped_objects.clear();
	state.scoped_variants.clear();
	auto *old_state = this->m_current_state;
	this->m_current_state = &state;
	this->m_calls_made++;
	Sandbox::m_global_calls_made++;

	try {
		GuestVariant *retvar = nullptr;
		auto &cpu = m_machine->cpu;
		auto &sp = cpu.reg(riscv::REG_SP);
		m_level++;
		// execute guest function
		if (m_level == 1) {
			cpu.reg(riscv::REG_RA) = m_machine->memory.exit_address();
			// reset the stack pointer to its initial location
			sp = m_machine->memory.stack_initial();
			// set up each argument, and return value
			retvar = this->setup_arguments(sp, args, argc);
			// execute!
			m_machine->simulate_with(get_instructions_max() << 30, 0u, address);
		} else if (m_level > 1 && m_level < MAX_LEVEL) {
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
		auto result = retvar->toVariant(*this);
		// Potentially free the return value
		retvar->free(*this);

		this->m_level--;
		this->m_current_state = old_state;
		error.error = GDEXTENSION_CALL_OK;
		return result;

	} catch (const std::exception &e) {
		this->m_level--;
		if (Engine::get_singleton()->is_editor_hint()) {
			// Throttle exceptions in the sandbox when calling from the editor
			this->m_throttled += EDITOR_THROTTLE;
		}
		this->handle_exception(address);

		this->m_current_state = old_state;
		error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
		error.argument = -1;
		return Variant();
	}
}
Variant Sandbox::vmcallable(const String &function) {
	const auto address = cached_address_of(function.hash(), function);
	if (address == 0x0) {
		ERR_PRINT("Function not found in the guest: " + function);
		return Variant();
	}

	auto *call = memnew(RiscvCallable);
	call->init(this, address);
	return Callable(call);
}
void RiscvCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	r_return_value = self->vmcall_internal(address, p_arguments, p_argcount, r_call_error);
}

void Sandbox::handle_exception(gaddr_t address) {
	auto callsite = machine().memory.lookup(address);
	UtilityFunctions::print(
			"[", get_name(), "] Exception when calling:\n  ", callsite.name.c_str(), " (0x",
			to_hex(callsite.address), ")\n", "Backtrace:");
	this->print_backtrace(address);

	try {
		throw; // re-throw
	} catch (const riscv::MachineTimeoutException &e) {
		this->handle_timeout(address);
		return; // NOTE: might wanna stay
	} catch (const riscv::MachineException &e) {
		const String instr(machine().cpu.current_instruction_to_string().c_str());
		const String regs(machine().cpu.registers().to_string().c_str());

		UtilityFunctions::print(
				"\nException: ", e.what(), "  (data: ", to_hex(e.data()), ")\n",
				">>> ", instr, "\n",
				">>> Machine registers:\n[PC\t", to_hex(machine().cpu.pc()),
				"] ", regs, "\n");
	} catch (const std::exception &e) {
		UtilityFunctions::print("\nMessage: ", e.what(), "\n\n");
		ERR_PRINT(("Exception: " + std::string(e.what())).c_str());
	}
	if constexpr (VERBOSE_EXCEPTIONS) {
		UtilityFunctions::print(
				"Program page: ", machine().memory.get_page_info(machine().cpu.pc()).c_str());
		UtilityFunctions::print(
				"Stack page: ", machine().memory.get_page_info(machine().cpu.reg(2)).c_str());
	}

	this->m_exceptions++;
	Sandbox::m_global_exceptions++;

	if (m_machine->memory.binary().empty()) {
		ERR_PRINT("No binary loaded. Remember to assign a program to the Sandbox!");
	}
}

void Sandbox::handle_timeout(gaddr_t address) {
	this->m_budget_overruns++;
	Sandbox::m_global_budget_overruns++;
	auto callsite = machine().memory.lookup(address);
	UtilityFunctions::print(
			"Sandbox: Timeout for '", callsite.name.c_str(),
			"' (Timeouts: ", m_budget_overruns, "\n");
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

void Sandbox::print_backtrace(const gaddr_t addr) {
	machine().memory.print_backtrace(
			[](std::string_view line) {
				String line_str(std::string(line).c_str());
				UtilityFunctions::print("-> ", line_str);
			});
	auto origin = machine().memory.lookup(addr);
	String name(origin.name.c_str());
	UtilityFunctions::print(
			"-> [-] 0x", to_hex(origin.address), " + 0x", to_hex(origin.offset),
			": ", name);
}

gaddr_t Sandbox::cached_address_of(int64_t hash, const String &function) const {
	gaddr_t address = 0x0;
	auto it = m_lookup.find(hash);
	if (it == m_lookup.end()) {
		const auto ascii = function.ascii();
		const std::string_view str{ ascii.get_data(), (size_t)ascii.length() };
		address = address_of(str);
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

int64_t Sandbox::get_heap_usage() const {
	if (machine().has_arena()) {
		return machine().arena().bytes_used();
	}
	return 0;
}

//-- Scoped objects and variants --//

void Sandbox::add_scoped_variant(uint32_t hash) {
	if (state().scoped_variants.size() >= this->m_max_refs) {
		ERR_PRINT("Maximum number of scoped variants reached.");
		throw std::runtime_error("Maximum number of scoped variants reached.");
	}
	state().scoped_variants.push_back(hash);
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
			const auto c_name = machine().memory.memstring(prop->g_name);
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
	for (const auto &prop : m_properties) {
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
	for (auto &prop : m_properties) {
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
	}
	return false;
}

bool Sandbox::get_property(const StringName &name, Variant &r_ret) {
	if (m_properties.empty()) {
		this->read_program_properties(false);
	}
	for (const auto &prop : m_properties) {
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
	}
	return false;
}

SandboxProperty *Sandbox::find_property_or_null(const StringName &name) const {
	for (auto &prop : m_properties) {
		if (prop.name() == name) {
			return &prop;
		}
	}
	return nullptr;
}

void SandboxProperty::set(Sandbox &sandbox, const Variant &value) {
	if (m_setter_address == 0) {
		ERR_PRINT("Sandbox: Setter not found for property: " + m_name);
		return;
	}
	const Variant *args[] = { &value };
	GDExtensionCallError error;
	sandbox.vmcall_internal(m_setter_address, args, 1, error);
}

Variant SandboxProperty::get(const Sandbox &sandbox) const {
	if (m_getter_address == 0) {
		ERR_PRINT("Sandbox: Getter not found for property: " + m_name);
		return Variant();
	}
	GDExtensionCallError error;
	return const_cast<Sandbox &>(sandbox).vmcall_internal(m_getter_address, nullptr, 0, error);
}
