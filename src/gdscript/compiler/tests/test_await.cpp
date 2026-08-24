// AWAIT lowering tests: frame handoff, state dispatch, barrier correctness.
// Runs compiled coroutines on libriscv against a minimal host stub.
// Handle promotion covered by test_await_host_* in tests/test_basic.gd.
#include "../compiler.h"
#include "../syscall_numbers.h"
#include "../variant_layout.h"
#include "../variant_types.h"
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

const VariantLayout LAYOUT = native_variant_layout();

struct Program {
	std::vector<uint8_t> elf;
	std::vector<FunctionSignature> signatures;
};

Program compile(const std::string& source, bool optimize = true) {
	Program out;
	Compiler compiler;
	CompilerOptions options;
	options.optimize = optimize;
	out.elf = compiler.compile(source, options);
	if (out.elf.empty()) {
		std::cerr << "FAILED to compile: " << compiler.get_error() << std::endl;
		failures++;
		return out;
	}
	out.signatures = compiler.get_function_signatures();
	return out;
}

// Host stub

// Captured suspension state.
struct Suspension {
	uint64_t operand = 0;
	uint64_t frame_base = 0;
	uint32_t frame_size = 0;
	int32_t state = 0;
	uint64_t resume = 0;
	int32_t result_offset = 0;
	std::vector<uint8_t> frame;
};

struct Host {
	std::vector<Suspension> suspensions;
	int64_t sent = 0;
	bool awaitable = true;
	bool size_mismatch_refused = false;
	int restores = 0;
	int resuming = -1;
	int guest_calls = 0;
};

Host g_host;

void await_syscall(machine_t& machine) {
	Suspension s;
	s.operand = machine.cpu.reg(riscv::REG_ARG0);
	s.frame_base = machine.cpu.reg(riscv::REG_ARG1);
	s.frame_size = uint32_t(machine.cpu.reg(riscv::REG_ARG2));
	s.state = int32_t(machine.cpu.reg(riscv::REG_ARG3));
	s.resume = machine.cpu.reg(riscv::REG_ARG0 + 4);
	s.result_offset = int32_t(machine.cpu.reg(riscv::REG_ARG0 + 5));

	if (!g_host.awaitable) {
		std::vector<uint8_t> value(size_t(LAYOUT.variant_size()));
		machine.copy_from_guest(value.data(), s.operand, value.size());
		machine.copy_to_guest(s.frame_base + uint32_t(s.result_offset), value.data(), value.size());
		g_host.suspensions.push_back(std::move(s));
		machine.set_result(0);
		return;
	}

	s.frame.resize(s.frame_size);
	machine.copy_from_guest(s.frame.data(), s.frame_base, s.frame_size);
	g_host.suspensions.push_back(std::move(s));
	machine.set_result(1);
}

void await_restore_syscall(machine_t& machine) {
	const uint64_t frame_base = machine.cpu.reg(riscv::REG_ARG0);
	const uint32_t frame_size = uint32_t(machine.cpu.reg(riscv::REG_ARG1));

	if (g_host.suspensions.empty()) {
		std::cerr << "FAILED: a resume ran without a suspension" << std::endl;
		failures++;
		machine.set_result(0);
		return;
	}
	Suspension& s = g_host.resuming >= 0
		? g_host.suspensions[size_t(g_host.resuming)]
		: g_host.suspensions.back();
	if (frame_size != s.frame.size()) {
		g_host.size_mismatch_refused = true;
		machine.set_result(0);
		return;
	}

	std::vector<uint8_t> frame = s.frame;
	if (s.result_offset >= 0) {
		uint8_t* slot = frame.data() + size_t(s.result_offset);
		std::memset(slot, 0, size_t(LAYOUT.variant_size()));
		const int32_t type = int32_t(Variant::INT);
		std::memcpy(slot + VariantLayout::TYPE_OFFSET, &type, sizeof(type));
		std::memcpy(slot + VariantLayout::DATA_OFFSET, &g_host.sent, sizeof(g_host.sent));
	}
	machine.copy_to_guest(frame_base, frame.data(), frame.size());
	g_host.restores++;
	machine.set_result(s.state);
}

