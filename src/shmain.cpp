#include "riscv.hpp"

#include <godot_cpp/core/class_db.hpp>

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#ifdef RISCV_BINARY_TRANSLATION
#include <thread> // TODO: Use Godot's threading API.
#endif

using namespace godot;

// There are two APIs:
// 1. The API that always makes sense, eg. Timers, Nodes, etc.
//    This will be using system calls. Assign a fixed-number.
// 2. The Game-specific API, eg. whatever bullshit you need for your game to be fucking awesome.
//    (will be implemented later)

static constexpr size_t MAX_HEAP = 16ull << 20;
static const int HEAP_SYSCALLS_BASE = 570;
static const int MEMORY_SYSCALLS_BASE = 575;
static const int THREADS_SYSCALL_BASE = 590;

String Sandbox::_to_string() const {
	return "[ GDExtension::Sandbox <--> Instance ID:" + uitos(get_instance_id()) + " ]";
}

void Sandbox::_bind_methods() {
	// Methods.
	ClassDB::bind_method(D_METHOD("load"), &Sandbox::load);
	{
		MethodInfo mi;
		mi.arguments.push_back(PropertyInfo(Variant::STRING, "function"));
		mi.name = "vmcall";
		mi.return_val = PropertyInfo(Variant::OBJECT, "result");
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "vmcall", &Sandbox::vmcall, mi);
	}
	ClassDB::bind_method(D_METHOD("vmcallable"), &Sandbox::vmcallable);
}

Sandbox::Sandbox() {
	// In order to reduce checks we guarantee that this
	// class is well-formed at all times.
	this->m_machine = new machine_t{};
	set_name("(name)");
	UtilityFunctions::print("Constructor, sizeof(Variant) == ", static_cast<int32_t>(sizeof(Variant)));
	UtilityFunctions::print("Constructor, alignof(Variant) == ", static_cast<int32_t>(alignof(Variant)));
}

Sandbox::~Sandbox() {
	UtilityFunctions::print("Destructor.");
	delete this->m_machine;
}

// Methods.
void Sandbox::load(const PackedByteArray &buffer, const TypedArray<String> &arguments) {
	UtilityFunctions::print("Loading file from buffer");

	m_binary = std::vector<uint8_t>{ buffer.ptr(), buffer.ptr() + buffer.size() };

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

		this->m_machine = new machine_t{ this->m_binary, options };
		machine_t &m = machine();

		m.set_userdata(this);

		this->initialize_syscalls();

		const auto heap_area = machine().memory.mmap_allocate(MAX_HEAP);

		// Add native system call interfaces
		machine().setup_native_heap(HEAP_SYSCALLS_BASE, heap_area, MAX_HEAP);
		machine().setup_native_memory(MEMORY_SYSCALLS_BASE);
		machine().setup_native_threads(THREADS_SYSCALL_BASE);

		m.setup_linux({ "program" });

		m.simulate(MAX_INSTRUCTIONS);
	} catch (const std::exception &e) {
		this->handle_exception(machine().cpu.pc());
	}
}

