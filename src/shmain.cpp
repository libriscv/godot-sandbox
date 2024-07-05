#include "riscv.hpp"

#include <godot_cpp/core/class_db.hpp>

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// There are two APIs:
// 1. The API that always makes sense, eg. Timers, Nodes, etc.
//    This will be using system calls. Assign a fixed-number.
// 2. The Game-specific API, eg. whatever bullshit you need for your game to be fucking awesome.
//    (will be implemented later)

String RiscvEmulator::_to_string() const
{
	return "[ GDExtension::RiscvEmulator <--> Instance ID:" + uitos(get_instance_id()) + " ]";
}

void RiscvEmulator::_bind_methods()
{
	// Methods.
	ClassDB::bind_method(D_METHOD("load"), &RiscvEmulator::load);
	ClassDB::bind_method(D_METHOD("vmcall"), &RiscvEmulator::vmcall);
}

RiscvEmulator::RiscvEmulator()
{
	// In order to reduce checks we guarantee that this
	// class is well-formed at all times.
	this->m_machine = new machine_t { };
	set_name("(name)");
	UtilityFunctions::print("Constructor.");
}

RiscvEmulator::~RiscvEmulator()
{
	UtilityFunctions::print("Destructor.");
	delete this->m_machine;
}

// Methods.
void RiscvEmulator::load(const PackedByteArray& buffer, const TypedArray<String>& arguments)
{
	UtilityFunctions::print("Loading file from buffer");

	m_binary = std::vector<uint8_t> {buffer.ptr(), buffer.ptr() + buffer.size()};

	try {
		delete this->m_machine;
		this->m_machine = new machine_t { this->m_binary };
		machine_t& m = machine();

		m.set_userdata(this);

		this->initialize_syscalls();

		m.setup_linux({"program"});

		m.simulate(MAX_INSTRUCTIONS);
	} catch (const std::exception& e) {
		UtilityFunctions::print("Exception: ", e.what());
	}
}

Variant RiscvEmulator::vmcall(String function, const Array& args)
{
	const auto ascii = function.ascii();
	const std::string_view sview {ascii.get_data(), (size_t)ascii.length()};
	gaddr_t address = 0x0;

	// reset the stack pointer to an initial location (deliberately)
	m_machine->cpu.reset_stack_pointer();
	// setup calling convention
	m_machine->setup_call();

	int iarg = 0;
	int farg = 0;

	for (size_t i = 0; i < args.size(); i++)
	{
		auto& arg = args[i];
		if (arg.get_type() == Variant::Type::STRING)
		{
			const auto ascii = arg.operator String().ascii();
			const std::string_view sview {ascii.get_data(), (size_t)ascii.length()};
			// XXX: We need to zero-terminate this string
			auto addr = machine().stack_push(sview.data(), sview.size());

			m_machine->cpu.reg(10 + iarg) = addr;
			iarg++;
			m_machine->cpu.reg(10 + iarg) = sview.size();
			iarg++;
		}
		else if (arg.get_type() == Variant::Type::INT)
		{
			m_machine->cpu.reg(10 + iarg) = arg.operator int64_t();
			iarg++;
		}
		else if (arg.get_type() == Variant::Type::FLOAT)
		{
			m_machine->cpu.registers().getfl(10 + farg).set_double(arg.operator double());
			farg++;
		}
		else
		{
			UtilityFunctions::print("Unsupported argument type: ", arg.get_type());
		}
	}

	try
	{
		// lookup function address
		gaddr_t address = 0x0;
		auto res = m_lookup.find_key(function);
		if (res.get_type() == Variant::Type::NIL)
		{
			address = machine().address_of(sview);
			m_lookup[function] = address;
		}
		else
		{
			address = res.operator int64_t();
		}

		// execute guest function
		m_machine->simulate_with<true>(MAX_INSTRUCTIONS, 0u, address);

		return m_machine->return_value<int64_t>();
	}
	catch (const std::exception& e)
	{
		this->handle_exception(address);
	}
	return -1;
}

void RiscvEmulator::execute()
{
	machine_t& m = machine();

	UtilityFunctions::print("Simulating...");
	m.simulate(MAX_INSTRUCTIONS);
	UtilityFunctions::print("Done, instructions: ", m.instruction_counter(),
		" result: ", m.return_value<int64_t>());
}

void RiscvEmulator::handle_exception(gaddr_t address)
{
	auto callsite = machine().memory.lookup(address);
	UtilityFunctions::print(
		"[", get_name(), "] Exception when calling:\n  ", callsite.name.c_str(), " (0x",
		String("%x").format(callsite.address), ")\n", "Backtrace:\n");
	//this->print_backtrace(address);

	try
	{
		throw; // re-throw
	}
	catch (const riscv::MachineTimeoutException& e)
	{
		this->handle_timeout(address);
		return; // NOTE: might wanna stay
	}
	catch (const riscv::MachineException& e)
	{
		const String instr (machine().cpu.current_instruction_to_string().c_str());
		const String regs  (machine().cpu.registers().to_string().c_str());

		UtilityFunctions::print(
			"\nException: ", e.what(), "  (data: ", String("%x").format(e.data()), ")\n",
			">>> ", instr, "\n",
			">>> Machine registers:\n[PC\t", String("%x").format(machine().cpu.pc()),
			"] ", regs, "\n");
	}
	catch (const std::exception& e)
	{
		UtilityFunctions::print("\nMessage: ", e.what(), "\n\n");
	}
	UtilityFunctions::print(
		"Program page: ", machine().memory.get_page_info(machine().cpu.pc()).c_str(),
		"\n");
	UtilityFunctions::print(
		"Stack page: ", machine().memory.get_page_info(machine().cpu.reg(2)).c_str(),
		"\n");
}

void RiscvEmulator::handle_timeout(gaddr_t address)
{
	this->m_budget_overruns++;
	auto callsite = machine().memory.lookup(address);
	UtilityFunctions::print(
		"RiscvEmulator: Timeout for '", callsite.name.c_str(),
		"' (Timeouts: ", m_budget_overruns, "\n");
}

void RiscvEmulator::print(std::string_view text)
{
	String str(static_cast<std::string> (text).c_str());
	if (this->m_last_newline) {
		UtilityFunctions::print("[", get_name(), "] says: ", str);
	}
	else {
		UtilityFunctions::print(str);
	}
	this->m_last_newline = (text.back() == '\n');
}

gaddr_t RiscvEmulator::address_of(std::string_view name) const
{
	return machine().address_of(name);
}
