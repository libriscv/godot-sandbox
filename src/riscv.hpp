#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/viewport.hpp>

#include <godot_cpp/core/binder_common.hpp>
#include <libriscv/machine.hpp>

using namespace godot;

class RiscvEmulator : public Control
{
	GDCLASS(RiscvEmulator, Control);

protected:
	static void _bind_methods();

	String _to_string() const;

public:
	static constexpr int MARCH = 4;
	static constexpr uint64_t MAX_INSTRUCTIONS = 16'000'000'000ULL;
	using gaddr_t = riscv::address_type<MARCH>;
	using machine_t = riscv::Machine<MARCH>;

private:
	machine_t* m_machine = nullptr;
	std::vector<uint8_t> m_binary;

public:
	RiscvEmulator();
	~RiscvEmulator();

	auto& machine() {
		if (m_machine != nullptr)
			return *m_machine;
		else
			throw riscv::MachineException(riscv::INVALID_PROGRAM, "Machine not initialized");
	}
	const auto& machine() const {
		if (m_machine != nullptr)
			return *m_machine;
		else
			throw riscv::MachineException(riscv::INVALID_PROGRAM, "Machine not initialized");
	}

	// Functions.
	void load(const PackedByteArray& buffer, const TypedArray<String>& arguments);
	void exec();
	void fork_exec();
};
