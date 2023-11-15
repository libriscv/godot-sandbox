#include "riscv.hpp"

#include <godot_cpp/core/class_db.hpp>

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

String RiscvEmulator::_to_string() const
{
	return "[ GDExtension::RiscvEmulator <--> Instance ID:" + uitos(get_instance_id()) + " ]";
}

void RiscvEmulator::_bind_methods()
{
	// Methods.
	ClassDB::bind_method(D_METHOD("load"), &RiscvEmulator::load);
	ClassDB::bind_method(D_METHOD("exec"), &RiscvEmulator::exec);
	ClassDB::bind_method(D_METHOD("fork_exec"), &RiscvEmulator::fork_exec);
}

RiscvEmulator::RiscvEmulator()
{
	// In order to reduce checks we guarantee that this
	// class is well-formed at all times.
	this->m_machine = new machine_t { };
	this->m_name = "(name)";
	UtilityFunctions::print("Constructor.");
}

RiscvEmulator::~RiscvEmulator()
{
	UtilityFunctions::print("Destructor.");
	delete this->m_machine;
}

// Methods.
const String& RiscvEmulator::name()
{
	return this->m_name;
}

void RiscvEmulator::load(const PackedByteArray& buffer, const TypedArray<String>& arguments)
{
	UtilityFunctions::print("Loading file from buffer");

	m_binary = std::vector<uint8_t> {buffer.ptr(), buffer.ptr() + buffer.size()};

	delete this->m_machine;
	this->m_machine = new machine_t { this->m_binary };
	machine_t& m = machine();

	m.setup_minimal_syscalls();
	m.setup_argv({"program"});
}
void RiscvEmulator::exec()
{
	machine_t& m = machine();

	UtilityFunctions::print("Simulating...");
	m.simulate(MAX_INSTRUCTIONS);
	UtilityFunctions::print("Done, instructions: ", m.instruction_counter(),
		" result: ", m.return_value<int64_t>());
}
void RiscvEmulator::fork_exec()
{

}

GDExtensionInt RiscvEmulator::call(String function)
{
	const auto ascii = function.ascii();
	const std::string_view sview {ascii.get_data(), (size_t)ascii.length()};
	gaddr_t address = 0x0;

	machine().reset_instruction_counter();
	try
	{
		address = machine().address_of(sview);
		return machine().vmcall<MAX_INSTRUCTIONS>(address);
	}
	catch (const std::exception& e)
	{
		this->handle_exception(address);
	}
	return -1;
}

void RiscvEmulator::handle_exception(gaddr_t address)
{
	auto callsite = machine().memory.lookup(address);
	UtilityFunctions::print(
		"[", name(), "] Exception when calling:\n  ", callsite.name.c_str(), " (0x",
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
		UtilityFunctions::print("[", name(), "] says: ", str);
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
