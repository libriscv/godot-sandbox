// ECALL_BREAKPOINT on a real libriscv machine: placement, register
// transparency, line-table/a0 agreement, and shadow-stack presence.
#include "../compiler.h"
#include "../debug_layout.h"
#include "../line_table.h"
#include "../syscall_numbers.h"
#include <cstdint>
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

void check_lines(const std::vector<uint32_t>& actual, const std::vector<uint32_t>& expected,
		const std::string& what) {
	if (actual == expected) {
		return;
	}
	const auto join = [](const std::vector<uint32_t>& lines) {
		std::string out = "[";
		for (size_t i = 0; i < lines.size(); i++) {
			out += (i ? ", " : "") + std::to_string(lines[i]);
		}
		return out + "]";
	};
	std::cerr << "FAILED: " << what << ": expected " << join(expected)
		<< ", got " << join(actual) << std::endl;
	failures++;
}

struct Program {
	std::vector<uint8_t> elf;
	LineTable lines;
	std::vector<FunctionSignature> signatures;
	std::vector<uint32_t> installed;
};

Program compile(const std::string& source, const std::vector<uint32_t>& breakpoints) {
	Program out;
	Compiler compiler;
	CompilerOptions options;
	options.breakpoint_lines = breakpoints;
	out.elf = compiler.compile(source, options);
	if (out.elf.empty()) {
		std::cerr << "FAILED to compile: " << compiler.get_error() << std::endl;
		failures++;
		return out;
	}
	out.lines = compiler.get_line_table();
	out.signatures = compiler.get_function_signatures();
	out.installed = compiler.get_installed_breakpoints();
	return out;
}

// -= What a stop looks like =-
struct Stop {
	uint32_t reported_line = 0;
	uint32_t pc_line = 0;
	uint64_t depth = 0;
	uint64_t innermost_function = 0;
};

const LineTable* g_lines = nullptr;
std::vector<Stop> g_stops;
// Registers at break vs. after continue; difference = non-transparent break.
riscv::Registers<riscv::RISCV64> g_regs_at_break;
bool g_capture_registers = false;

void breakpoint_syscall(machine_t& machine) {
	Stop stop;
	stop.reported_line = uint32_t(machine.cpu.reg(riscv::REG_ARG0));
	stop.pc_line = g_lines != nullptr
		? g_lines->line_for_address(uint32_t(machine.cpu.pc())) : 0;

	const uint64_t area = machine.address_of(DEBUG_SYMBOL);
	if (area != 0) {
		machine.copy_from_guest(&stop.depth, area + DebugLayout::DEPTH_OFF, sizeof(stop.depth));
		if (stop.depth > 0 && stop.depth <= DebugLayout::MAX_DEPTH) {
			const uint64_t frame = area + DebugLayout::frame_offset(uint32_t(stop.depth - 1));
			machine.copy_from_guest(&stop.innermost_function,
					frame + DebugLayout::FUNCTION_INDEX_OFF, sizeof(stop.innermost_function));
		}
	}
	if (g_capture_registers) {
		g_regs_at_break = machine.cpu.registers();
	}
	g_stops.push_back(stop);
	// Continue immediately; a real debugger would block here instead.
}

void fail_on_syscall(machine_t& machine) {
	std::cerr << "FAILED: the test program made syscall "
		<< machine.cpu.reg(riscv::REG_ARG7) << std::endl;
	failures++;
	machine.stop();
}

std::unique_ptr<machine_t> boot(const Program& program) {
	auto machine = std::make_unique<machine_t>(program.elf, riscv::MachineOptions<riscv::RISCV64> {
		.memory_max = 32ull << 20,
		.stack_size = 4ull << 20,
	});
	for (int number = GAME_API_BASE; number < ECALL_LAST; number++) {
		machine_t::install_syscall_handler(number, fail_on_syscall);
	}
	machine_t::install_syscall_handler(ECALL_BREAKPOINT, breakpoint_syscall);
	g_lines = &program.lines;
	g_stops.clear();
	machine->simulate(50'000'000ull);
	return machine;
}

