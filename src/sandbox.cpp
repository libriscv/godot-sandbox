#include "sandbox.h"

#include <godot_cpp/core/class_db.hpp>

#include <charconv>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#ifdef RISCV_BINARY_TRANSLATION
#include <thread> // TODO: Use Godot's threading API.
#endif

using namespace godot;

static constexpr size_t MAX_HEAP = 16ull << 20;
static const int HEAP_SYSCALLS_BASE = 480;
static const int MEMORY_SYSCALLS_BASE = 485;

static inline String to_hex(gaddr_t value) {
	char str[20] = { 0 };
	std::to_chars(std::begin(str), std::end(str), value, 16);
	return String{ str };
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
	if (Engine::get_singleton()->is_editor_hint())
		return;
	PackedByteArray data = m_program_data->get_content();
	this->load(std::move(data), m_program_arguments);
}
Ref<ELFScript> Sandbox::get_program() {
	return m_program_data;
}
void Sandbox::load(PackedByteArray &&buffer, const TypedArray<String> &arguments) {
	if (buffer.is_empty()) {
		ERR_PRINT("Empty binary, cannot load program.");
		return;
	}
	this->m_binary = std::move(buffer);
	const auto binary_view = std::string_view{ (const char *)this->m_binary.ptr(), static_cast<size_t>(this->m_binary.size()) };

	try {
		delete this->m_machine;

		const riscv::MachineOptions<RISCV_ARCH> options{
			//.verbose_loader = true,
			.default_exit_function = "fast_exit",
#ifdef RISCV_BINARY_TRANSLATION
			.translate_background_callback =
					[](auto &compilation_step) {
						// TODO: Use Godot's threading API.
						std::thread([compilation_step = std::move(compilation_step)] {
							compilation_step();
						}).detach();
					}
#endif
		};

		this->m_machine = new machine_t{ binary_view, options };
		machine_t &m = machine();

		m.set_userdata(this);

		this->initialize_syscalls();

		const auto heap_area = machine().memory.mmap_allocate(MAX_HEAP);

		// Add native system call interfaces
		machine().setup_native_heap(HEAP_SYSCALLS_BASE, heap_area, MAX_HEAP);
		machine().setup_native_memory(MEMORY_SYSCALLS_BASE);

		std::vector<std::string> args;
		for (size_t i = 0; i < arguments.size(); i++) {
			args.push_back(arguments[i].operator String().utf8().get_data());
		}
		if (args.empty()) {
			// Create at least one argument (as required by main)
			args.push_back("program");
		}
		m.setup_linux(args);

		m.simulate(MAX_INSTRUCTIONS);
	} catch (const std::exception &e) {
		ERR_PRINT(("Sandbox exception: " + std::string(e.what())).c_str());
		this->handle_exception(machine().cpu.pc());
		// TODO: Program failed to load.
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
	return this->vmcall_internal(cached_address_of(String(function)), args, arg_count, error);
}
Variant Sandbox::vmcall_fn(const String &function, const Variant **args, GDExtensionInt arg_count) {
	GDExtensionCallError error;
	return this->vmcall_internal(cached_address_of(function), args, arg_count, error);
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
			m_machine->simulate_with(MAX_INSTRUCTIONS, 0u, address);
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
			cpu.preempt_internal(regs, true, address, MAX_INSTRUCTIONS);
		} else {
			throw std::runtime_error("Recursion level exceeded");
		}

		// Treat return value as pointer to Variant
		auto result = retvar->toVariant(*this);

		m_level--;
		m_scoped_variants.clear();

		error.error = GDEXTENSION_CALL_OK;
		return result;

	} catch (const std::exception &e) {
		m_level--;
		m_scoped_variants.clear();
		this->handle_exception(address);

		error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
		error.argument = -1;
		return Variant();
	}
}
Variant Sandbox::vmcallable(String function) {
	const auto address = cached_address_of(function);

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
	UtilityFunctions::print(
			"Program page: ", machine().memory.get_page_info(machine().cpu.pc()).c_str());
	UtilityFunctions::print(
			"Stack page: ", machine().memory.get_page_info(machine().cpu.reg(2)).c_str());

	if (m_machine->memory.binary().empty()) {
		ERR_PRINT("No binary loaded. Remember to assign a program to the Sandbox!");
	}
}

void Sandbox::handle_timeout(gaddr_t address) {
	this->m_budget_overruns++;
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
				String line_str(static_cast<std::string>(line).c_str());
				UtilityFunctions::print("-> ", line_str);
			});
	auto origin = machine().memory.lookup(addr);
	String name(origin.name.c_str());
	UtilityFunctions::print(
			"-> [-] 0x", to_hex(origin.address), " + 0x", to_hex(origin.offset),
			": ", name);
}

gaddr_t Sandbox::cached_address_of(String function) const {
	const auto ascii = function.ascii();
	const std::string str{ ascii.get_data(), (size_t)ascii.length() };

	// lookup function address
	gaddr_t address = 0x0;
	auto it = m_lookup.find(str);
	if (it == m_lookup.end()) {
		address = address_of(str);
		m_lookup.insert_or_assign(str, address);
	} else {
		address = it->second;
	}

	return address;
}

gaddr_t Sandbox::address_of(std::string_view name) const {
	return machine().address_of(name);
}
