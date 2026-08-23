// DebugLayout shadow stack on a real libriscv machine. Outer frame lines
// resolved from return addresses via the line table; nothing per statement.
#include "../compiler.h"
#include "../debug_layout.h"
#include "../line_table.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <libriscv/machine.hpp>

using namespace gdscript;
using machine_t = riscv::Machine<riscv::RISCV64>;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
	if (!condition) {
		std::cerr << "FAILED: " << what << std::endl;
		failures++;
	}
}

template <typename T>
void check_eq(T actual, T expected, const std::string& what) {
	if (actual != expected) {
		std::cerr << "FAILED: " << what << ": expected " << expected
			<< ", got " << actual << std::endl;
		failures++;
	}
}

struct Program {
	std::vector<uint8_t> elf;
	LineTable lines;
	std::vector<FunctionSignature> signatures;
};

Program compile(const std::string& source, bool debug_info) {
	Program out;
	Compiler compiler;
	CompilerOptions options;
	options.debug_info = debug_info;
	out.elf = compiler.compile(source, options);
	if (out.elf.empty()) {
		std::cerr << "FAILED to compile: " << compiler.get_error() << std::endl;
		failures++;
		return out;
	}
	out.lines = compiler.get_line_table();
	out.signatures = compiler.get_function_signatures();
	return out;
}

// -= Reading the area back out of a machine =-

struct Frame {
	uint64_t function_index = 0;
	uint64_t return_address = 0;
	uint64_t frame_sp = 0;
};

struct Area {
	uint64_t address = 0;
	uint32_t magic = 0;
	uint32_t version = 0;
	uint32_t function_count = 0;
	uint32_t frame_size = 0;
	uint32_t max_depth = 0;
	uint64_t depth = 0;
	std::vector<Frame> frames;
};

template <typename T>
T read(machine_t& machine, uint64_t address) {
	T value {};
	machine.copy_from_guest(&value, address, sizeof(T));
	return value;
}

Area read_area(machine_t& machine) {
	Area area;
	area.address = machine.address_of(DEBUG_SYMBOL);
	if (area.address == 0) {
		return area;
	}
	const uint64_t base = area.address;
	area.magic = read<uint32_t>(machine, base + DebugLayout::MAGIC_OFF);
	area.version = read<uint32_t>(machine, base + DebugLayout::VERSION_OFF);
	area.function_count = read<uint32_t>(machine, base + DebugLayout::FUNCTION_COUNT_OFF);
	area.frame_size = read<uint32_t>(machine, base + DebugLayout::FRAME_SIZE_OFF);
	area.max_depth = read<uint32_t>(machine, base + DebugLayout::MAX_DEPTH_OFF);
	area.depth = read<uint64_t>(machine, base + DebugLayout::DEPTH_OFF);

	// Frames past MAX_DEPTH are not written; depth still counts.
	const uint64_t recorded = area.depth < DebugLayout::MAX_DEPTH
		? area.depth : uint64_t(DebugLayout::MAX_DEPTH);
	for (uint64_t i = 0; i < recorded; i++) {
		const uint64_t frame = base + DebugLayout::frame_offset(uint32_t(i));
		Frame entry;
		entry.function_index = read<uint64_t>(machine, frame + DebugLayout::FUNCTION_INDEX_OFF);
		entry.return_address = read<uint64_t>(machine, frame + DebugLayout::RETURN_ADDRESS_OFF);
		entry.frame_sp = read<uint64_t>(machine, frame + DebugLayout::FRAME_SP_OFF);
		area.frames.push_back(entry);
	}
	return area;
}

// Reconstructed stack entry: name + line, innermost last.
struct StackEntry {
	std::string function;
	uint32_t line = 0;
};

std::vector<StackEntry> reconstruct(const Area& area, const Program& program, uint64_t pc) {
	std::vector<StackEntry> stack;
	for (size_t i = 0; i < area.frames.size(); i++) {
		StackEntry entry;
		const uint64_t index = area.frames[i].function_index;
		entry.function = index < program.signatures.size()
			? program.signatures[index].name : "?";
		if (i + 1 < area.frames.size()) {
			entry.line = program.lines.line_for_address(
				uint32_t(area.frames[i + 1].return_address - 4));
		} else {
			entry.line = program.lines.line_for_address(uint32_t(pc));
		}
		stack.push_back(entry);
	}
	return stack;
}

// -= Running =-

// Captured at the print() syscall (interrupts without needing a host answer).
Area captured;
uint64_t captured_pc = 0;
bool capture_armed = false;

void capture_syscall(machine_t& machine) {
	if (capture_armed) {
		captured_pc = machine.cpu.pc();
		captured = read_area(machine);
		capture_armed = false;
	}
}

void fail_on_syscall(machine_t& machine) {
	std::cerr << "FAILED: the test program made syscall "
		<< machine.cpu.reg(riscv::REG_ARG7) << std::endl;
	failures++;
	machine.stop();
}

std::unique_ptr<machine_t> boot(const std::vector<uint8_t>& elf) {
	auto machine = std::make_unique<machine_t>(elf, riscv::MachineOptions<riscv::RISCV64> {
		.memory_max = 32ull << 20,
		.stack_size = 4ull << 20,
	});
	for (int number = 500; number < 560; number++) {
		machine_t::install_syscall_handler(number, fail_on_syscall);
	}
	machine_t::install_syscall_handler(500, capture_syscall); // ECALL_PRINT
	machine->simulate(50'000'000ull);
	return machine;
}

