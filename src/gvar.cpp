#include "riscv.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

Variant GuestVariant::toVariant(const machine_t& machine) const
{
	switch (type) {
	case Variant::BOOL:
		return v.b;
	case Variant::INT:
		return v.i;
	case Variant::FLOAT:
		return v.f;
	case Variant::STRING: {
		auto s = machine.memory.rvspan<GuestStdString>(v.s, 1);
		return s[0].to_godot_string(machine);
	}
	default:
		UtilityFunctions::print("GuestVariant::toVariant(): Unsupported type: ", type);
		return Variant();
	}

}

Variant RiscvEmulator::GetVariant(const machine_t& machine, gaddr_t address)
{
	auto v = machine.memory.rvspan<GuestVariant>(address, 1);
	return v[0].toVariant(machine);
}

void RiscvEmulator::SetVariant(machine_t& machine, gaddr_t address, Variant value)
{
	auto v = machine.memory.rvspan<GuestVariant>(address, 1);

	switch (value.get_type()) {
	case Variant::BOOL:
		v[0].type = Variant::BOOL;
		v[0].v.b = value;
		break;
	case Variant::INT:
		v[0].type = Variant::INT;
		v[0].v.i = value;
		break;
	case Variant::FLOAT:
		v[0].type = Variant::FLOAT;
		v[0].v.f = value;
		break;
	default:
		UtilityFunctions::print("SetVariant(): Unsupported type: ", value.get_type());
		break;
	}
}
