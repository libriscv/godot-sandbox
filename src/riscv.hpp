#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/viewport.hpp>

#include <godot_cpp/core/binder_common.hpp>
#include <libriscv/machine.hpp>

using namespace godot;
#define RISCV_ARCH riscv::RISCV64
using gaddr_t = riscv::address_type<RISCV_ARCH>;
using machine_t = riscv::Machine<RISCV_ARCH>;

class RiscvEmulator : public Control
{
	GDCLASS(RiscvEmulator, Control);

protected:
	static void _bind_methods();

	String _to_string() const;

public:
	static constexpr uint64_t MAX_INSTRUCTIONS = 16'000'000'000ULL;

	RiscvEmulator();
	~RiscvEmulator();

	auto& machine() { return *m_machine; }
	const auto& machine() const { return *m_machine; }

	// Functions.
	void load(const PackedByteArray& buffer, const TypedArray<String>& arguments);
	Variant vmcall(String function,
		const Array& args = Array());

	void print(std::string_view text);
	gaddr_t address_of(std::string_view name) const;

private:
	void execute();
	void handle_exception(gaddr_t);
	void handle_timeout(gaddr_t);
	void initialize_syscalls();

	machine_t* m_machine = nullptr;
	std::vector<uint8_t> m_binary;

	Dictionary m_lookup;

	bool m_last_newline = false;
	unsigned m_budget_overruns = 0;
};
