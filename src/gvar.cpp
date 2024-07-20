#include "riscv.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

Variant GuestVariant::toVariant(const machine_t& machine) const
{
	switch (type) {
	case Variant::NIL:
		return Variant();
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
	case Variant::CALLABLE:
		return Variant{*(Variant*)&v.opaque[0]};
	default:
		UtilityFunctions::print("GuestVariant::toVariant(): Unsupported type: ", type);
		return Variant();
	}
}

Variant* GuestVariant::toVariantPtr(const machine_t& machine) const
{
	switch (type) {
	case Variant::CALLABLE: {
		return (Variant*)&v.opaque[0];
	}
	default:
		throw std::runtime_error("Don't use toVariantPtr() on unsupported type: " + std::to_string(type));
	}
}

void GuestVariant::set(machine_t& machine, const Variant& value)
{
	switch (value.get_type()) {
	case Variant::NIL:
		this->type = Variant::NIL;
		break;
	case Variant::BOOL:
		this->type = Variant::BOOL;
		this->v.b = value;
		break;
	case Variant::INT:
		this->type = Variant::INT;
		this->v.i = value;
		break;
	case Variant::FLOAT:
		this->type = Variant::FLOAT;
		this->v.f = value;
		break;
	case Variant::STRING: {
		this->type = Variant::STRING;
		auto s = value.operator String();
		auto str = s.utf8();
		// Allocate memory for the GuestStdString (which is a std::string)
		// TODO: Improve this by allocating string + contents + null terminator in one go
		auto ptr = machine.arena().malloc(sizeof(GuestStdString));
		auto gstr = machine.memory.rvspan<GuestStdString>(ptr, 1);
		gstr[0].set_string(machine, ptr, str.get_data(), str.length());
		this->v.s = ptr;
		break;
	}
	case Variant::CALLABLE: {
		this->type = Variant::CALLABLE;
		std::memcpy(this->v.opaque, &value, sizeof(Variant));
		break;
	}
	default:
		UtilityFunctions::print("SetVariant(): Unsupported type: ", value.get_type());
		break;
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

	v[0].set(machine, value);
}
