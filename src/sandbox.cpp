#include "sandbox.h"

#include "guest_datatypes.h"
#include "sandbox_project_settings.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

GODOT_NAMESPACE

static const int HEAP_SYSCALLS_BASE = 480;
static const int MEMORY_SYSCALLS_BASE = 485;
static const std::vector<std::string> program_arguments = { "program" };
static riscv::Machine<RISCV_ARCH> *dummy_machine;

String Sandbox::_to_string() const {
	return "[ GDExtension::Sandbox <--> Instance ID:" + uitos(get_instance_id()) + " ]";
}

void Sandbox::_bind_methods() {
	// Constructors.
	ClassDB::bind_static_method("Sandbox", D_METHOD("FromBuffer", "buffer"), &Sandbox::FromBuffer);
	ClassDB::bind_static_method("Sandbox", D_METHOD("FromProgram", "program"), &Sandbox::FromProgram);
	// Methods.
	ClassDB::bind_method(D_METHOD("set_program", "program"), &Sandbox::set_program);
	ClassDB::bind_method(D_METHOD("get_program"), &Sandbox::get_program);
	ClassDB::bind_method(D_METHOD("has_program_loaded"), &Sandbox::has_program_loaded);
	ClassDB::bind_method(D_METHOD("load_buffer", "buffer"), &Sandbox::load_buffer);
	{
		MethodInfo mi;
		//mi.arguments.push_back(PropertyInfo(Variant::STRING, "function"));
		mi.name = "vmcall";
		mi.return_val = PropertyInfo(Variant::OBJECT, "result");
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "vmcall", &Sandbox::vmcall, mi, DEFVAL(std::vector<Variant>{}));
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "vmcallv", &Sandbox::vmcallv, mi, DEFVAL(std::vector<Variant>{}));
	}
	ClassDB::bind_method(D_METHOD("vmcallable", "function", "args"), &Sandbox::vmcallable, DEFVAL(Array{}));
	ClassDB::bind_method(D_METHOD("vmcallable_address", "address", "args"), &Sandbox::vmcallable_address, DEFVAL(Array{}));

	// Sandbox restrictions.
	ClassDB::bind_method(D_METHOD("set_restrictions"), &Sandbox::set_restrictions);
	ClassDB::bind_method(D_METHOD("get_restrictions"), &Sandbox::get_restrictions);
	ClassDB::bind_method(D_METHOD("add_allowed_object", "instance"), &Sandbox::add_allowed_object);
	ClassDB::bind_method(D_METHOD("remove_allowed_object", "instance"), &Sandbox::remove_allowed_object);
	ClassDB::bind_method(D_METHOD("clear_allowed_objects"), &Sandbox::clear_allowed_objects);
	ClassDB::bind_method(D_METHOD("set_class_allowed_callback", "instance"), &Sandbox::set_class_allowed_callback);
	ClassDB::bind_method(D_METHOD("set_object_allowed_callback", "instance"), &Sandbox::set_object_allowed_callback);
	ClassDB::bind_method(D_METHOD("set_method_allowed_callback", "instance"), &Sandbox::set_method_allowed_callback);
	ClassDB::bind_method(D_METHOD("set_property_allowed_callback", "instance"), &Sandbox::set_property_allowed_callback);
	ClassDB::bind_method(D_METHOD("set_resource_allowed_callback", "instance"), &Sandbox::set_resource_allowed_callback);
	ClassDB::bind_method(D_METHOD("is_allowed_class", "name"), &Sandbox::is_allowed_class);
	ClassDB::bind_method(D_METHOD("is_allowed_object", "instance"), &Sandbox::is_allowed_object);
	ClassDB::bind_method(D_METHOD("is_allowed_method", "instance", "method"), &Sandbox::is_allowed_method);
	ClassDB::bind_method(D_METHOD("is_allowed_property", "instance", "property", "is_set"), &Sandbox::is_allowed_property, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("is_allowed_resource", "res"), &Sandbox::is_allowed_resource);
	ClassDB::bind_static_method("Sandbox", D_METHOD("restrictive_callback_function"), &Sandbox::restrictive_callback_function);

	// Internal testing, debugging and introspection.
	ClassDB::bind_method(D_METHOD("set_redirect_stdout", "callback"), &Sandbox::set_redirect_stdout);
	ClassDB::bind_method(D_METHOD("get_general_registers"), &Sandbox::get_general_registers);
	ClassDB::bind_method(D_METHOD("get_floating_point_registers"), &Sandbox::get_floating_point_registers);
	ClassDB::bind_method(D_METHOD("set_argument_registers", "args"), &Sandbox::set_argument_registers);
	ClassDB::bind_method(D_METHOD("get_current_instruction"), &Sandbox::get_current_instruction);
	ClassDB::bind_method(D_METHOD("make_resumable"), &Sandbox::make_resumable);
	ClassDB::bind_method(D_METHOD("resume"), &Sandbox::resume);

	ClassDB::bind_method(D_METHOD("assault", "test", "iterations"), &Sandbox::assault);
	ClassDB::bind_method(D_METHOD("has_function", "function"), &Sandbox::has_function);
	ClassDB::bind_method(D_METHOD("get_functions"), &Sandbox::get_functions);
	ClassDB::bind_static_method("Sandbox", D_METHOD("generate_api", "language", "header_extra", "use_argument_names"), &Sandbox::generate_api, DEFVAL("cpp"), DEFVAL(""), DEFVAL(false));

	// Profiling.
	ClassDB::bind_method(D_METHOD("get_hotspots", "elf", "total"), &Sandbox::get_hotspots, DEFVAL(10));
	ClassDB::bind_method(D_METHOD("clear_hotspots"), &Sandbox::clear_hotspots);

	// Binary translation.
	ClassDB::bind_method(D_METHOD("emit_binary_translation", "ignore_instruction_limit", "automatic_nbit_address_space"), &Sandbox::emit_binary_translation, DEFVAL(true), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("is_binary_translated"), &Sandbox::is_binary_translated);

	// Properties.
	ClassDB::bind_method(D_METHOD("set", "name", "value"), &Sandbox::set);
	ClassDB::bind_method(D_METHOD("get", "name"), &Sandbox::get);
	ClassDB::bind_method(D_METHOD("get_property_list"), &Sandbox::get_property_list);

	ClassDB::bind_method(D_METHOD("set_max_refs", "max"), &Sandbox::set_max_refs, DEFVAL(MAX_REFS));
	ClassDB::bind_method(D_METHOD("get_max_refs"), &Sandbox::get_max_refs);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "references_max", PROPERTY_HINT_NONE, "Maximum objects and variants referenced by a sandbox call"), "set_max_refs", "get_max_refs");

	ClassDB::bind_method(D_METHOD("set_memory_max", "max"), &Sandbox::set_memory_max, DEFVAL(MAX_VMEM));
	ClassDB::bind_method(D_METHOD("get_memory_max"), &Sandbox::get_memory_max);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "memory_max", PROPERTY_HINT_NONE, "Maximum memory (in MiB) used by the sandboxed program"), "set_memory_max", "get_memory_max");

	ClassDB::bind_method(D_METHOD("set_instructions_max", "max"), &Sandbox::set_instructions_max, DEFVAL(MAX_INSTRUCTIONS));
	ClassDB::bind_method(D_METHOD("get_instructions_max"), &Sandbox::get_instructions_max);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "execution_timeout", PROPERTY_HINT_NONE, "Maximum millions of instructions executed before cancelling execution"), "set_instructions_max", "get_instructions_max");

	ClassDB::bind_method(D_METHOD("set_use_unboxed_arguments", "use_unboxed_arguments"), &Sandbox::set_use_unboxed_arguments);
	ClassDB::bind_method(D_METHOD("get_use_unboxed_arguments"), &Sandbox::get_use_unboxed_arguments);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_unboxed_arguments", PROPERTY_HINT_NONE, "Use unboxed arguments for VM function calls"), "set_use_unboxed_arguments", "get_use_unboxed_arguments");

	ClassDB::bind_method(D_METHOD("set_use_precise_simulation", "use_precise_simulation"), &Sandbox::set_use_precise_simulation);
	ClassDB::bind_method(D_METHOD("get_use_precise_simulation"), &Sandbox::get_use_precise_simulation);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_precise_simulation", PROPERTY_HINT_NONE, "Use precise simulation for VM execution"), "set_use_precise_simulation", "get_use_precise_simulation");

	ClassDB::bind_method(D_METHOD("set_profiling", "enable"), &Sandbox::set_profiling, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_profiling"), &Sandbox::get_profiling);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "profiling", PROPERTY_HINT_NONE, "Enable profiling of VM calls"), "set_profiling", "get_profiling");

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "restrictions", PROPERTY_HINT_NONE, "Enable sandbox restrictions"), "set_restrictions", "get_restrictions");

	// Group for monitored Sandbox health.
	ADD_GROUP("Sandbox Monitoring", "monitor_");

	ClassDB::bind_method(D_METHOD("get_heap_usage"), &Sandbox::get_heap_usage);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_heap_usage", PROPERTY_HINT_NONE, "Current arena usage"), "", "get_heap_usage");

	ClassDB::bind_method(D_METHOD("get_exceptions"), &Sandbox::get_exceptions);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_exceptions", PROPERTY_HINT_NONE, "Number of exceptions thrown"), "", "get_exceptions");

	ClassDB::bind_method(D_METHOD("get_timeouts"), &Sandbox::get_timeouts);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_execution_timeouts", PROPERTY_HINT_NONE, "Number of execution timeouts"), "", "get_timeouts");

	ClassDB::bind_method(D_METHOD("get_calls_made"), &Sandbox::get_calls_made);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_calls_made", PROPERTY_HINT_NONE, "Number of calls made"), "", "get_calls_made");

	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_calls_made"), &Sandbox::get_global_calls_made);
	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_exceptions"), &Sandbox::get_global_exceptions);
	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_timeouts"), &Sandbox::get_global_timeouts);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_calls_made", PROPERTY_HINT_NONE, "Number of calls made"), "", "get_global_calls_made");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_exceptions", PROPERTY_HINT_NONE, "Number of exceptions thrown"), "", "get_global_exceptions");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_execution_timeouts", PROPERTY_HINT_NONE, "Number of execution timeouts"), "", "get_global_timeouts");

	ClassDB::bind_static_method("Sandbox", D_METHOD("get_global_instance_count"), &Sandbox::get_global_instance_count);
	ClassDB::bind_static_method("Sandbox", D_METHOD("get_accumulated_startup_time"), &Sandbox::get_accumulated_startup_time);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "monitor_global_instance_count", PROPERTY_HINT_NONE, "Number of active sandbox instances"), "", "get_global_instance_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "monitor_accumulated_startup_time", PROPERTY_HINT_NONE, "Accumulated startup time of all sandbox instantiations"), "", "get_accumulated_startup_time");

	// Group for sandboxed properties.
	ADD_GROUP("Sandboxed Properties", "custom_");
}