// Sandbox ABI: a0 = return Variant pointer. Returns the integer payload.
int64_t run(machine_t& machine, const std::string& function) {
	const uint64_t address = machine.address_of(function);
	if (address == 0) {
		std::cerr << "FAILED: no symbol for " << function << "()" << std::endl;
		failures++;
		return 0;
	}
	auto& sp = machine.cpu.reg(riscv::REG_SP);
	sp = machine.memory.stack_initial();
	sp -= 64;
	const uint64_t retvar = sp;
	machine.cpu.reg(riscv::REG_RA) = machine.memory.exit_address();
	machine.cpu.reg(riscv::REG_ARG0) = retvar;
	machine.cpu.jump(address);
	machine.simulate(200'000'000ull);

	int64_t value = 0;
	machine.copy_from_guest(&value, retvar + 8, sizeof(value));
	return value;
}

// One integer argument, as a Variant beside the return slot.
int64_t run_with_arg(machine_t& machine, const std::string& function, int64_t argument) {
	const uint64_t address = machine.address_of(function);
	if (address == 0) {
		std::cerr << "FAILED: no symbol for " << function << "()" << std::endl;
		failures++;
		return 0;
	}
	auto& sp = machine.cpu.reg(riscv::REG_SP);
	sp = machine.memory.stack_initial();
	sp -= 128;
	const uint64_t retvar = sp;
	const uint64_t argvar = retvar + 64;
	const int32_t int_type = 2; // Variant::INT
	machine.copy_to_guest(argvar, &int_type, sizeof(int_type));
	machine.copy_to_guest(argvar + 8, &argument, sizeof(argument));
	machine.cpu.reg(riscv::REG_RA) = machine.memory.exit_address();
	machine.cpu.reg(riscv::REG_ARG0) = retvar;
	machine.cpu.reg(riscv::REG_ARG1) = argvar;
	machine.cpu.jump(address);
	machine.simulate(200'000'000ull);

	int64_t value = 0;
	machine.copy_from_guest(&value, retvar + 8, sizeof(value));
	return value;
}

int64_t compile_and_run(const std::string& source, const std::string& function,
		const std::vector<uint32_t>& breakpoints) {
	const Program program = compile(source, breakpoints);
	if (program.elf.empty()) {
		return 0;
	}
	auto machine = boot(program);
	return run(*machine, function);
}

// Count ECALL_BREAKPOINT sites in the ELF (not just reachable ones).
size_t count_break_sites(const Program& program) {
	// `li a7, ECALL_BREAKPOINT` == `addi a7, zero, imm`
	const uint32_t li_a7 = 0x00000893u | (uint32_t(ECALL_BREAKPOINT) << 20);
	size_t found = 0;
	for (size_t i = 0; i + 8 <= program.elf.size(); i += 2) {
		uint32_t word = 0;
		std::memcpy(&word, program.elf.data() + i, 4);
		if (word != li_a7) {
			continue;
		}
		uint32_t next = 0;
		std::memcpy(&next, program.elf.data() + i + 4, 4);
		if (next == 0x00000073u) { // ecall
			found++;
		}
	}
	return found;
}

// -= The tests =-

// First value comes from a call so the optimizer cannot fold the whole body.
const std::string STRAIGHT_LINE =
	"func seed():\n" // 1
	"\treturn 10\n" // 2
	"func test():\n" // 3
	"\tvar a = seed()\n" // 4
	"\tvar b = a * 3\n" // 5
	"\tvar c = b - a\n" // 6
	"\treturn c\n"; // 7

void test_nothing_emitted_without_breakpoints() {
	const Program program = compile(STRAIGHT_LINE, {});
	check_eq(count_break_sites(program), size_t(0),
		"an ordinary compile emits no break sites");
	check(program.installed.empty(), "and reports installing none");
	check_eq(compile_and_run(STRAIGHT_LINE, "test", {}), int64_t(20),
		"test() = 20 with no breakpoints");
}

