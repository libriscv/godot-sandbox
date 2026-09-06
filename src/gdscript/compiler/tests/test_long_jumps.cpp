#include "../elf_builder.h"
#include "../riscv_codegen.h"
#include <libriscv/machine.hpp>
#include <cassert>
#include <cstring>
#include <iostream>

using namespace gdscript;
using machine_t = riscv::Machine<riscv::RISCV64>;

static IRInstruction load(int reg, int64_t value) {
	IRInstruction instr(IROpcode::LOAD_IMM, IRValue::reg(reg), IRValue::imm(value));
	instr.type_hint = Variant::INT;
	return instr;
}

static void pad(IRFunction& fn) {
	fn.instructions.insert(fn.instructions.end(), 140000, load(1, 0x12345678));
}

static void return_value(IRFunction& fn, int64_t value) {
	fn.instructions.push_back(load(0, value));
	fn.instructions.emplace_back(IROpcode::RETURN);
}

static IRProgram program_with_long_jumps() {
	IRProgram program;
	auto label = [&](const char* name) { return IRValue::label(program.strings.intern(name)); };
	auto function = [](const char* name, bool parameter = false) {
		IRFunction fn;
		fn.name = name;
		fn.max_registers = 2;
		if (parameter) {
			fn.parameters.push_back("value");
		}
		return fn;
	};
	auto branch = [&](IROpcode opcode, const char* target) {
		IRInstruction instr(opcode, IRValue::reg(0), label(target));
		instr.type_hint = Variant::INT;
		return instr;
	};

	auto early = function("early");
	return_value(early, 11);
	program.functions.push_back(std::move(early));

	auto forward = function("forward");
	forward.instructions.emplace_back(IROpcode::CALL, IRValue::str(program.strings.intern("late")),
		IRValue::reg(0), IRValue::imm(0));
	forward.instructions.emplace_back(IROpcode::RETURN);
	program.functions.push_back(std::move(forward));

	auto flow = function("flow", true);
	flow.instructions.emplace_back(IROpcode::LABEL, label("flow_start"));
	flow.instructions.push_back(branch(IROpcode::BRANCH_NOT_ZERO, "flow_end"));
	return_value(flow, 17);
	pad(flow);
	flow.instructions.emplace_back(IROpcode::LABEL, label("flow_end"));
	flow.instructions.push_back(load(0, 0));
	flow.instructions.emplace_back(IROpcode::JUMP, label("flow_start"));
	program.functions.push_back(std::move(flow));

	auto backward = function("backward", true);
	backward.instructions.emplace_back(IROpcode::JUMP, label("backward_end"));
	backward.instructions.emplace_back(IROpcode::LABEL, label("backward_start"));
	backward.instructions.push_back(load(0, 0));
	backward.instructions.emplace_back(IROpcode::JUMP, label("backward_end"));
	pad(backward);
	backward.instructions.emplace_back(IROpcode::LABEL, label("backward_end"));
	backward.instructions.push_back(branch(IROpcode::BRANCH_NOT_ZERO, "backward_start"));
	return_value(backward, 19);
	program.functions.push_back(std::move(backward));

	auto table = function("table", true);
	IRInstruction dispatch(IROpcode::SWITCH, IRValue::reg(0), IRValue::imm(0), IRValue::imm(2));
	dispatch.type_hint = Variant::INT;
	dispatch.operands.push_back(label("table_near"));
	dispatch.operands.push_back(label("table_far"));
	table.instructions.push_back(std::move(dispatch));
	return_value(table, 23);
	table.instructions.emplace_back(IROpcode::LABEL, label("table_near"));
	return_value(table, 29);
	pad(table);
	table.instructions.emplace_back(IROpcode::LABEL, label("table_far"));
	return_value(table, 31);
	program.functions.push_back(std::move(table));

	auto late = function("late");
	late.instructions.emplace_back(IROpcode::CALL, IRValue::str(program.strings.intern("early")),
		IRValue::reg(0), IRValue::imm(0));
	late.instructions.emplace_back(IROpcode::RETURN);
	program.functions.push_back(std::move(late));
	return program;
}

