// Deterministic code-quality guardrail for the interpreter hot path. Wall-clock
// benchmarks are machine-dependent; the libriscv instruction counter is not.
#include "../compiler.h"
#include "../variant_layout.h"
#include "../variant_types.h"
#include "scope_stub.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <libriscv/machine.hpp>

using namespace gdscript;
using Machine = riscv::Machine<riscv::RISCV64>;

namespace {

const char* SOURCE = R"(
func loop_int(n : int) -> int:
	var acc : int = 0
	var i : int = 0
	while i < n:
		acc += i * 3 - (i >> 2)
		i += 1
	return acc

func loop_float(n : int) -> float:
	var acc : float = 0.0
	var i : int = 0
	while i < n:
		acc += float(i) * 0.5 - 0.25
		i += 1
	return acc

func _number(pick : int):
	if pick == 0:
		return 1.5
	return 3

func untyped_float(n : int) -> float:
	var acc = _number(0)
	var step = _number(0)
	var i : int = 0
	while i < n:
		acc = acc + step
		i += 1
	return acc

func untyped_float_compare(n : int) -> int:
	var a = _number(0)
	var b = _number(1)
	var acc : int = 0
	var i : int = 0
	while i < n:
		if a < b:
			acc += 1
		i += 1
	return acc
)";

void write_variant(Machine& machine, uint64_t address, const VariantLayout& layout,
	int32_t type, int64_t payload)
{
	std::vector<uint8_t> bytes(size_t(layout.variant_size()), 0);
	std::memcpy(bytes.data() + VariantLayout::TYPE_OFFSET, &type, sizeof(type));
	std::memcpy(bytes.data() + VariantLayout::DATA_OFFSET, &payload, sizeof(payload));
	machine.copy_to_guest(address, bytes.data(), bytes.size());
}

struct RunResult {
	uint64_t instructions = 0;
	int32_t type = Variant::NIL;
	uint64_t payload = 0;
};

RunResult run(const std::vector<uint8_t>& elf, const std::string& function, int64_t n) {
	const VariantLayout layout = native_variant_layout();
	Machine machine { elf, riscv::MachineOptions<riscv::RISCV64> {
		.memory_max = 16ull << 20,
		.stack_size = 1ull << 20,
	} };
	install_scope_stub<Machine>();
	// ELF entry initializes globals and exits. These kernels have none.
	machine.simulate(50'000'000ull);
	machine.set_instruction_counter(0);
	machine.set_max_instructions(UINT64_MAX);

	uint64_t& sp = machine.cpu.reg(riscv::REG_SP);
	sp = machine.memory.stack_initial();
	const uint64_t stride = (uint64_t(layout.variant_size()) + 15u) & ~15u;
	sp -= stride * 2;
	const uint64_t result = sp;
	const uint64_t argument = sp + stride;
	write_variant(machine, result, layout, Variant::NIL, 0);
	write_variant(machine, argument, layout, Variant::INT, n);

	machine.cpu.reg(riscv::REG_RA) = machine.memory.exit_address();
	machine.cpu.reg(riscv::REG_ARG0) = result;
	machine.cpu.reg(riscv::REG_ARG1) = argument;
	machine.cpu.jump(machine.address_of(function));
	machine.simulate(100'000'000ull);
	RunResult out;
	out.instructions = machine.instruction_counter();
	machine.copy_from_guest(&out.type, result + VariantLayout::TYPE_OFFSET, sizeof(out.type));
	machine.copy_from_guest(&out.payload, result + VariantLayout::DATA_OFFSET, sizeof(out.payload));
	return out;
}

void check_budget(const std::vector<uint8_t>& elf, const std::string& function,
	uint64_t budget)
{
	constexpr int64_t SMALL = 200;
	constexpr int64_t LARGE = 1200;
	const RunResult small = run(elf, function, SMALL);
	const RunResult large = run(elf, function, LARGE);
	const uint64_t per_iteration = (large.instructions - small.instructions) /
		uint64_t(LARGE - SMALL);
	if (per_iteration > budget) {
		std::cerr << function << " executes " << per_iteration
			<< " instructions/iteration; budget is " << budget << std::endl;
		std::exit(1);
	}
	std::cout << function << ": " << per_iteration << " instructions/iteration" << std::endl;
	if (function == "untyped_float") {
		double value = 0.0;
		std::memcpy(&value, &large.payload, sizeof(value));
		if (large.type != Variant::FLOAT || value != 1.5 * double(LARGE + 1)) {
			std::cerr << function << " returned " << value << " instead of "
				<< 1.5 * double(LARGE + 1) << std::endl;
			std::exit(1);
		}
	} else if (function == "untyped_float_compare") {
		if (large.type != Variant::INT || int64_t(large.payload) != LARGE) {
			std::cerr << function << " returned the wrong comparison count" << std::endl;
			std::exit(1);
		}
	}
}

} // namespace

int main() {
	Compiler compiler;
	const std::vector<uint8_t> elf = compiler.compile(SOURCE);
	if (elf.empty()) {
		std::cerr << compiler.get_error() << std::endl;
		return 1;
	}
	check_budget(elf, "loop_int", 12);
	check_budget(elf, "loop_float", 14);
	check_budget(elf, "untyped_float", 14);
	check_budget(elf, "untyped_float_compare", 20);
	return 0;
}