std::vector<PropertyInfo> Sandbox::create_sandbox_property_list() const {
	std::vector<PropertyInfo> list;
	// Create a list of properties for the Sandbox class only.
	// This is used to expose the basic properties to the editor.

	// Group for sandbox restrictions.
	list.push_back(PropertyInfo(Variant::INT, "references_max", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::INT, "memory_max", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::INT, "execution_timeout", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::BOOL, "use_unboxed_arguments", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::BOOL, "use_precise_simulation", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::BOOL, "profiling", PROPERTY_HINT_NONE));
	list.push_back(PropertyInfo(Variant::BOOL, "restrictions", PROPERTY_HINT_NONE));

	// Group for monitored Sandbox health.
	// Add the group name to the property name to group them in the editor.
	list.push_back(PropertyInfo(Variant::NIL, "Monitoring", PROPERTY_HINT_NONE, "monitor_", PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SCRIPT_VARIABLE));
	list.push_back(PropertyInfo(Variant::INT, "monitor_heap_usage", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY | PROPERTY_USAGE_SCRIPT_VARIABLE));
	list.push_back(PropertyInfo(Variant::INT, "monitor_exceptions", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY | PROPERTY_USAGE_SCRIPT_VARIABLE));
	list.push_back(PropertyInfo(Variant::INT, "monitor_execution_timeouts", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY | PROPERTY_USAGE_SCRIPT_VARIABLE));
	list.push_back(PropertyInfo(Variant::INT, "monitor_calls_made", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY | PROPERTY_USAGE_SCRIPT_VARIABLE));

	return list;
}

