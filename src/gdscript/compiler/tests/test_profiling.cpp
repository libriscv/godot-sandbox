// Runs profiled ELFs on a real libriscv machine and checks the accounting:
// self excludes callees, total includes them, entry/exit balance across
// recursion past MAX_DEPTH. INSTRUCTIONS clock throughout for determinism.
#include "../compiler.h"
#include "scope_stub.h"
#include "../profiling_layout.h"
#include <cstdint>
#include <cstring>
#include <iostream>
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

std::vector<uint8_t> compile(const std::string& source, bool profiling,
	ProfilingClock clock = ProfilingClock::INSTRUCTIONS) {
	Compiler compiler;
	CompilerOptions options;
	options.profiling = profiling;
	options.profiling_clock = clock;
	std::vector<uint8_t> elf = compiler.compile(source, options);
	if (elf.empty()) {
		std::cerr << "FAILED to compile: " << compiler.get_error() << std::endl;
		failures++;
	}
	return elf;
}

// -= Reading the area back out of a machine =-

struct Record {
	uint64_t call_count = 0;
	uint64_t self = 0;
	uint64_t total = 0;
};

struct Area {
	uint64_t address = 0;
	uint32_t magic = 0;
	uint32_t version = 0;
	uint32_t function_count = 0;
	uint32_t record_size = 0;
	uint32_t max_depth = 0;
	uint32_t clock = 0;
	uint64_t depth = 0;
	uint64_t overflow = 0;
	std::vector<Record> records;
};

template <typename T>
T read(machine_t& machine, uint64_t address) {
	T value {};
	machine.copy_from_guest(&value, address, sizeof(T));
	return value;
}

Area read_area(machine_t& machine) {
	Area area;
	area.address = machine.address_of(PROFILING_SYMBOL);
	if (area.address == 0) {
		return area;
	}
	const uint64_t base = area.address;
	area.magic = read<uint32_t>(machine, base + ProfilingLayout::MAGIC_OFF);
	area.version = read<uint32_t>(machine, base + ProfilingLayout::VERSION_OFF);
	area.function_count = read<uint32_t>(machine, base + ProfilingLayout::FUNCTION_COUNT_OFF);
	area.record_size = read<uint32_t>(machine, base + ProfilingLayout::RECORD_SIZE_OFF);
	area.max_depth = read<uint32_t>(machine, base + ProfilingLayout::MAX_DEPTH_OFF);
	area.clock = read<uint32_t>(machine, base + ProfilingLayout::CLOCK_OFF);
	area.depth = read<uint64_t>(machine, base + ProfilingLayout::DEPTH_OFF);
	area.overflow = read<uint64_t>(machine, base + ProfilingLayout::OVERFLOW_OFF);
	for (uint32_t i = 0; i < area.function_count; i++) {
		const uint64_t record = base + ProfilingLayout::record_offset(i);
		Record entry;
		entry.call_count = read<uint64_t>(machine, record + ProfilingLayout::CALL_COUNT_OFF);
		entry.self = read<uint64_t>(machine, record + ProfilingLayout::SELF_OFF);
		entry.total = read<uint64_t>(machine, record + ProfilingLayout::TOTAL_OFF);
		area.records.push_back(entry);
	}
	return area;
}

// The Sandbox ABI, reduced to what a program with no arguments needs: a0 points
// at a Variant the callee writes its return value into.
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

// The corpus below is written to need no host: no strings, no containers, and
// no untyped arithmetic, so nothing reaches a syscall. Anything that does is a
// test bug, and says so rather than being answered with a zero.
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
	install_scope_stub<machine_t>();
	// The entry point initializes globals and stops.
	machine->simulate(50'000'000ull);
	return machine;
}

// -= The tests =-

void test_off_by_default() {
	const std::string source =
		"func leaf():\n"
		"\treturn 1\n"
		"func test():\n"
		"\treturn leaf()\n";

	Compiler compiler;
	CompilerOptions options;
	check(!options.profiling, "profiling defaults to off");

	const std::vector<uint8_t> elf = compile(source, false);
	if (elf.empty()) {
		return;
	}
	auto machine = boot(elf);
	check_eq(machine->address_of(PROFILING_SYMBOL), uint64_t(0),
		"an unprofiled build exports no profiling symbol");

	// Nothing reads a CSR unless the instrumentation is emitted, so the whole
	// text is searchable for one: csrrs rd, csr, x0 is opcode 0x73 funct3 2.
	size_t csr_reads = 0;
	for (size_t i = 0; i + 4 <= elf.size(); i += 4) {
		uint32_t word = 0;
		memcpy(&word, elf.data() + i, 4);
		if ((word & 0x7F) == 0x73 && ((word >> 12) & 0x7) == 0x2) {
			csr_reads++;
		}
	}
	check_eq(csr_reads, size_t(0), "an unprofiled build reads no clock");
}