static int64_t call(machine_t& machine, const char* name, int64_t argument = 0) {
	auto& sp = machine.cpu.reg(riscv::REG_SP);
	sp = machine.memory.stack_initial() - 128;
	const uint64_t result = sp;
	const uint64_t parameter = sp + 64;
	const uint32_t type = Variant::INT;
	machine.copy_to_guest(parameter, &type, sizeof(type));
	machine.copy_to_guest(parameter + 8, &argument, sizeof(argument));
	machine.cpu.reg(riscv::REG_ARG0) = result;
	machine.cpu.reg(riscv::REG_ARG1) = parameter;
	machine.cpu.reg(riscv::REG_RA) = machine.address_of(FAST_EXIT_SYMBOL);
	machine.cpu.jump(machine.address_of(name));
	machine.simulate(1000000ull);
	uint32_t result_type;
	int64_t value;
	machine.copy_from_guest(&result_type, result, sizeof(result_type));
	machine.copy_from_guest(&value, result + 8, sizeof(value));
	if (result_type != Variant::INT) {
		std::cerr << name << " returned type " << result_type << " and payload " << value << '\n';
	}
	assert(result_type == Variant::INT);
	return value;
}

static uint32_t word_at(const std::vector<uint8_t>& code, size_t offset) {
	uint32_t word;
	std::memcpy(&word, code.data() + offset, sizeof(word));
	return word;
}

int main() {
	const auto program = program_with_long_jumps();
	RISCVCodeGen codegen;
	const auto code = codegen.generate(program);
	assert(codegen.get_function_offsets().at("late") - codegen.get_function_offsets().at("forward") > (1u << 20));
	size_t forward_calls = 0, backward_calls = 0, forward_jumps = 0, backward_jumps = 0;
	for (size_t pc = 0; pc + 8 <= code.size(); pc += 4) {
		const uint32_t upper = word_at(code, pc);
		const uint32_t lower = word_at(code, pc + 4);
		if ((upper & 0x7F) != 0x17 || (lower & 0x7F) != 0x67) {
			continue;
		}
		assert(((upper >> 7) & 31) == RISCVCodeGen::REG_WIDE_SCRATCH);
		assert(((lower >> 15) & 31) == RISCVCodeGen::REG_WIDE_SCRATCH);
		const int64_t displacement = int64_t(int32_t(upper & 0xFFFFF000)) + (int32_t(lower) >> 20);
		const bool is_call = ((lower >> 7) & 31) != 0;
		if (displacement >= (1 << 20)) {
			(is_call ? forward_calls : forward_jumps)++;
		} else if (displacement < -(1 << 20)) {
			(is_call ? backward_calls : backward_jumps)++;
		}
	}
	assert(forward_calls > 0 && backward_calls > 0 && forward_jumps > 0 && backward_jumps > 0);

	ElfBuilder builder;
	const auto elf = builder.build(program);
	machine_t machine(elf, riscv::MachineOptions<riscv::RISCV64>{.memory_max = 64ull << 20});
	machine.simulate(1000000ull);
	assert(call(machine, "forward") == 11);
	assert(call(machine, "late") == 11);
	assert(call(machine, "flow", 0) == 17);
	assert(call(machine, "flow", 1) == 17);
	assert(call(machine, "backward", 0) == 19);
	assert(call(machine, "backward", 1) == 19);
	assert(call(machine, "table", -1) == 23);
	assert(call(machine, "table", 0) == 29);
	assert(call(machine, "table", 1) == 31);
	assert(call(machine, "table", 2) == 23);
	std::cout << "Long jumps, calls, branches and dense tables passed (" << code.size() << " bytes)\n";
}