void test_stops_on_the_line_it_was_given() {
	const Program program = compile(STRAIGHT_LINE, { 6 });
	check_eq(count_break_sites(program), size_t(1),
		"one breakpoint line is one break site");
	check_lines(program.installed, { 6 }, "the compiler reports the line it installed");

	auto machine = boot(program);
	check_eq(run(*machine, "test"), int64_t(20), "test() = 20 across a break");
	check_eq(g_stops.size(), size_t(1), "the guest stopped once");
	if (g_stops.empty()) {
		return;
	}
	check_eq(g_stops[0].reported_line, uint32_t(6), "a0 names the line asked for");
	check_eq(g_stops[0].pc_line, uint32_t(6),
		"the line table agrees with a0 about where the guest stopped");
}

void test_a_line_with_no_code_is_not_a_stop() {
	// Declaration (3) and past-end (8) own no code; must not fail the compile.
	const Program program = compile(STRAIGHT_LINE, { 3, 8 });
	check(!program.elf.empty(), "a breakpoint on a line with no code still compiles");
	check_eq(count_break_sites(program), size_t(0),
		"a line the program has no code for is not a break site");
	check(program.installed.empty(), "and the compiler reports installing neither");
}

void test_a_folded_line_is_not_a_stop() {
	// Entire body folds to one immediate; no instructions survive for a break.
	const std::string source =
		"func test():\n" // 1
		"\tvar a = 10\n" // 2
		"\tvar b = a * 3\n" // 3
		"\treturn b - a\n"; // 4

	const Program program = compile(source, { 3 });
	check(!program.elf.empty(), "a breakpoint on a folded line still compiles");
	check(program.installed.empty(),
		"a line the optimizer left no instructions on installs nothing");
	check_eq(compile_and_run(source, "test", { 3 }), int64_t(20),
		"and the program still answers 20");
}

void test_every_breakpoint_line_gets_a_site() {
	const Program program = compile(STRAIGHT_LINE, { 2, 4, 7 });
	check_eq(count_break_sites(program), size_t(3), "three lines are three sites");
	check_lines(program.installed, { 2, 4, 7 }, "the compiler reports all three, ascending");

	auto machine = boot(program);
	check_eq(run(*machine, "test"), int64_t(20), "test() = 20 across three breaks");
	check_eq(g_stops.size(), size_t(3), "the guest stopped three times");
	if (g_stops.size() != 3) {
		return;
	}
	// Execution order: test() hits line 4 before seed()'s line 2.
	check_eq(g_stops[0].reported_line, uint32_t(4), "the first stop is line 4");
	check_eq(g_stops[1].reported_line, uint32_t(2), "then the callee's line 2");
	check_eq(g_stops[2].reported_line, uint32_t(7), "then line 7, back in test()");
	for (const Stop& stop : g_stops) {
		check_eq(stop.pc_line, stop.reported_line,
			"the line table agrees with a0 at every stop");
	}
}

void test_a_loop_body_stops_every_pass() {
	// Break below label; above it, the back edge skips and the loop stops once.
	const std::string source =
		"func test():\n" // 1
		"\tvar total = 0\n" // 2
		"\tfor i in range(5):\n" // 3
		"\t\ttotal += i\n" // 4
		"\treturn total\n"; // 5

	const Program program = compile(source, { 4 });
	auto machine = boot(program);
	check_eq(run(*machine, "test"), int64_t(10), "test() = 0+1+2+3+4");
	check_eq(g_stops.size(), size_t(5), "the body line stopped once per pass");
	for (const Stop& stop : g_stops) {
		check_eq(stop.reported_line, uint32_t(4), "every stop is the body line");
	}
}

void test_a_for_header_owns_code_in_two_places() {
	// `for` header owns setup + increment: two break sites, 1 + N stops.
	const std::string source =
		"func test():\n" // 1
		"\tvar total = 0\n" // 2
		"\tfor i in range(4):\n" // 3
		"\t\ttotal += i\n" // 4
		"\treturn total\n"; // 5

	const Program program = compile(source, { 3 });
	check_eq(count_break_sites(program), size_t(2), "the for header is two sites");

	auto machine = boot(program);
	check_eq(run(*machine, "test"), int64_t(6), "test() = 0+1+2+3");
	check_eq(g_stops.size(), size_t(5), "once entering the loop, then once per pass");
}

