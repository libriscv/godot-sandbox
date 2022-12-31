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

	delete this->m_machine;

	m_binary = std::vector<uint8_t> {buffer.ptr(), buffer.ptr() + buffer.size()};

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