void call_guest_syscall(machine_t& machine) {
	const uint64_t address = machine.cpu.reg(riscv::REG_ARG0);
	const uint64_t args_ptr = machine.cpu.reg(riscv::REG_ARG1);
	const unsigned argc = unsigned(machine.cpu.reg(riscv::REG_ARG2));
	const uint64_t result_ptr = machine.cpu.reg(riscv::REG_ARG0 + 3);
	g_host.guest_calls++;

	const size_t before = g_host.suspensions.size();

	auto& cpu = machine.cpu;
	riscv::Registers<riscv::RISCV64> regs = cpu.registers();
	auto& sp = cpu.reg(riscv::REG_SP);

	sp -= 64;
	const uint64_t retvar = sp;
	sp -= 32;

	cpu.reg(riscv::REG_ARG0) = retvar;
	for (unsigned i = 0; i < argc; i++) {
		cpu.reg(riscv::REG_ARG1 + i) = args_ptr + uint64_t(i) * uint64_t(LAYOUT.variant_size());
	}
	cpu.reg(riscv::REG_RA) = machine.memory.exit_address();
	cpu.preempt_internal(regs, true, true, address, 200'000'000ull);

	std::vector<uint8_t> value(size_t(LAYOUT.variant_size()), 0);
	if (g_host.suspensions.size() > before) {
		const int32_t type = int32_t(Variant::SIGNAL);
		const int64_t which = int64_t(g_host.suspensions.size() - 1);
		std::memcpy(value.data() + VariantLayout::TYPE_OFFSET, &type, sizeof(type));
		std::memcpy(value.data() + VariantLayout::DATA_OFFSET, &which, sizeof(which));
	} else {
		machine.copy_from_guest(value.data(), retvar, value.size());
	}
	machine.copy_to_guest(result_ptr, value.data(), value.size());
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
	machine_t::install_syscall_handler(ECALL_AWAIT, await_syscall);
	machine_t::install_syscall_handler(ECALL_AWAIT_RESTORE, await_restore_syscall);
	machine_t::install_syscall_handler(ECALL_CALL_GUEST, call_guest_syscall);
	g_host = Host {};
	machine->simulate(50'000'000ull);
	return machine;
}

struct Call {
	int64_t value = 0;
	int32_t type = 0;
	uint64_t stack_on_entry = 0;
	uint64_t stack_on_exit = 0;
};

Call run_at(machine_t& machine, uint64_t address, const std::vector<int64_t>& args) {
	if (address == 0) {
		std::cerr << "FAILED: called address 0" << std::endl;
		failures++;
		return {};
	}
	auto& sp = machine.cpu.reg(riscv::REG_SP);
	sp = machine.memory.stack_initial();
	sp -= 64 + 64 * int(args.size());
	const uint64_t retvar = sp;

	for (size_t i = 0; i < args.size(); i++) {
		const uint64_t slot = retvar + 64 + 64 * i;
		const int32_t type = int32_t(Variant::INT);
		machine.copy_to_guest(slot + VariantLayout::TYPE_OFFSET, &type, sizeof(type));
		machine.copy_to_guest(slot + VariantLayout::DATA_OFFSET, &args[i], sizeof(args[i]));
		machine.cpu.reg(riscv::REG_ARG1 + i) = slot;
	}

	machine.cpu.reg(riscv::REG_RA) = machine.memory.exit_address();
	machine.cpu.reg(riscv::REG_ARG0) = retvar;
	machine.cpu.jump(address);
	machine.simulate(200'000'000ull);

	Call out;
	out.stack_on_entry = retvar;
	out.stack_on_exit = machine.cpu.reg(riscv::REG_SP);
	machine.copy_from_guest(&out.type, retvar + VariantLayout::TYPE_OFFSET, sizeof(out.type));
	machine.copy_from_guest(&out.value, retvar + VariantLayout::DATA_OFFSET, sizeof(out.value));
	return out;
}

Call run(machine_t& machine, const std::string& function, const std::vector<int64_t>& args = {}) {
	const uint64_t address = machine.address_of(function);
	if (address == 0) {
		std::cerr << "FAILED: no symbol for " << function << "()" << std::endl;
		failures++;
	}
	return run_at(machine, address, args);
}

const FunctionSignature* signature_of(const Program& program, const std::string& name) {
	for (const FunctionSignature& sig : program.signatures) {
		if (sig.name == name) {
			return &sig;
		}
	}
	return nullptr;
}