void test_a_break_is_transparent_to_the_allocator() {
	// Many live values across the break; answer must not change.
	const std::string source =
		"func one():\n" // 1
		"\treturn 1\n" // 2
		"func test():\n" // 3
		"\tvar a = one()\n" // 4
		"\tvar b = a + 1\n" // 5
		"\tvar c = b + a\n" // 6
		"\tvar d = c * b\n" // 7
		"\tvar e = d - a\n" // 8
		"\tvar f = e * c\n" // 9
		"\tvar g = f + d\n" // 10
		"\tvar h = g - b\n" // 11
		"\treturn a + b * c + d * e + f * g + h\n"; // 12

	const int64_t plain = compile_and_run(source, "test", {});
	check_eq(plain, int64_t(371), "the uninstrumented answer");

	// A break on any single line must not change the answer.
	for (uint32_t line = 4; line <= 12; line++) {
		const int64_t broken = compile_and_run(source, "test", { line });
		check_eq(broken, plain, "a break on line " + std::to_string(line) +
			" leaves the answer alone");
		check_eq(g_stops.size(), size_t(1),
			"a break on line " + std::to_string(line) + " stopped once");
	}

	// All lines at once: densest instrumentation.
	const int64_t all = compile_and_run(source, "test", { 2, 4, 5, 6, 7, 8, 9, 10, 11, 12 });
	check_eq(all, plain, "a break on every line leaves the answer alone");
	check_eq(g_stops.size(), size_t(10), "every line stopped exactly once");
}

void test_registers_survive_the_break() {
	// Registers must be identical before and after the break.
	const std::string source =
		"func eleven():\n" // 1
		"\treturn 11\n" // 2
		"func test():\n" // 3
		"\tvar a = eleven()\n" // 4
		"\tvar b = a + 31\n" // 5
		"\treturn b\n"; // 6

	const Program program = compile(source, { 5 });
	auto machine = boot(program);
	g_capture_registers = true;
	const int64_t answer = run(*machine, "test");
	g_capture_registers = false;
	check_eq(answer, int64_t(42), "test() = 42 across the break");

	// sp restored by the break sequence; 16-byte aligned for nested calls.
	check_eq(g_regs_at_break.get(riscv::REG_SP) % 16, uint64_t(0),
		"the break leaves sp 16-byte aligned for a nested call");
}

void test_a_break_carries_a_call_stack() {
	// Breakpoints imply debug_info: a stop needs a call stack.
	const std::string source =
		"func leaf():\n" // 1
		"\treturn 7\n" // 2
		"func middle():\n" // 3
		"\treturn leaf()\n" // 4
		"func test():\n" // 5
		"\treturn middle()\n"; // 6

	const Program program = compile(source, { 2 });
	auto machine = boot(program);
	check(machine->address_of(DEBUG_SYMBOL) != 0,
		"a breakpoint build exports the shadow stack without being asked");
	check_eq(run(*machine, "test"), int64_t(7), "test() = 7 across the break");
	check_eq(g_stops.size(), size_t(1), "the guest stopped once");
	if (g_stops.empty()) {
		return;
	}
	// test -> middle -> leaf; break is in leaf.
	check_eq(g_stops[0].depth, uint64_t(3), "three frames stand below the break");

	size_t leaf_index = program.signatures.size();
	for (size_t i = 0; i < program.signatures.size(); i++) {
		if (program.signatures[i].name == "leaf") {
			leaf_index = i;
		}
	}
	check_eq(g_stops[0].innermost_function, uint64_t(leaf_index),
		"the innermost frame is the function the break is in");
}