// Sandbox ABI: a0 = return Variant pointer.
bool run(machine_t& machine, const std::string& function) {
	const uint64_t address = machine.address_of(function);
	if (address == 0) {
		std::cerr << "FAILED: no symbol for " << function << "()" << std::endl;
		failures++;
		return false;
	}
	auto& sp = machine.cpu.reg(riscv::REG_SP);
	sp = machine.memory.stack_initial();
	sp -= 64;
	machine.cpu.reg(riscv::REG_RA) = machine.memory.exit_address();
	machine.cpu.reg(riscv::REG_ARG0) = sp;
	machine.cpu.jump(address);
	machine.simulate(200'000'000ull);
	return true;
}

// -= The tests =-

void test_off_by_default() {
	const std::string source =
		"func leaf():\n"
		"\treturn 1\n"
		"func test():\n"
		"\treturn leaf()\n";

	CompilerOptions defaults;
	check(!defaults.debug_info, "debug info defaults to off");

	const Program plain = compile(source, false);
	const Program debug = compile(source, true);
	if (plain.elf.empty() || debug.elf.empty()) {
		return;
	}

	auto machine = boot(plain.elf);
	check_eq(machine->address_of(DEBUG_SYMBOL), uint64_t(0),
		"an ordinary build exports no debug symbol");

	auto debug_machine = boot(debug.elf);
	check(debug_machine->address_of(DEBUG_SYMBOL) != 0,
		"a debug build exports the debug symbol");

	check(debug.elf.size() > plain.elf.size(),
		"the shadow stack is code an ordinary build does not carry");

	// Line table is metadata; produced regardless of debug_info.
	check(!plain.lines.entries.empty(), "an ordinary build still has a line table");
	check(plain.lines.is_normalized(), "and it is ordered");
}

void test_header() {
	const std::string source =
		"func a():\n"
		"\treturn 1\n"
		"func b():\n"
		"\treturn 2\n"
		"func test():\n"
		"\treturn 3\n";

	const Program program = compile(source, true);
	if (program.elf.empty()) {
		return;
	}
	auto machine = boot(program.elf);
	const Area area = read_area(*machine);

	check(area.address != 0, "the debug area is addressable");
	check_eq(area.magic, DebugLayout::MAGIC, "magic");
	check_eq(area.version, DebugLayout::LAYOUT_VERSION, "version");
	check_eq(area.function_count, uint32_t(3), "function count");
	check_eq(area.frame_size, uint32_t(DebugLayout::FRAME_SIZE), "frame size");
	check_eq(area.max_depth, DebugLayout::MAX_DEPTH, "max depth");
	check_eq(area.depth, uint64_t(0), "nothing is on the stack before the first call");
}

void test_call_stack_mid_call() {
	const std::string source =
		"func c():\n" // 1
		"\tprint(1)\n" // 2
		"\treturn 3\n" // 3
		"func b():\n" // 4
		"\treturn c()\n" // 5
		"func a():\n" // 6
		"\treturn b()\n" // 7
		"func test():\n" // 8
		"\treturn a()\n"; // 9

	const Program program = compile(source, true);
	if (program.elf.empty()) {
		return;
	}
	auto machine = boot(program.elf);

	captured = Area{};
	captured_pc = 0;
	capture_armed = true;
	run(*machine, "test");
	check(!capture_armed, "the run reached print()");
	if (capture_armed) {
		return;
	}

	check_eq(captured.depth, uint64_t(4), "four frames are live inside c()");
	const std::vector<StackEntry> stack = reconstruct(captured, program, captured_pc);
	if (stack.size() != 4) {
		check(false, "four frames were recorded");
		return;
	}

	check_eq(stack[0].function, std::string("test"), "outermost frame");
	check_eq(stack[1].function, std::string("a"), "second frame");
	check_eq(stack[2].function, std::string("b"), "third frame");
	check_eq(stack[3].function, std::string("c"), "innermost frame");

	check_eq(stack[0].line, uint32_t(9), "test() is sitting in its call to a()");
	check_eq(stack[1].line, uint32_t(7), "a() is sitting in its call to b()");
	check_eq(stack[2].line, uint32_t(5), "b() is sitting in its call to c()");
	check_eq(stack[3].line, uint32_t(2), "c() is on the line it called print() from");

	// Stack grows down: inner frame sp <= outer frame sp.
	for (size_t i = 1; i < captured.frames.size(); i++) {
		check(captured.frames[i].frame_sp <= captured.frames[i - 1].frame_sp,
			"frame " + std::to_string(i) + " sits below its caller");
	}
}

void test_depth_balances() {
	const std::string source =
		"func leaf(n):\n"
		"\treturn n + 1\n"
		"func test():\n"
		"\tvar total = 0\n"
		"\tfor i in range(10):\n"
		"\t\ttotal = total + leaf(i)\n"
		"\treturn total\n";

	const Program program = compile(source, true);
	if (program.elf.empty()) {
		return;
	}
	auto machine = boot(program.elf);
	run(*machine, "test");

	const Area area = read_area(*machine);
	check_eq(area.depth, uint64_t(0), "every call that returned was popped");
}

void test_overflow_keeps_counting() {
	// Past MAX_DEPTH: frames stop recording but depth balances to zero.
	const std::string source =
		"func down(n):\n"
		"\tif n <= 0:\n"
		"\t\treturn 0\n"
		"\treturn down(n - 1)\n"
		"func test():\n"
		"\treturn down(400)\n";

	const Program program = compile(source, true);
	if (program.elf.empty()) {
		return;
	}
	auto machine = boot(program.elf);
	run(*machine, "test");

	const Area area = read_area(*machine);
	check_eq(area.depth, uint64_t(0), "a run past MAX_DEPTH still balances");
}

} // namespace

int main() {
	test_off_by_default();
	test_header();
	test_call_stack_mid_call();
	test_depth_balances();
	test_overflow_keeps_counting();

	if (failures > 0) {
		std::cerr << failures << " debug info test(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All debug info tests passed" << std::endl;
	return 0;
}