void test_header() {
	const std::string source =
		"func a():\n"
		"\treturn 1\n"
		"func b():\n"
		"\treturn 2\n"
		"func test():\n"
		"\treturn 3\n";

	const std::vector<uint8_t> elf = compile(source, true);
	if (elf.empty()) {
		return;
	}
	auto machine = boot(elf);
	const Area area = read_area(*machine);

	check(area.address != 0, "a profiled build exports the profiling symbol");
	check_eq(area.magic, ProfilingLayout::MAGIC, "magic");
	check_eq(area.version, ProfilingLayout::LAYOUT_VERSION, "version");
	check_eq(area.function_count, uint32_t(3), "one record per function");
	check_eq(area.record_size, uint32_t(ProfilingLayout::RECORD_SIZE), "record size");
	check_eq(area.max_depth, ProfilingLayout::MAX_DEPTH, "max depth");
	check_eq(area.clock, uint32_t(ProfilingClock::INSTRUCTIONS), "the clock that was asked for");
	check_eq(area.depth, uint64_t(0), "depth starts at zero");
	check_eq(area.overflow, uint64_t(0), "overflow starts at zero");

	// The area must be readable to its last byte: a short .data segment would
	// only show up when a deep call reached the end of the shadow stack.
	const uint64_t last = area.address + ProfilingLayout::area_size(3) - 8;
	check_eq(read<uint64_t>(*machine, last), uint64_t(0), "the shadow stack is mapped to its end");

	// A record is untouched until its function runs.
	for (const Record& record : area.records) {
		check_eq(record.call_count, uint64_t(0), "no calls recorded before the program runs");
	}

	// The TIME clock is the same sequence reading a different CSR.
	const std::vector<uint8_t> timed = compile(source, true, ProfilingClock::TIME);
	if (!timed.empty()) {
		auto timed_machine = boot(timed);
		check_eq(read_area(*timed_machine).clock, uint32_t(ProfilingClock::TIME),
			"the header names the clock the code reads");
		check_eq(timed.size(), elf.size(), "the clock choice does not change the code size");
	}
}

void test_call_counts_and_nesting() {
	// leaf is called twice per mid, mid three times by test.
	const std::string source =
		"func leaf():\n"
		"\treturn 1\n"
		"func mid():\n"
		"\tleaf()\n"
		"\tleaf()\n"
		"\treturn 2\n"
		"func test():\n"
		"\tmid()\n"
		"\tmid()\n"
		"\tmid()\n"
		"\treturn 3\n";

	const std::vector<uint8_t> elf = compile(source, true);
	if (elf.empty()) {
		return;
	}
	auto machine = boot(elf);
	if (!run(*machine, "test")) {
		return;
	}
	const Area area = read_area(*machine);
	if (area.records.size() != 3) {
		check(false, "three records");
		return;
	}
	const Record& leaf = area.records[0];
	const Record& mid = area.records[1];
	const Record& test = area.records[2];

	check_eq(leaf.call_count, uint64_t(6), "leaf() called six times");
	check_eq(mid.call_count, uint64_t(3), "mid() called three times");
	check_eq(test.call_count, uint64_t(1), "test() called once");

	check_eq(area.depth, uint64_t(0), "entry and exit balance out");
	check_eq(area.overflow, uint64_t(0), "nothing overflowed the shadow stack");

	// A leaf has no children, so the two numbers are the same span.
	check_eq(leaf.self, leaf.total, "a leaf's self time is its total time");

	// Everything is inside test(), which is where the outermost span is.
	check(test.total > mid.total, "test() encloses every mid() call");
	check(mid.total > leaf.total, "mid() encloses the leaf() calls it makes");

	// self excludes callees: subtracting them is what makes the numbers add up.
	check(test.self < test.total, "test() spends most of its span in mid()");
	check(mid.self < mid.total, "mid() spends part of its span in leaf()");
	check(test.total >= test.self + mid.total,
		"test()'s total covers its own work and all of mid()'s");
	check(mid.total >= mid.self + leaf.total,
		"mid()'s total covers its own work and all of leaf()'s");

	// Nothing is charged twice: the three spans partition the outermost one.
	check_eq(test.self + mid.self + leaf.self, test.total,
		"self times partition the outermost total");
}

void test_recursion_overflows_the_shadow_stack() {
	// Deeper than MAX_DEPTH, so the frames past the cap are counted and skipped
	// while the ones below keep their timings -- and depth still comes back to
	// zero, which is the property that makes the cap safe.
	const std::string source =
		"func down(n: int):\n"
		"\tif n > 0:\n"
		"\t\tdown(n - 1)\n"
		"\treturn n\n"
		"func test():\n"
		"\tdown(400)\n"
		"\treturn 0\n";

	const std::vector<uint8_t> elf = compile(source, true);
	if (elf.empty()) {
		return;
	}
	auto machine = boot(elf);
	if (!run(*machine, "test")) {
		return;
	}
	const Area area = read_area(*machine);
	if (area.records.size() != 2) {
		check(false, "two records");
		return;
	}
	const Record& down = area.records[0];
	const Record& test = area.records[1];

	check_eq(area.depth, uint64_t(0), "depth returns to zero past the cap");

	// A record describes recorded frames only. test() plus 400 nested down()
	// frames fill the cap; the rest are counted in `overflow` and appear in no
	// record at all, which is what keeps call_count, self and total consistent
	// with each other.
	const uint64_t recorded = ProfilingLayout::MAX_DEPTH - 1;
	const uint64_t expected_overflow = 401 - recorded;
	check_eq(down.call_count, recorded, "the frames below the cap are counted");
	check_eq(area.overflow, expected_overflow, "the frames past it are counted separately");

	check(down.total > 0, "the frames below the cap were still timed");

	// Under recursion `total` counts the same span once per active frame, so
	// the totals do not partition anything. The self times still do -- an
	// unrecorded frame's work lands in the self time of the recorded frame
	// below it, rather than being lost.
	check_eq(down.self + test.self, test.total,
		"self times partition the outermost total even past the cap");
}

} // namespace

int main() {
	test_off_by_default();
	test_header();
	test_call_counts_and_nesting();
	test_recursion_overflows_the_shadow_stack();

	if (failures > 0) {
		std::cerr << failures << " profiling test(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All profiling tests passed" << std::endl;
	return 0;
}