void Sandbox::constructor_initialize() {
	this->m_current_state = &this->m_states[0];
	this->m_use_unboxed_arguments = SandboxProjectSettings::use_native_types();
	// For each call state, reset the state
	for (size_t i = 0; i < this->m_states.size(); i++) {
		this->m_states[i].reinitialize(i, this->m_max_refs);
	}
}
void Sandbox::reset_machine() {
	try {
		if (this->m_machine != dummy_machine) {
			delete this->m_machine;
			this->m_machine = dummy_machine;
		}
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
	}
}
void Sandbox::full_reset() {
	this->reset_machine();
	const bool use_unboxed_arguments = this->get_use_unboxed_arguments();
	this->constructor_initialize();
	this->set_use_unboxed_arguments(use_unboxed_arguments);

	this->m_properties.clear();
	this->m_lookup.clear();
	this->m_allowed_objects.clear();
}
Sandbox::Sandbox() {
	if (dummy_machine == nullptr) {
		dummy_machine = new machine_t{};
	}
	this->constructor_initialize();
	this->m_tree_base = this;
	this->m_global_instance_count += 1;
	// In order to reduce checks we guarantee that this
	// class is well-formed at all times.
	this->reset_machine();
}
Sandbox::Sandbox(const PackedByteArray &buffer) : Sandbox() {
	this->load_buffer(buffer);
}
Sandbox::Sandbox(Ref<ELFScript> program) : Sandbox() {
	this->set_program(program);
}