// Single-suspension round trip.
void test_one_suspension() {
	std::cout << "Testing a single suspension..." << std::endl;

	const Program program = compile(
		"func wait_for(sig, base):\n"
		"\tvar got = await sig\n"
		"\treturn base + got\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);
	const Call suspended = run(*machine, "wait_for", { 0, 100 });

	check_eq<size_t>(g_host.suspensions.size(), 1, "one suspension");
	if (g_host.suspensions.empty()) {
		return;
	}
	const Suspension& s = g_host.suspensions.front();

	check_eq<uint64_t>(s.operand, s.frame_base,
		"the first slot of the frame is the first parameter");
	check(s.frame_size % uint32_t(LAYOUT.variant_size()) == 0,
		"the frame is a whole number of Variant slots");
	check(s.frame_size >= 4u * uint32_t(LAYOUT.variant_size()),
		"the frame covers the function's slots and the scratch slots");
	check_eq<int32_t>(s.state, 0, "the first suspension is state 0");
	check(s.resume != 0, "a resume address was handed over");
	check(s.result_offset >= 0, "the await names a result slot");
	check(s.result_offset % LAYOUT.variant_size() == 0,
		"the result slot is one of the frame's Variant slots");
	check(uint32_t(s.result_offset) + uint32_t(LAYOUT.variant_size()) <= s.frame_size,
		"the result slot lies inside the frame");

	check_eq<uint64_t>(suspended.stack_on_exit, suspended.stack_on_entry,
		"the suspend epilogue put the stack pointer back");

	g_host.sent = 7;
	const Call finished = run_at(*machine, s.resume, {});
	check_eq<int>(g_host.restores, 1, "the resume asked for its frame back");
	check_eq<int64_t>(finished.value, 107, "the resumed body computed with the sent value");
	check_eq<int32_t>(finished.type, int32_t(Variant::INT), "and returned an integer");

	std::cout << "  ✓ One suspension round-trips through the host" << std::endl;
}

// Two suspension points dispatch to distinct states.
void test_two_suspensions_dispatch() {
	std::cout << "Testing dispatch over two suspension points..." << std::endl;

	const Program program = compile(
		"func twice(a, b):\n"
		"\tvar first = await a\n"
		"\tvar second = await b\n"
		"\treturn first * 100 + second\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);
	run(*machine, "twice", { 0, 0 });
	check_eq<size_t>(g_host.suspensions.size(), 1, "suspended at the first await");
	if (g_host.suspensions.empty()) {
		return;
	}
	check_eq<int32_t>(g_host.suspensions.back().state, 0, "state 0");
	const uint64_t resume = g_host.suspensions.back().resume;

	g_host.sent = 3;
	run_at(*machine, resume, {});
	check_eq<size_t>(g_host.suspensions.size(), 2, "suspended again at the second await");
	if (g_host.suspensions.size() < 2) {
		return;
	}
	check_eq<int32_t>(g_host.suspensions.back().state, 1, "state 1");
	check_eq<uint64_t>(g_host.suspensions.back().resume, resume,
		"both suspensions name the same resume entry");
	check_eq<uint32_t>(g_host.suspensions.back().frame_size, g_host.suspensions.front().frame_size,
		"the frame is the same size at both ends");

	g_host.sent = 4;
	const Call finished = run_at(*machine, g_host.suspensions.back().resume, {});
	check_eq<int64_t>(finished.value, 304, "each resume landed on its own state");

	std::cout << "  ✓ Two suspensions dispatch to their own states" << std::endl;
}

// Locals survive the suspension barrier.
void test_the_barrier_keeps_locals() {
	std::cout << "Testing that locals survive a suspension..." << std::endl;

	const Program program = compile(
		"func carry(sig, seed):\n"
		"\tvar a = seed * 2\n"
		"\tvar b = a + 1\n"
		"\tvar c = b * b\n"
		"\tvar got = await sig\n"
		"\treturn a + b + c + got\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);
	run(*machine, "carry", { 0, 5 });
	if (g_host.suspensions.empty()) {
		check(false, "the body suspended");
		return;
	}

	g_host.sent = 1000;
	const Call finished = run_at(*machine, g_host.suspensions.back().resume, {});
	check_eq<int64_t>(finished.value, 1142, "every local came back");

	std::cout << "  ✓ Locals cross the suspension in the frame" << std::endl;
}

// Non-Signal operand: no suspension, falls through.
void test_not_awaitable_falls_through() {
	std::cout << "Testing an operand the host will not suspend on..." << std::endl;

	const Program program = compile(
		"func wait_for(sig, base):\n"
		"\tvar got = await sig\n"
		"\treturn base + got\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);
	g_host.awaitable = false;
	const Call finished = run(*machine, "wait_for", { 42, 100 });

	check_eq<size_t>(g_host.suspensions.size(), 1, "the await still reached the host");
	check_eq<int>(g_host.restores, 0, "but nothing was restored");
	check_eq<int64_t>(finished.value, 142, "the body ran to its return in one call");

	std::cout << "  ✓ A non-awaitable operand does not suspend" << std::endl;
}

// Suspend and resume agree on frame size.
void test_the_frame_size_is_the_contract() {
	std::cout << "Testing that both ends agree on the frame size..." << std::endl;

	const Program program = compile(
		"func wait_for(sig):\n"
		"\treturn await sig\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);
	run(*machine, "wait_for", { 0 });
	if (g_host.suspensions.empty()) {
		check(false, "the body suspended");
		return;
	}

	g_host.sent = 9;
	run_at(*machine, g_host.suspensions.back().resume, {});
	check(!g_host.size_mismatch_refused,
		"the resume asked for exactly the frame the suspension recorded");

	std::cout << "  ✓ The suspension and the resume name the same frame" << std::endl;
}

// A coroutine always opens a frame, even with a trivial body.
void test_a_coroutine_always_has_a_frame() {
	std::cout << "Testing that a coroutine is never frameless..." << std::endl;

	const Program program = compile(
		"func plain():\n"
		"\treturn 1\n"
		"func tiny(sig):\n"
		"\treturn await sig\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);

	const auto opens_a_frame = [&](const std::string& name) {
		const uint64_t address = machine->address_of(name);
		if (address == 0) {
			check(false, "no symbol for " + name + "()");
			return false;
		}
		uint32_t word = 0;
		machine->copy_from_guest(&word, address, sizeof(word));
		const bool is_addi_sp = (word & 0x000FFFFFu) == 0x00010113u;
		const int32_t imm = int32_t(word) >> 20;
		return is_addi_sp && imm < 0;
	};

	check(!opens_a_frame("plain"), "a function that writes only through a0 keeps no frame");
	check(opens_a_frame("tiny"), "a coroutine opens a frame even when its body is one await");

	std::cout << "  ✓ A coroutine always has a frame" << std::endl;
}

// Coroutine signature publishes ANY return type and the is_coroutine flag.
void test_the_signature_says_coroutine() {
	std::cout << "Testing the published signature..." << std::endl;

	const Program program = compile(
		"func plain() -> int:\n"
		"\treturn 1\n"
		"func waits(sig) -> int:\n"
		"\treturn await sig\n");
	if (program.elf.empty()) {
		return;
	}

	const FunctionSignature* plain = signature_of(program, "plain");
	const FunctionSignature* waits = signature_of(program, "waits");
	check(plain != nullptr && waits != nullptr, "both signatures published");
	if (plain == nullptr || waits == nullptr) {
		return;
	}
	check(!plain->is_coroutine, "an ordinary function is not a coroutine");
	check_eq<int32_t>(plain->return_type, int32_t(Variant::INT), "and keeps its declared type");
	check(waits->is_coroutine, "a function containing await is a coroutine");
	check_eq<int32_t>(waits->return_type, FunctionParameter::ANY_TYPE,
		"and publishes no return type, because a suspension answers with a Signal");

	// The blob is what actually crosses to the host.
	const std::vector<uint8_t> blob = encode_function_signatures(program.signatures);
	std::vector<FunctionSignature> decoded;
	check(decode_function_signatures(blob.data(), blob.size(), decoded), "the blob decodes");
	check_eq<size_t>(decoded.size(), program.signatures.size(), "with every signature");
	if (decoded.size() == program.signatures.size()) {
		for (size_t i = 0; i < decoded.size(); i++) {
			check(decoded[i].is_coroutine == program.signatures[i].is_coroutine,
				"is_coroutine survives the encoding for " + decoded[i].name);
		}
	}

	std::cout << "  ✓ The signature carries the coroutine bit" << std::endl;
}

// `await` in a loop: one resume entry, one state, entered as many times as the loop runs.
void test_awaiting_in_a_loop() {
	std::cout << "Testing a suspension inside a loop..." << std::endl;

	const Program program = compile(
		"func gather(sig):\n"
		"\tvar total = 0\n"
		"\tfor i in range(3):\n"
		"\t\ttotal = total + await sig\n"
		"\treturn total\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);
	run(*machine, "gather", { 0 });
	check_eq<size_t>(g_host.suspensions.size(), 1, "suspended on the first pass");
	if (g_host.suspensions.empty()) {
		return;
	}
	const uint64_t resume = g_host.suspensions.back().resume;

	g_host.sent = 10;
	run_at(*machine, resume, {});
	check_eq<size_t>(g_host.suspensions.size(), 2, "and again on the second");

	g_host.sent = 20;
	run_at(*machine, resume, {});
	check_eq<size_t>(g_host.suspensions.size(), 3, "and the third");

	g_host.sent = 30;
	const Call finished = run_at(*machine, resume, {});
	check_eq<size_t>(g_host.suspensions.size(), 3, "the loop is done suspending");
	check_eq<int64_t>(finished.value, 60, "the accumulator survived every pass");

	std::cout << "  ✓ A loop body may suspend on every pass" << std::endl;
}

// An unused await is not deleted by the optimizer.
void test_an_unused_await_is_kept() {
	std::cout << "Testing that an unused await is not optimized away..." << std::endl;

	const Program program = compile(
		"func ping(sig):\n"
		"\tawait sig\n"
		"\treturn 5\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);
	run(*machine, "ping", { 0 });
	check_eq<size_t>(g_host.suspensions.size(), 1, "the await survived the optimizer");
	if (g_host.suspensions.empty()) {
		return;
	}

	g_host.sent = 0;
	const Call finished = run_at(*machine, g_host.suspensions.back().resume, {});
	check_eq<int64_t>(finished.value, 5, "and the body ran to its return");

	std::cout << "  ✓ An await with an unused result is kept" << std::endl;
}

int64_t resume_suspension(machine_t& machine, int index, int64_t sent) {
	const int previous = g_host.resuming;
	g_host.resuming = index;
	g_host.sent = sent;
	const int64_t value = run_at(machine, g_host.suspensions[size_t(index)].resume, {}).value;
	g_host.resuming = previous;
	return value;
}

void test_awaiting_another_coroutine() {
	std::cout << "Testing one coroutine awaiting another..." << std::endl;

	const Program program = compile(
		"func inner(sig, n):\n"
		"\tvar got = await sig\n"
		"\treturn got + n\n"
		"func outer(sig, n):\n"
		"\tvar v = await inner(sig, n)\n"
		"\treturn v + 1\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);
	const Call first = run(*machine, "outer", { 0, 10 });
	check_eq(g_host.guest_calls, 1, "the call to the coroutine went through the host");
	check_eq<size_t>(g_host.suspensions.size(), 2,
		"both frames suspended: the callee's, then the caller's on its Signal");
	check_eq<int32_t>(first.type, int32_t(Variant::NIL), "the entry call answered nothing yet");

	const int64_t inner_result = resume_suspension(*machine, 0, 32);
	check_eq<int64_t>(inner_result, 42, "the callee saw its own await's value and its argument");

	check_eq<int64_t>(resume_suspension(*machine, 1, inner_result), 43,
		"the caller resumed with what the callee returned");

	std::cout << "  ✓ A coroutine can await another one in the same program" << std::endl;
}

void test_calling_a_coroutine_without_awaiting_it() {
	std::cout << "Testing a coroutine called without await..." << std::endl;

	const Program program = compile(
		"func inner(sig):\n"
		"\treturn await sig\n"
		"func outer(sig):\n"
		"\tvar handle = inner(sig)\n"
		"\treturn typeof(handle)\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);
	const Call result = run(*machine, "outer", { 0 });
	check_eq<int64_t>(result.value, int64_t(Variant::SIGNAL),
		"a call with no await answers the Signal to await later");
	check_eq<size_t>(g_host.suspensions.size(), 1, "only the callee suspended");

	std::cout << "  ✓ A call with no await answers the Signal, and the caller runs on"
		<< std::endl;
}

void test_a_global_read_is_not_hoisted_across_a_hosted_call() {
	std::cout << "Testing a global read across a call to a coroutine..." << std::endl;

	const Program program = compile(
		"var g: int = 1\n"
		"func inner(sig):\n"
		"\tg = 9\n"
		"\treturn await sig\n"
		"func outer(sig, k: int):\n"
		"\tvar a: int = g\n"
		"\tinner(sig)\n"
		"\treturn a + k\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);
	const Call result = run(*machine, "outer", { 0, 0 });
	check_eq(g_host.guest_calls, 1, "the coroutine was reached through the host");
	check_eq<int64_t>(result.value, 1,
		"the read happened before the coroutine stored to the global");

	std::cout << "  ✓ A hosted call counts as a store to every global" << std::endl;
}

void test_an_ordinary_call_stays_a_jal() {
	std::cout << "Testing that only a coroutine leaves the program..." << std::endl;

	const Program program = compile(
		"func plain(x):\n"
		"\treturn x + 1\n"
		"func waits(sig):\n"
		"\treturn plain(await sig)\n");
	check(!program.elf.empty(), "a coroutine may call an ordinary function");
	if (program.elf.empty()) {
		return;
	}
	auto machine = boot(program);
	run(*machine, "waits", { 0 });
	if (g_host.suspensions.empty()) {
		check(false, "the body suspended");
		return;
	}
	g_host.sent = 41;
	check_eq<int64_t>(run_at(*machine, g_host.suspensions.back().resume, {}).value, 42,
		"and the call runs after the resume");
	check_eq(g_host.guest_calls, 0, "an ordinary call did not go through the host");

	std::cout << "  ✓ Only a call to a coroutine costs a trip through the host" << std::endl;
}

// Coroutine prologue NILs all slots; no inherited stack bytes survive.
void test_the_frame_starts_out_nil() {
	std::cout << "Testing that a coroutine's unwritten slots are NIL..." << std::endl;

	const Program program = compile(
		"func waits(sig, a, b, c):\n"
		"\tvar x = a + b\n"
		"\tvar got = await sig\n"
		"\treturn x + c + got\n");
	if (program.elf.empty()) {
		return;
	}

	auto machine = boot(program);

	// Poison the stack region the frame will occupy.
	const size_t vsize = size_t(LAYOUT.variant_size());
	const uint64_t retvar = machine->memory.stack_initial() - (64 + 64 * 4);
	std::vector<uint8_t> dirt(8192, 0xFF);
	machine->copy_to_guest(retvar - dirt.size(), dirt.data(), dirt.size());

	run(*machine, "waits", { 0, 1, 2, 3 });
	if (g_host.suspensions.empty()) {
		check(false, "the body suspended");
		return;
	}

	const std::vector<uint8_t>& frame = g_host.suspensions.back().frame;
	int uninitialised = 0;
	for (size_t off = 0; off + vsize <= frame.size(); off += vsize) {
		int32_t type = 0;
		std::memcpy(&type, frame.data() + off + VariantLayout::TYPE_OFFSET, sizeof(type));
		// Body stores only integers; anything else leaked from the caller's stack.
		if (type != int32_t(Variant::NIL) && type != int32_t(Variant::INT)) {
			uninitialised++;
		}
	}
	check_eq<int>(uninitialised, 0, "no slot in the frame came from the caller's stack");

	std::cout << "  ✓ A coroutine's frame holds nothing it did not put there" << std::endl;
}

} // namespace

int main() {
	std::cout << "=== Await Tests ===" << std::endl << std::endl;

	test_one_suspension();
	test_two_suspensions_dispatch();
	test_the_barrier_keeps_locals();
	test_not_awaitable_falls_through();
	test_the_frame_size_is_the_contract();
	test_a_coroutine_always_has_a_frame();
	test_the_signature_says_coroutine();
	test_awaiting_in_a_loop();
	test_an_unused_await_is_kept();
	test_awaiting_another_coroutine();
	test_calling_a_coroutine_without_awaiting_it();
	test_a_global_read_is_not_hoisted_across_a_hosted_call();
	test_an_ordinary_call_stays_a_jal();
	test_the_frame_starts_out_nil();

	if (failures != 0) {
		std::cerr << std::endl << failures << " await test(s) failed" << std::endl;
		return 1;
	}
	std::cout << std::endl << "All await tests passed!" << std::endl;
	return 0;
}