Variant Sandbox::vmcall_address(gaddr_t address, const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	error.error = GDEXTENSION_CALL_OK;
	return this->vmcall_internal(address, args, arg_count);
}
Variant Sandbox::vmcall(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error) {
	if (arg_count < 1) {
		error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		return Variant();
	}
	auto &function = *args[0];
	args += 1;
	arg_count -= 1;
	error.error = GDEXTENSION_CALL_OK;
	return this->vmcall_internal(cached_address_of(String(function)), args, arg_count);
}
void Sandbox::setup_arguments(const Variant **args, int argc) {
	// Stack pointer
	auto &sp = m_machine->cpu.reg(2);
	sp -= sizeof(GuestVariant) * argc;
	const auto spanDataPtr = sp;
	const auto spanElements = argc;

	auto v = m_machine->memory.rvspan<GuestVariant>(spanDataPtr, spanElements);

	for (size_t i = 0; i < argc; i++) {
		//printf("args[%zu] = type %d\n", i, int(args[i]->get_type()));
		v[i].set(*this, (*args)[i]);
	}
	sp &= ~gaddr_t(0xF); // align stack pointer

	// Set up a std::span of GuestVariant
	m_machine->cpu.reg(10) = spanDataPtr;
	m_machine->cpu.reg(11) = spanElements;
}
Variant Sandbox::vmcall_internal(gaddr_t address, const Variant **args, int argc) {
	try {
		m_level++;
		// execute guest function
		if (m_level == 1) {
			// reset the stack pointer to an initial location (deliberately)
			m_machine->cpu.reset_stack_pointer();
			// setup calling convention
			m_machine->setup_call();
			// set up each argument
			this->setup_arguments(args, argc);
			// execute!
			m_machine->simulate_with(MAX_INSTRUCTIONS, 0u, address);
		} else if (m_level > 1 && m_level < MAX_LEVEL) {
			riscv::Registers<RISCV_ARCH> regs;
			regs = m_machine->cpu.registers();
			// we need to make some stack room
			m_machine->cpu.reg(riscv::REG_SP) -= 16u;
			// setup calling convention
			m_machine->setup_call();
			// set up each argument
			this->setup_arguments(args, argc);
			// execute!
			return m_machine->cpu.preempt_internal(regs, true, address, MAX_INSTRUCTIONS);
		} else {
			throw std::runtime_error("Recursion level exceeded");
		}

		m_level--;
		m_scoped_variants.clear();
		return m_machine->return_value<int64_t>();
	} catch (const std::exception &e) {
		m_level--;
		m_scoped_variants.clear();
		this->handle_exception(address);
	}
	return -1;
}
Variant Sandbox::vmcallable(String function) {
	const auto address = cached_address_of(function);

	auto *call = memnew(RiscvCallable);
	call->init(this, address);
	return Callable(call);
}
void RiscvCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	auto result = self->vmcall_internal(address, p_arguments, p_argcount);

	r_return_value = result;
	r_call_error.error = GDEXTENSION_CALL_OK;
}

void Sandbox::execute() {
	machine_t &m = machine();

	UtilityFunctions::print("Simulating...");
	m.simulate(MAX_INSTRUCTIONS);
	UtilityFunctions::print("Done, instructions: ", m.instruction_counter(),
			" result: ", m.return_value<int64_t>());
}

void Sandbox::handle_exception(gaddr_t address) {
	auto callsite = machine().memory.lookup(address);
	UtilityFunctions::print(
			"[", get_name(), "] Exception when calling:\n  ", callsite.name.c_str(), " (0x",
			String("%x").format(callsite.address), ")\n", "Backtrace:\n");
	//this->print_backtrace(address);

	try {
		throw; // re-throw
	} catch (const riscv::MachineTimeoutException &e) {
		this->handle_timeout(address);
		return; // NOTE: might wanna stay
	} catch (const riscv::MachineException &e) {
		const String instr(machine().cpu.current_instruction_to_string().c_str());
		const String regs(machine().cpu.registers().to_string().c_str());

		UtilityFunctions::print(
				"\nException: ", e.what(), "  (data: ", String("%x").format(e.data()), ")\n",
				">>> ", instr, "\n",
				">>> Machine registers:\n[PC\t", String("%x").format(machine().cpu.pc()),
				"] ", regs, "\n");
	} catch (const std::exception &e) {
		UtilityFunctions::print("\nMessage: ", e.what(), "\n\n");
		ERR_PRINT(("Exception: " + std::string(e.what())).c_str());
	}
	UtilityFunctions::print(
			"Program page: ", machine().memory.get_page_info(machine().cpu.pc()).c_str(),
			"\n");
	UtilityFunctions::print(
			"Stack page: ", machine().memory.get_page_info(machine().cpu.reg(2)).c_str(),
			"\n");
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