Sandbox::~Sandbox() {
	if (this->is_in_vmcall()) {
		ERR_PRINT("Sandbox instance destroyed while a VM call is in progress.");
	}
	this->m_global_instance_count -= 1;
	this->set_program_data_internal(nullptr);
	try {
		if (this->m_machine != dummy_machine)
			delete this->m_machine;
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
	}
}

void Sandbox::set_program(Ref<ELFScript> program) {
	// Check if a call is being made from the VM already,
	// which could spell trouble when we now reset the machine.
	if (this->is_in_vmcall()) {
		ERR_PRINT("Cannot load a new program while a VM call is in progress.");
		return;
	}

	// Avoid reloading the same program
	if (program.is_valid() && this->m_program_data == program) {
		if (this->m_source_version == program->get_source_version()) {
			return;
		}
	} else {
		this->m_source_version = -1;
	}

	// Try to retain Sandboxed properties
	std::vector<SandboxProperty> properties = std::move(this->m_properties);
	std::vector<Variant> property_values;
	property_values.reserve(properties.size());
	for (const SandboxProperty &prop : properties) {
		Variant value;
		this->get_property(prop.name(), value);
		property_values.push_back(value);
	}

	this->set_program_data_internal(program);
	this->m_program_bytes = {};

	// Unload program and reset the machine
	this->full_reset();

	if (this->m_program_data.is_null())
		return;

	if (this->load(&m_program_data->get_content())) {
		this->m_source_version = m_program_data->get_source_version();
	}

	// Restore Sandboxed properties by comparing the new program's properties
	// with the old ones, then comparing the type. If the property is found,
	// try to set the property with the old value.
	for (const SandboxProperty &old_prop : properties) {
		const Variant *value = nullptr;
		for (const SandboxProperty &new_prop : this->m_properties) {
			if (new_prop.name() == old_prop.name() && new_prop.type() == old_prop.type()) {
				value = &property_values[&old_prop - &properties[0]];
				break;
			}
		}
		if (value) {
			this->set_property(old_prop.name(), *value);
		}
	}
}
void Sandbox::set_program_data_internal(Ref<ELFScript> program) {
	if (this->m_program_data.is_valid()) {
		//printf("Sandbox %p: Program *unset* from %s\n", this, this->m_program_data->get_path().utf8().ptr());
		this->m_program_data->unregister_instance(this);
	}
	this->m_program_data = program;
	if (this->m_program_data.is_valid()) {
		//printf("Sandbox %p: Program set to %s\n", this, this->m_program_data->get_path().utf8().ptr());
		this->m_program_data->register_instance(this);
	}
}
Ref<ELFScript> Sandbox::get_program() {
	return m_program_data;
}
void Sandbox::load_buffer(const PackedByteArray &buffer) {
	// Check if a call is being made from the VM already,
	// which could spell trouble when we now reset the machine.
	if (this->is_in_vmcall()) {
		ERR_PRINT("Cannot load a new program while a VM call is in progress.");
		return;
	}

	this->set_program_data_internal(nullptr);
	this->m_program_bytes = buffer;
	this->load(&this->m_program_bytes);
}
bool Sandbox::has_program_loaded() const {
	return !machine().memory.binary().empty();
}
bool Sandbox::load(const PackedByteArray *buffer, const std::vector<std::string> *argv_ptr) {
	if (buffer == nullptr || buffer->is_empty()) {
		ERR_PRINT("Empty binary, cannot load program.");
		this->reset_machine();
		return false;
	}
	const std::string_view binary_view = std::string_view{ (const char *)buffer->ptr(), static_cast<size_t>(buffer->size()) };

	// Get t0 for the startup time
	const uint64_t startup_t0 = Time::get_singleton()->get_ticks_usec();

	/** We can't handle exceptions until the Machine is fully constructed. Two steps.  */
	try {
		if (this->m_machine != dummy_machine)
			delete this->m_machine;

		auto options = std::make_shared<riscv::MachineOptions<RISCV_ARCH>>(riscv::MachineOptions<RISCV_ARCH>{
				.memory_max = uint64_t(get_memory_max()) << 20, // in MiB
				//.verbose_loader = true,
				.default_exit_function = "fast_exit",
#ifdef RISCV_BINARY_TRANSLATION
				.translate_enabled = false,
				.translate_enable_embedded = true,
				.translate_future_segments = false,
				.translate_invoke_compiler = false,
				//.translate_trace = true,
				//.translate_timing = true,
				// We don't care about the instruction limit when full binary translation is enabled
				// Specifically, for the Machines where full binary translation is *available*, so
				// technically we need a way to check if a Machine has it available before setting this.
				.translate_ignore_instruction_limit = true,
#endif
		});

		this->m_machine = new machine_t{ binary_view, *options };
		this->m_machine->set_options(std::move(options));
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox construction exception: " + std::string(e.what())).c_str());
		this->m_machine = dummy_machine;
		return false;
	}

	/** Now we can process symbols, backtraces etc. */
	try {
		machine_t &m = machine();

		m.set_userdata(this);
		m.set_printer([](const machine_t &m, const char *str, size_t len) {
			Sandbox *sandbox = m.get_userdata<Sandbox>();
			sandbox->print(String::utf8(str, len));
		});

		this->initialize_syscalls();

		const gaddr_t heap_size = MAX_HEAP << 20; // in MiB
		const gaddr_t heap_area = machine().memory.mmap_allocate(heap_size);

		// Add native system call interfaces
		machine().setup_native_heap(HEAP_SYSCALLS_BASE, heap_area, heap_size);
		machine().setup_native_memory(MEMORY_SYSCALLS_BASE);

		// Set up a Linux environment for the program
		const std::vector<std::string> *argv = argv_ptr ? argv_ptr : &program_arguments;
		m.setup_linux(*argv, { "LC_CTYPE=C", "LC_ALL=C", "TZ=UTC", "LD_LIBRARY_PATH=" });

		// Run the program through to its main() function
		if (!this->m_resumable_mode) {
			if (!this->get_use_precise_simulation()) {
				m.simulate(get_instructions_max() << 30);
			} else {
				// Precise simulation can help discover bugs in the program,
				// as the exact PC address will be known when an exception occurs.
				m.set_max_instructions(get_instructions_max() << 30);
				m.cpu.simulate_precise();
			}
		}
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
		this->handle_exception(machine().cpu.pc());
	}

	// Read the program's custom properties, if any
	this->read_program_properties(true);

	// Accumulate startup time
	const uint64_t startup_t1 = Time::get_singleton()->get_ticks_usec();
	double startup_time = (startup_t1 - startup_t0) / 1e6;
	m_accumulated_startup_time += startup_time;
	//fprintf(stderr, "Sandbox startup time: %.3f seconds\n", startup_time);
	return true;
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
	Variant result = this->vmcall_internal(cached_address_of(function.hash(), function), args, arg_count);
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
			case Variant::VECTOR3: {
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec3_flt[0];
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec3_flt[2];
				break;
			}
			case Variant::VECTOR3I: {
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->ivec3_int[0];
				machine.cpu.reg(index++) = inner->ivec3_int[2];
				break;
			}
			case Variant::VECTOR4: {
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec4_flt[0];
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec4_flt[2];
				break;
			}
			case Variant::VECTOR4I: {
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->ivec4_int[0];
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->ivec4_int[2];
				break;
			}
			case Variant::COLOR: { // 16-byte struct (must use integer registers)
				// RVG calling convention:
				// Unions and arrays containing floats are passed in integer registers
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->color_flt[0];
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->color_flt[2];
				break;
			}
			case Variant::PLANE: {
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec4_flt[0];
				machine.cpu.reg(index++) = *(gaddr_t *)&inner->vec4_flt[2];
				break;
			}
			case Variant::OBJECT: { // Objects are represented as uintptr_t
				Object *obj = inner->to_object();
				this->add_scoped_object(obj);
				machine.cpu.reg(index++) = uintptr_t(obj); // Fits in a single register
				break;
			}
			case Variant::ARRAY:
			case Variant::DICTIONARY:
			case Variant::STRING:
			case Variant::STRING_NAME:
			case Variant::NODE_PATH:
			case Variant::RID:
			case Variant::CALLABLE:
			case Variant::TRANSFORM2D:
			case Variant::BASIS:
			case Variant::TRANSFORM3D:
			case Variant::QUATERNION:
			case Variant::PACKED_BYTE_ARRAY:
			case Variant::PACKED_FLOAT32_ARRAY:
			case Variant::PACKED_FLOAT64_ARRAY:
			case Variant::PACKED_INT32_ARRAY:
			case Variant::PACKED_INT64_ARRAY:
			case Variant::PACKED_VECTOR2_ARRAY:
			case Variant::PACKED_VECTOR3_ARRAY:
			case Variant::PACKED_COLOR_ARRAY:
			case Variant::PACKED_STRING_ARRAY: { // Uses Variant index to reference the object
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
			case Variant::OBJECT: {
				Object *obj = inner->to_object();
				// Objects passed directly as arguments are implicitly trusted/allowed
				g_arg.set_object(*this, obj);
				break;
			}
			default:
				g_arg.set(*this, *args[i], true);
		}
		m_machine->cpu.reg(11 + i) = arrayDataPtr + (i + 1) * sizeof(GuestVariant);
	}
	// A0 is the return value (Variant) of the function
	return &v[0];
}
Variant Sandbox::vmcall_internal(gaddr_t address, const Variant **args, int argc) {
	this->m_current_state += 1;
	if (UNLIKELY(this->m_current_state >= this->m_states.end())) {
		ERR_PRINT("Too many VM calls in progress");
		this->m_exceptions ++;
		this->m_global_exceptions ++;
		this->m_current_state -= 1;
		return Variant();
	}

	CurrentState &state = *this->m_current_state;
	const bool is_reentrant_call = (this->m_current_state - this->m_states.begin()) > 1;
	state.reset();

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
			if (UNLIKELY(this->m_precise_simulation)) {
				m_machine->set_instruction_counter(0);
				m_machine->set_max_instructions(get_instructions_max() << 20);
				m_machine->cpu.jump(address);
				m_machine->cpu.simulate_precise();
			} else if (UNLIKELY(this->m_profiling_data != nullptr)) {
				m_machine->cpu.jump(address);
				ProfilingData &profdata = *this->m_profiling_data;
				do {
					const int32_t next = std::max(int32_t(0), int32_t(profdata.profiling_interval) - int32_t(profdata.profiler_icounter_accumulator));
					m_machine->simulate<false>(next, 0u);
					if (m_machine->instruction_limit_reached()) {
						profdata.profiler_icounter_accumulator = 0;
						profdata.visited[m_machine->cpu.pc()]++;
					}
				} while (m_machine->instruction_limit_reached());
				// update the accumulator with the remaining instructions
				profdata.profiler_icounter_accumulator += m_machine->instruction_counter();
			} else {
				m_machine->simulate_with(get_instructions_max() << 20, 0u, address);
			}
		} else {
			riscv::Registers<RISCV_ARCH> regs;
			regs = cpu.registers();
			// we are in a recursive call, so wait before setting exit address
			cpu.reg(riscv::REG_RA) = m_machine->memory.exit_address();
			// we need to make some stack room
			sp -= 16u;
			// set up each argument, and return value
			retvar = this->setup_arguments(sp, args, argc);
			// execute preemption! (precise simulation not supported)
			cpu.preempt_internal(regs, true, address, get_instructions_max() << 20);
		}

		// Treat return value as pointer to Variant
		Variant result = retvar->toVariant(*this);
		// Restore the previous state
		this->m_current_state -= 1;
		return result;

	} catch (const std::exception &e) {
		if (Engine::get_singleton()->is_editor_hint()) {
			// Throttle exceptions in the sandbox when calling from the editor
			this->m_throttled += EDITOR_THROTTLE;
		}
		this->handle_exception(address);
		// TODO: Free the function arguments and return value? Will help keep guest memory clean

		this->m_current_state -= 1;
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

gaddr_t Sandbox::cached_address_of(int64_t hash, const String &function) const {
	gaddr_t address = 0x0;
	auto it = m_lookup.find(hash);
	if (it == m_lookup.end()) {
		const CharString ascii = function.ascii();
		const std::string_view str{ ascii.get_data(), (size_t)ascii.length() };
		address = address_of(str);
		if (address != 0x0) {
			// We tolerate exceptions here, as we are just trying to improve performance
			// If that fails, the function will still work, just a tiny bit slower
			// Some broken linker scripts put a few late functions outside of .text, so we will
			// just have to catch the exception and move on
			try {
				// Cheating a bit here, as we are in a const function
				// But this does not functionally change the Machine, it only boosts performance a bit
				const_cast<machine_t *>(m_machine)->cpu.create_fast_path_function(address);
			} catch (const riscv::MachineException &me) {
				String error = "Sandbox exception: " + String(me.what());
				char buffer[32];
				snprintf(buffer, sizeof(buffer), " (Address 0x%lX)", long(address));
				error += buffer;
				ERR_PRINT(error);
			}
		}
		m_lookup.insert_or_assign(hash, address);
	} else {
		address = it->second;
	}

	return address;
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
	CurrentState &st = this->state();
	if (st.scoped_variants.size() >= st.variants.capacity()) {
		ERR_PRINT("Maximum number of scoped variants reached.");
		throw std::runtime_error("Maximum number of scoped variants reached.");
	}
	st.scoped_variants.push_back(value);
	if (&st != &this->m_states[0])
		return int32_t(st.scoped_variants.size()) - 1;
	else
		return -int32_t(st.scoped_variants.size());
}
unsigned Sandbox::create_scoped_variant(Variant &&value) const {
	CurrentState &st = this->state();
	if (st.scoped_variants.size() >= st.variants.capacity()) {
		ERR_PRINT("Maximum number of scoped variants reached.");
		throw std::runtime_error("Maximum number of scoped variants reached.");
	}
	st.append(std::move(value));
	if (&st != &this->m_states[0])
		return int32_t(st.scoped_variants.size()) - 1;
	else
		return -int32_t(st.scoped_variants.size());
}
std::optional<const Variant *> Sandbox::get_scoped_variant(int32_t index) const noexcept {
	if (index >= 0 && index < state().scoped_variants.size()) {
		return state().scoped_variants[index];
	} else if (index < 0) {
		// Negative index is access into initialization state
		index = -index - 1;
		auto &init_state = this->m_states[0];
		if (index < init_state.scoped_variants.size()) {
			return init_state.scoped_variants[index];
		}
	}
	ERR_PRINT("Invalid scoped variant index: " + itos(index));
	return std::nullopt;
}
Variant &Sandbox::get_mutable_scoped_variant(int32_t index) {
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
		if (state().variants.size() >= state().variants.capacity()) {
			ERR_PRINT("Maximum number of scoped variants reached.");
			throw std::runtime_error("Maximum number of scoped variants reached.");
		}
		state().append(Variant(*var));
		return state().variants.back();
	}
	return *it;
}
unsigned Sandbox::create_permanent_variant(unsigned idx) {
	if (int32_t(idx) < 0) {
		// It's already a permanent variant
		return idx;
	}
	std::optional<const Variant *> var_opt = get_scoped_variant(idx);
	if (!var_opt.has_value()) {
		ERR_PRINT("Invalid scoped variant index.");
		throw std::runtime_error("Invalid scoped variant index: " + std::to_string(idx));
	}
	const Variant *var = var_opt.value();
	// Find the variant in the variants list
	auto it = std::find_if(state().variants.begin(), state().variants.end(), [var](const Variant &v) {
		return &v == var;
	});

	CurrentState &perm_state = this->m_states[0];
	if (perm_state.variants.size() >= perm_state.variants.capacity()) {
		ERR_PRINT("Maximum number of scoped variants in permanent state reached.");
		// Just return the old scoped variant
		return idx;
	}

	if (it == state().variants.end()) {
		// Create a new variant in the permanent list
		perm_state.append(var->duplicate());
	} else {
		// Move the variant to the permanent list, leave the old one in the scoped list
		perm_state.append(std::move(*it));
	}
	unsigned perm_idx = perm_state.variants.size() - 1;
	// Return the index of the new permanent variant converted to negative
	return -int32_t(perm_idx) - 1;
}
void Sandbox::assign_permanent_variant(int32_t idx, Variant &&val) {
	if (idx < 0) {
		// It's a permanent variant, verify the index
		idx = -idx - 1;
		if (idx < this->m_states[0].variants.size()) {
			this->m_states[0].variants[idx] = std::move(val);
			return;
		}
	}
	// It's either a scoped (temporary) variant, or invalid
	ERR_PRINT("Invalid permanent variant index.");
	throw std::runtime_error("Invalid permanent variant index: " + std::to_string(idx));
}
unsigned Sandbox::try_reuse_assign_variant(int32_t src_idx, const Variant &src_var, int32_t assign_to_idx, const Variant &new_value) {
	if (this->is_permanent_variant(assign_to_idx)) {
		// The Variant is permanent, so we need to assign it directly.
		// Permanent Variants are scarce and should not be duplicated.
		this->assign_permanent_variant(assign_to_idx, Variant(new_value));
		return assign_to_idx;
	} else if (assign_to_idx == src_idx && this->state().is_mutable_variant(src_var)) {
		// They are the same, and the Variant belongs to the current state, so we can modify it directly.
		const_cast<Variant &>(src_var) = new_value;
		return assign_to_idx;
	} else {
		// The Variant is either temporary or invalid, so we can replace it directly.
		return this->create_scoped_variant(Variant(new_value));
	}
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
	gaddr_t prop_addr = 0x0;
	try {
		// Properties is an array named properties, that ends with an invalid property
		prop_addr = machine().address_of("properties");
		if (prop_addr == 0x0)
			return;
	} catch (...) {
		return;
	}
	try {
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
				//ERR_PRINT("Sandbox: Invalid property size");
				break;
			}
			const std::string c_name = machine().memory.memstring(prop->g_name);
			Variant def_val = prop->def_val.toVariant(*this);

			this->add_property(String::utf8(c_name.c_str(), c_name.size()), prop->type, prop->setter, prop->getter, def_val);
		}
	} catch (const std::exception &e) {
		ERR_PRINT("Sandbox exception in " + get_name() + " while reading properties: " + String(e.what()));
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
	if (name == StringName("references_max")) {
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
	} else if (name == StringName("use_precise_simulation")) {
		set_use_precise_simulation(value);
		return true;
	} else if (name == StringName("profiling")) {
		set_profiling(value);
		return true;
	} else if (name == StringName("restrictions")) {
		set_restrictions(value);
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
	if (name == StringName("references_max")) {
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
	} else if (name == StringName("use_precise_simulation")) {
		r_ret = get_use_precise_simulation();
		return true;
	} else if (name == StringName("profiling")) {
		r_ret = get_profiling();
		return true;
	} else if (name == StringName("restrictions")) {
		r_ret = get_restrictions();
		return true;
	} else if (name == StringName("monitor_heap_usage")) {
		r_ret = get_heap_usage();
		return true;
	} else if (name == StringName("monitor_exceptions")) {
		r_ret = get_exceptions();
		return true;
	} else if (name == StringName("monitor_execution_timeouts")) {
		r_ret = get_timeouts();
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
	} else if (name == StringName("global_timeouts")) {
		r_ret = get_global_timeouts();
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

Variant Sandbox::get(const StringName &name) {
	Variant result;
	if (get_property(name, result)) {
		return result;
	}
	// Get as if it's on the underlying Node object
	return Node::get(name);
}

void Sandbox::set(const StringName &name, const Variant &value) {
	if (!set_property(name, value)) {
		// Set as if it's on the underlying Node object
		Node::set(name, value);
	}
}

Array Sandbox::get_property_list() const {
	Array arr;
	// Sandboxed properties
	for (const SandboxProperty &prop : m_properties) {
		Dictionary d;
		d["name"] = prop.name();
		d["type"] = prop.type();
		d["usage"] = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_SCRIPT_VARIABLE;
		arr.push_back(d);
	}
	// Our properties
	for (const PropertyInfo &prop : this->create_sandbox_property_list()) {
		Dictionary d;
		d["name"] = prop.name;
		d["type"] = prop.type;
		d["usage"] = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_SCRIPT_VARIABLE;;
		arr.push_back(d);
	}
	// Node properties
	arr.append_array(Node::get_property_list());
	return arr;
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

void Sandbox::CurrentState::initialize(unsigned level, unsigned max_refs) {
	(void)level;
	this->variants.reserve(max_refs);
}
void Sandbox::CurrentState::reinitialize(unsigned level, unsigned max_refs) {
	(void)level;
	this->variants.reserve(max_refs);
	this->variants.clear();
	this->scoped_objects.clear();
	this->scoped_variants.clear();
}
bool Sandbox::CurrentState::is_mutable_variant(const Variant &var) const {
	// Check if the address of the variant is within the range of the current state std::vector
	const Variant *ptr = &var;
	return ptr >= &variants[0] && ptr < &variants[0] + variants.size();
}

void Sandbox::set_max_refs(uint32_t max) {
	this->m_max_refs = max;
	// If we are not in a call, reset the states
	if (!this->is_in_vmcall()) {
		for (size_t i = 0; i < this->m_states.size(); i++) {
			this->m_states[i].initialize(i, max);
		}
	} else {
		ERR_PRINT("Sandbox: Cannot change max references during a Sandbox call.");
	}
}

String Sandbox::emit_binary_translation(bool ignore_instruction_limit, bool automatic_nbit_as) const {
	const std::string_view &binary = machine().memory.binary();
	if (binary.empty()) {
		ERR_PRINT("Sandbox: No binary loaded.");
		return String();
	}
#ifdef RISCV_BINARY_TRANSLATION
	// 1. Re-create the same options
	auto options = std::make_shared<riscv::MachineOptions<RISCV_ARCH>>(machine().options());
	options->use_shared_execute_segments = false;
	options->translate_enabled = false;
	options->translate_enable_embedded = true;
	options->translate_invoke_compiler = false;
	options->translate_ignore_instruction_limit = ignore_instruction_limit;
	options->translate_automatic_nbit_address_space = automatic_nbit_as;

	// 2. Enable binary translation output to a string
	std::string code_output;
	options->cross_compile.push_back(riscv::MachineTranslationEmbeddableCodeOptions{
			.result_c99 = &code_output,
	});

	// 3. Emit the binary translation by constructing a new machine
	machine_t m{ binary, *options };

	// 4. Verify that the translation was successful
	if (code_output.empty()) {
		ERR_PRINT("Sandbox: Binary translation failed.");
		return String();
	}
	// 5. Return the translated code
	return String::utf8(code_output.c_str(), code_output.size());
#else
	ERR_PRINT("Sandbox: Binary translation is not enabled.");
	return String();
#endif
}

bool Sandbox::is_binary_translated() const {
	return this->m_machine->is_binary_translation_enabled();
}

void Sandbox::print(const Variant &v) {
	static bool already_been_here = false;
	if (already_been_here) {
		ERR_PRINT("Recursive call to Sandbox::print() detected, ignoring.");
		return;
	}
	already_been_here = true;

	if (this->m_redirect_stdout.is_valid()) {
		// Redirect to a GDScript callback function
		this->m_redirect_stdout.call(v);
	} else {
		// Print to the console
		UtilityFunctions::print(v);
	}

	already_been_here = false;
}