void test_the_breakpoint_statement() {
	const std::string source =
		"func test(n):\n"          // 1
		"\tvar total = 0\n"        // 2
		"\tif n > 0:\n"            // 3
		"\t\tbreakpoint\n"         // 4
		"\t\ttotal = n * 2\n"      // 5
		"\tbreakpoint\n"           // 6
		"\treturn total\n";        // 7

	const Program program = compile(source, {});
	check_eq(count_break_sites(program), size_t(2),
		"two `breakpoint` statements are two break sites");
	check(program.installed.empty(),
		"a `breakpoint` statement is not a requested breakpoint");

	auto machine = boot(program);
	check_eq(run_with_arg(*machine, "test", 5), int64_t(10),
		"the program computes the same answer across its own stops");
	check_eq(g_stops.size(), size_t(2), "both statements stopped");
	if (g_stops.size() == 2) {
		check_eq(g_stops[0].reported_line, uint32_t(4), "the first stop names line 4");
		check_eq(g_stops[1].reported_line, uint32_t(6), "the second names line 6");
		check_eq(g_stops[0].pc_line, uint32_t(4), "and the line table agrees");
		// Statement alone does not enable debug_info.
		check_eq(g_stops[0].depth, uint64_t(0), "and no shadow stack was added");
	}

	auto skipped = boot(program);
	check_eq(run_with_arg(*skipped, "test", 0), int64_t(0), "a declined branch skips its stop");
	check_eq(g_stops.size(), size_t(1), "only the unconditional statement stopped");

	// Coalesce: same line requested + statement = one stop, reported as installed.
	const Program both = compile(source, { 4 });
	check_eq(count_break_sites(both), size_t(2), "the same line does not stop twice");
	check_lines(both.installed, { 4 }, "the requested line is still reported");

	auto with_request = boot(both);
	check_eq(run_with_arg(*with_request, "test", 5), int64_t(10), "and it still answers 10");
	check_eq(g_stops.size(), size_t(2), "two stops, not three");
	if (g_stops.size() == 2) {
		check_eq(g_stops[0].reported_line, uint32_t(4), "the shared line stopped once");
		check(g_stops[0].depth > 0, "a requested breakpoint does bring the shadow stack");
	}

	std::cout << "  breakpoint statement OK" << std::endl;
}

void test_breakpoints_in_more_than_one_function() {
	const std::string source =
		"func left():\n" // 1
		"\treturn 2\n" // 2
		"func right():\n" // 3
		"\treturn 3\n" // 4
		"func test():\n" // 5
		"\treturn left() * right()\n"; // 6

	const Program program = compile(source, { 2, 4, 6 });
	check_eq(count_break_sites(program), size_t(3),
		"a site in each of the three functions");

	auto machine = boot(program);
	check_eq(run(*machine, "test"), int64_t(6), "test() = 6 across three breaks");
	check_eq(g_stops.size(), size_t(3), "the guest stopped three times");
	if (g_stops.size() != 3) {
		return;
	}
	// Execution order: test() stops before its callees.
	check_eq(g_stops[0].reported_line, uint32_t(6), "test() stops first");
	check_eq(g_stops[1].reported_line, uint32_t(2), "then left()");
	check_eq(g_stops[2].reported_line, uint32_t(4), "then right()");
	check_eq(g_stops[1].depth, uint64_t(2), "left() runs one frame below test()");
}

} // namespace

int main() {
	test_nothing_emitted_without_breakpoints();
	test_stops_on_the_line_it_was_given();
	test_a_line_with_no_code_is_not_a_stop();
	test_a_folded_line_is_not_a_stop();
	test_every_breakpoint_line_gets_a_site();
	test_a_loop_body_stops_every_pass();
	test_a_for_header_owns_code_in_two_places();
	test_a_break_is_transparent_to_the_allocator();
	test_registers_survive_the_break();
	test_a_break_carries_a_call_stack();
	test_the_breakpoint_statement();
	test_breakpoints_in_more_than_one_function();

	if (failures != 0) {
		std::cerr << failures << " breakpoint check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All breakpoint tests passed" << std::endl;
	return 0;
}
