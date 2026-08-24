// Callables: lambdas, a function name used as a value, and calling a Callable
// held in a variable.
//
// Run on libriscv against a host stub that answers the four syscalls involved,
// because what a lambda compiles to is only pinned by running it: the capture
// contract is that ECALL_CALLABLE_CREATE's bound Variant is *prepended* to
// every call (RiscvCallable::call), which no amount of reading the IR proves.
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

std::vector<uint8_t> compile(const std::string& source) {
	Compiler compiler;
	CompilerOptions options;
	std::vector<uint8_t> elf = compiler.compile(source, options);
	if (elf.empty()) {
		std::cerr << "FAILED to compile: " << compiler.get_error() << std::endl;
		failures++;
	}
	return elf;
}

std::string compile_error(const std::string& source) {
	Compiler compiler;
	CompilerOptions options;
	if (!compiler.compile(source, options).empty()) {
		return "";
	}
	return compiler.get_error();
}

// -= Host stub =-
//
// Variants the guest cannot inline live here and are named by index, the way
// the sandbox's scoped variants are. Only what these tests need is modelled:
// integers, Arrays of them, and Callables.

struct HostVariant {
	int32_t type = Variant::NIL;
	int64_t integer = 0;
	std::vector<HostVariant> array;
	uint64_t address = 0;           // CALLABLE: guest function
	std::vector<HostVariant> bound; // CALLABLE: prepended to every call
};

struct Host {
	std::vector<HostVariant> scoped;
	int callables_created = 0;
	// Argument counts the guest handed to Callable.call(), in order.
	std::vector<int> call_argument_counts;
};

Host g_host;
machine_t* g_machine = nullptr;

unsigned scope(HostVariant value) {
	g_host.scoped.push_back(std::move(value));
	return unsigned(g_host.scoped.size() - 1);
}

HostVariant read_variant(uint64_t address) {
	int32_t type = 0;
	int64_t data = 0;
	g_machine->copy_from_guest(&type, address + VariantLayout::TYPE_OFFSET, sizeof(type));
	g_machine->copy_from_guest(&data, address + VariantLayout::DATA_OFFSET, sizeof(data));

	HostVariant out;
	out.type = type;
	// NIL carries no payload; the data bytes may be uninitialised.
	if (type == Variant::NIL) {
		return out;
	}
	if (type == Variant::INT || type == Variant::BOOL) {
		out.integer = data;
		return out;
	}
	// Everything else the guest holds by index into the scoped list.
	const size_t index = size_t(uint32_t(data));
	if (index < g_host.scoped.size()) {
		out = g_host.scoped[index];
	}
	return out;
}

void write_variant(uint64_t address, const HostVariant& value) {
	int32_t type = value.type;
	int64_t data = value.integer;
	if (type != Variant::INT && type != Variant::BOOL && type != Variant::NIL) {
		data = int64_t(scope(value));
	}
	g_machine->copy_to_guest(address + VariantLayout::TYPE_OFFSET, &type, sizeof(type));
	g_machine->copy_to_guest(address + VariantLayout::DATA_OFFSET, &data, sizeof(data));
}

void write_int(uint64_t address, int64_t value) {
	HostVariant v;
	v.type = Variant::INT;
	v.integer = value;
	write_variant(address, v);
}

// Sandbox ABI: a0 = return Variant pointer, a1.. = argument Variant pointers.
HostVariant call_guest(uint64_t address, const std::vector<HostVariant>& args);

void callable_create_syscall(machine_t& machine) {
	HostVariant callable;
	callable.type = Variant::CALLABLE;
	callable.address = machine.cpu.reg(riscv::REG_ARG0);

	const uint64_t bound_ptr = machine.cpu.reg(riscv::REG_ARG1);
	if (bound_ptr != 0) {
		const HostVariant bound = read_variant(bound_ptr);
		if (bound.type != Variant::NIL) {
			// api_callable_create pushes the one Variant it is given.
			callable.bound.push_back(bound);
		}
	}

	g_host.callables_created++;
	machine.set_result(scope(std::move(callable)));
}

void vcall_syscall(machine_t& machine) {
	const uint64_t object = machine.cpu.reg(riscv::REG_ARG0);
	const uint64_t method_ptr = machine.cpu.reg(riscv::REG_ARG1);
	const unsigned method_len = unsigned(machine.cpu.reg(riscv::REG_ARG2));
	const uint64_t args_ptr = machine.cpu.reg(riscv::REG_ARG3);
	const int argc = int(machine.cpu.reg(riscv::REG_ARG0 + 4));
	const uint64_t result_ptr = machine.cpu.reg(riscv::REG_ARG0 + 5);

	std::string method(method_len, '\0');
	machine.copy_from_guest(method.data(), method_ptr, method_len);

	const HostVariant target = read_variant(object);
	if (target.type != Variant::CALLABLE || method != "call") {
		std::cerr << "FAILED: unexpected vcall '" << method << "' on type "
			<< target.type << std::endl;
		failures++;
		machine.set_result(0);
		return;
	}

	std::vector<HostVariant> args;
	for (int i = 0; i < argc; i++) {
		args.push_back(read_variant(args_ptr + uint64_t(i) * uint64_t(LAYOUT.variant_size())));
	}
	g_host.call_argument_counts.push_back(argc);

	// RiscvCallable::call: the bound arguments come first, the caller's after.
	std::vector<HostVariant> all = target.bound;
	all.insert(all.end(), args.begin(), args.end());

	const HostVariant answer = call_guest(target.address, all);
	if (result_ptr != 0) {
		write_variant(result_ptr, answer);
	}
}

void vcreate_syscall(machine_t& machine) {
	const uint64_t result_ptr = machine.cpu.reg(riscv::REG_ARG0);
	const int32_t type = int32_t(machine.cpu.reg(riscv::REG_ARG1));
	const int64_t count = int64_t(machine.cpu.reg(riscv::REG_ARG2));
	const uint64_t data_ptr = machine.cpu.reg(riscv::REG_ARG3);

	HostVariant value;
	value.type = type;
	if (type == Variant::ARRAY) {
		for (int64_t i = 0; i < count; i++) {
			value.array.push_back(read_variant(data_ptr + uint64_t(i) * uint64_t(LAYOUT.variant_size())));
		}
	}
	write_variant(result_ptr, value);
}

void array_at_syscall(machine_t& machine) {
	const size_t handle = size_t(uint32_t(machine.cpu.reg(riscv::REG_ARG0)));
	const int64_t index = int64_t(machine.cpu.reg(riscv::REG_ARG1));
	const uint64_t value_ptr = machine.cpu.reg(riscv::REG_ARG2);

	if (handle >= g_host.scoped.size()) {
		std::cerr << "FAILED: array handle " << handle << " does not exist" << std::endl;
		failures++;
		return;
	}
	const std::vector<HostVariant>& array = g_host.scoped[handle].array;
	if (index < 0 || size_t(index) >= array.size()) {
		std::cerr << "FAILED: array index " << index << " out of range" << std::endl;
		failures++;
		return;
	}
	write_variant(value_ptr, array[size_t(index)]);
}

void array_size_syscall(machine_t& machine) {
	const size_t handle = size_t(uint32_t(machine.cpu.reg(riscv::REG_ARG0)));
	machine.set_result(handle < g_host.scoped.size()
		? int64_t(g_host.scoped[handle].array.size()) : 0);
}

// ECALL_VEVAL stub: integer-only, enough for untyped-parameter tests.
void veval_syscall(machine_t& machine) {
	enum { OP_EQUAL = 0, OP_NOT_EQUAL, OP_LESS, OP_LESS_EQUAL, OP_GREATER, OP_GREATER_EQUAL,
		OP_ADD, OP_SUBTRACT, OP_MULTIPLY, OP_DIVIDE };

	const int op = int(machine.cpu.reg(riscv::REG_ARG0));
	const HostVariant a = read_variant(machine.cpu.reg(riscv::REG_ARG1));
	const HostVariant b = read_variant(machine.cpu.reg(riscv::REG_ARG0 + 2));
	const uint64_t result_ptr = machine.cpu.reg(riscv::REG_ARG0 + 3);

	const bool integers = (a.type == Variant::INT || a.type == Variant::BOOL) &&
		(b.type == Variant::INT || b.type == Variant::BOOL);
	if (!integers) {
		std::cerr << "FAILED: veval " << op << " on types " << a.type << " and "
			<< b.type << std::endl;
		failures++;
		machine.stop();
		return;
	}

	HostVariant out;
	out.type = Variant::INT;
	switch (op) {
		case OP_ADD:      out.integer = a.integer + b.integer; break;
		case OP_SUBTRACT: out.integer = a.integer - b.integer; break;
		case OP_MULTIPLY: out.integer = a.integer * b.integer; break;
		case OP_DIVIDE:   out.integer = b.integer != 0 ? a.integer / b.integer : 0; break;
		case OP_EQUAL:    out.type = Variant::BOOL; out.integer = a.integer == b.integer; break;
		case OP_NOT_EQUAL: out.type = Variant::BOOL; out.integer = a.integer != b.integer; break;
		case OP_LESS:     out.type = Variant::BOOL; out.integer = a.integer < b.integer; break;
		default:
			std::cerr << "FAILED: veval operator " << op << " is not stubbed" << std::endl;
			failures++;
			machine.stop();
			return;
	}
	machine.set_result(1); // valid
	if (result_ptr != 0) {
		write_variant(result_ptr, out);
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
	for (int number = GAME_API_BASE; number < ECALL_LAST; number++) {
		machine_t::install_syscall_handler(number, fail_on_syscall);
	}
	machine_t::install_syscall_handler(ECALL_CALLABLE_CREATE, callable_create_syscall);
	machine_t::install_syscall_handler(ECALL_VCALL, vcall_syscall);
	machine_t::install_syscall_handler(ECALL_VCREATE, vcreate_syscall);
	machine_t::install_syscall_handler(ECALL_ARRAY_AT, array_at_syscall);
	machine_t::install_syscall_handler(ECALL_ARRAY_SIZE, array_size_syscall);
	machine_t::install_syscall_handler(ECALL_VEVAL, veval_syscall);

	g_host = Host {};
	machine->simulate(50'000'000ull);
	g_machine = machine.get();
	return machine;
}

// A call into the guest from inside a syscall, the way Sandbox::vmcall_internal
// makes one: the register file is snapshotted *before* the call is set up, so
// preempt_internal puts the interrupted run back exactly as it was. Handing
// Machine::preempt() the same job restores the stack pointer this function
// moved, and the outer frame is lost.
HostVariant call_guest(uint64_t address, const std::vector<HostVariant>& args) {
	machine_t& machine = *g_machine;
	riscv::Registers<riscv::RISCV64> interrupted = machine.cpu.registers();

	const uint64_t slot_size = uint64_t(LAYOUT.variant_size());
	uint64_t sp = machine.cpu.reg(riscv::REG_SP);
	if (sp == 0) {
		sp = machine.memory.stack_initial();
	}

	// Argument slots below the interrupted frame, and the nested call's own
	// frames below those: neither can land on the other.
	const uint64_t needed = slot_size * (args.size() + 1);
	const uint64_t retvar = (sp - needed - 64) & ~uint64_t(15);
	for (size_t i = 0; i < args.size(); i++) {
		const uint64_t slot = retvar + slot_size * (i + 1);
		write_variant(slot, args[i]);
		machine.cpu.reg(riscv::REG_ARG1 + i) = slot;
	}
	machine.cpu.reg(riscv::REG_ARG0) = retvar;
	machine.cpu.reg(riscv::REG_RA) = machine.memory.exit_address();
	machine.cpu.reg(riscv::REG_SP) = retvar - 64;

	machine.cpu.preempt_internal(interrupted, true, true, address, 20'000'000ull);

	return read_variant(retvar);
}

struct Call {
	int64_t value = 0;
	int32_t type = 0;
};

Call run(machine_t& machine, const std::string& function, const std::vector<int64_t>& args = {}) {
	const uint64_t address = machine.address_of(function);
	if (address == 0) {
		std::cerr << "FAILED: no symbol for " << function << "()" << std::endl;
		failures++;
		return {};
	}

	const uint64_t slot_size = uint64_t(LAYOUT.variant_size());
	auto& sp = machine.cpu.reg(riscv::REG_SP);
	sp = machine.memory.stack_initial();
	sp -= 256 + slot_size * (args.size() + 1);
	const uint64_t retvar = sp;

	for (size_t i = 0; i < args.size(); i++) {
		const uint64_t slot = retvar + slot_size * (i + 1);
		write_int(slot, args[i]);
		machine.cpu.reg(riscv::REG_ARG1 + i) = slot;
	}

	machine.cpu.reg(riscv::REG_RA) = machine.memory.exit_address();
	machine.cpu.reg(riscv::REG_ARG0) = retvar;
	machine.cpu.jump(address);
	machine.simulate(200'000'000ull);

	Call out;
	int32_t type = 0;
	int64_t data = 0;
	machine.copy_from_guest(&type, retvar + VariantLayout::TYPE_OFFSET, sizeof(type));
	machine.copy_from_guest(&data, retvar + VariantLayout::DATA_OFFSET, sizeof(data));
	out.type = type;
	out.value = data;
	return out;
}

// -= Tests =-

void test_a_lambda_is_a_callable() {
	std::cout << "Testing that a lambda evaluates to a Callable..." << std::endl;

	const std::vector<uint8_t> elf = compile(
		"func f():\n"
		"\treturn func(x): return x * 2\n");
	if (elf.empty()) {
		return;
	}

	auto machine = boot(elf);
	const Call answer = run(*machine, "f");

	check_eq<int32_t>(answer.type, Variant::CALLABLE, "f() answers a Callable");
	check_eq(g_host.callables_created, 1, "one Callable created");
	if (!g_host.scoped.empty()) {
		check(g_host.scoped.back().bound.empty(), "a lambda with no captures binds nothing");
		check(g_host.scoped.back().address == machine->address_of("@lambda_0"),
			"the Callable points at the lifted function");
	}

	std::cout << "  ✓ A lambda is a Callable over a lifted function" << std::endl;
}

void test_calling_a_lambda() {
	std::cout << "Testing a lambda called through the host..." << std::endl;

	const std::vector<uint8_t> elf = compile(
		"func f(n):\n"
		"\tvar double = func(x): return x * 2\n"
		"\treturn double.call(n)\n");
	if (elf.empty()) {
		return;
	}

	auto machine = boot(elf);
	const Call answer = run(*machine, "f", { 21 });

	check_eq<int32_t>(answer.type, Variant::INT, "the answer is an integer");
	check_eq<int64_t>(answer.value, 42, "the lambda ran");

	std::cout << "  ✓ A lambda runs when the host calls it" << std::endl;
}

void test_captures_arrive_prepended() {
	std::cout << "Testing that captures arrive before the arguments..." << std::endl;

	const std::vector<uint8_t> elf = compile(
		"func f(x):\n"
		"\tvar scale = 10\n"
		"\tvar offset = 3\n"
		"\tvar g = func(v): return v * scale + offset\n"
		"\treturn g.call(x)\n");
	if (elf.empty()) {
		return;
	}

	auto machine = boot(elf);
	const Call answer = run(*machine, "f", { 4 });

	check_eq<int64_t>(answer.value, 43, "the lambda saw both captures in order");
	check_eq<size_t>(g_host.call_argument_counts.size(), 1, "one call through the host");
	if (!g_host.call_argument_counts.empty()) {
		// The captures travel bound, not as call arguments: the guest passes one.
		check_eq(g_host.call_argument_counts.front(), 1,
			"captures are bound, not passed at the call");
	}

	std::cout << "  ✓ Captures travel bound, and land before the parameters" << std::endl;
}

void test_captures_are_by_value() {
	std::cout << "Testing that a capture is the value at creation..." << std::endl;

	// GDScript's rule, checked against the engine: assigning to the captured
	// local after the lambda exists does not change what the lambda sees, and
	// assigning to it inside the lambda does not reach the outer one.
	const std::vector<uint8_t> elf = compile(
		"func f():\n"
		"\tvar n = 1\n"
		"\tvar g = func(): return n\n"
		"\tn = 50\n"
		"\treturn g.call() * 100 + n\n");
	if (elf.empty()) {
		return;
	}

	auto machine = boot(elf);
	const Call answer = run(*machine, "f");

	check_eq<int64_t>(answer.value, 150, "the lambda kept the value it was built with");

	const std::vector<uint8_t> written = compile(
		"func f():\n"
		"\tvar n = 1\n"
		"\tvar g = func():\n"
		"\t\tn = 9\n"
		"\t\treturn n\n"
		"\tvar inner = g.call()\n"
		"\treturn inner * 100 + n\n");
	if (written.empty()) {
		return;
	}

	auto second = boot(written);
	const Call after = run(*second, "f");
	check_eq<int64_t>(after.value, 901, "a write inside the lambda stayed inside it");

	std::cout << "  ✓ Captures are by value, at the point the lambda is built" << std::endl;
}

void test_a_function_name_is_a_callable() {
	std::cout << "Testing a function name used as a value..." << std::endl;

	const std::vector<uint8_t> elf = compile(
		"func helper(a):\n"
		"\treturn a + 1\n"
		"func f():\n"
		"\tvar c = helper\n"
		"\treturn c.call(41)\n");
	if (elf.empty()) {
		return;
	}

	auto machine = boot(elf);
	const Call answer = run(*machine, "f");

	check_eq<int64_t>(answer.value, 42, "the named function ran");
	check_eq(g_host.callables_created, 1, "one Callable created");
	if (!g_host.scoped.empty()) {
		check(g_host.scoped.front().address == machine->address_of("helper"),
			"the Callable points at helper()");
		check(g_host.scoped.front().bound.empty(), "nothing is bound to a plain function");
	}

	std::cout << "  ✓ A function name is a Callable over that function" << std::endl;
}

void test_calling_a_callable_variable() {
	std::cout << "Testing a Callable called through a variable..." << std::endl;

	// Ours, not GDScript's: the engine refuses c(1) and insists on c.call(1).
	const std::vector<uint8_t> elf = compile(
		"func helper(a, b):\n"
		"\treturn a * b\n"
		"func f():\n"
		"\tvar c = helper\n"
		"\treturn c(6, 7)\n");
	if (elf.empty()) {
		return;
	}

	auto machine = boot(elf);
	const Call answer = run(*machine, "f");

	check_eq<int64_t>(answer.value, 42, "the variable was called");
	check_eq<size_t>(g_host.call_argument_counts.size(), 1, "one call through the host");

	std::cout << "  ✓ A variable holding a Callable can be called directly" << std::endl;
}

void test_a_lambda_called_through_its_variable() {
	std::cout << "Testing a lambda called through its variable..." << std::endl;

	const std::vector<uint8_t> elf = compile(
		"func f(n):\n"
		"\tvar base = 100\n"
		"\tvar add = func(v): return v + base\n"
		"\treturn add(n)\n");
	if (elf.empty()) {
		return;
	}

	auto machine = boot(elf);
	const Call answer = run(*machine, "f", { 5 });

	check_eq<int64_t>(answer.value, 105, "the lambda ran with its capture");

	std::cout << "  ✓ A captured lambda answers to f(x) as well as f.call(x)" << std::endl;
}

// `a[0]()` / `get_f()()`: lowered as `.call()`, same as `c(1)`.
void test_calling_an_expression() {
	std::cout << "Testing a call on an expression..." << std::endl;

	const std::vector<uint8_t> elf = compile(
		"func double(x):\n"
		"\treturn x * 2\n"
		"func get_f():\n"
		"\treturn double\n"
		"func from_array(n):\n"
		"\tvar a = [double]\n"
		"\treturn a[0](n)\n"
		"func from_call(n):\n"
		"\treturn get_f()(n)\n"
		"func from_lambda(n):\n"
		"\tvar a = [func(x): return x + 1]\n"
		"\treturn a[0](n)\n"
		"func chained(n):\n"
		"\treturn get_f()(get_f()(n))\n");
	if (elf.empty()) {
		return;
	}

	auto machine = boot(elf);
	check_eq<int64_t>(run(*machine, "from_array", { 4 }).value, 8, "a[0](n) called the element");
	check_eq<int64_t>(run(*machine, "from_call", { 5 }).value, 10, "get_f()(n) called the result");
	check_eq<int64_t>(run(*machine, "from_lambda", { 6 }).value, 7, "a lambda in an Array is callable");
	check_eq<int64_t>(run(*machine, "chained", { 3 }).value, 12, "two calls on expressions compose");

	// Must produce identical ELF to the explicit `.call()` spelling.
	const std::vector<uint8_t> spelled_out = compile(
		"func double(x):\n"
		"\treturn x * 2\n"
		"func get_f():\n"
		"\treturn double\n"
		"func from_array(n):\n"
		"\tvar a = [double]\n"
		"\treturn a[0].call(n)\n"
		"func from_call(n):\n"
		"\treturn get_f().call(n)\n"
		"func from_lambda(n):\n"
		"\tvar a = [func(x): return x + 1]\n"
		"\treturn a[0].call(n)\n"
		"func chained(n):\n"
		"\treturn get_f().call(get_f().call(n))\n");
	check(spelled_out == elf, "f(x) and f.call(x) on an expression compile alike");

	// Named arguments have no meaning on a Callable.
	check(!compile_error("func f(a):\n\treturn a[0](x = 1)\n").empty(),
		"a named argument in a call on an expression is refused");

	std::cout << "  ✓ A call on an expression is the .call() it stands for" << std::endl;
}

void test_a_lambda_inside_a_lambda() {
	std::cout << "Testing a lambda inside a lambda..." << std::endl;

	// `n` is free in the inner lambda, so it has to be captured by the outer
	// one too: the inner can only reach it through the outer's frame.
	const std::vector<uint8_t> elf = compile(
		"func f():\n"
		"\tvar n = 7\n"
		"\tvar outer = func(x):\n"
		"\t\tvar inner = func(y): return y + n\n"
		"\t\treturn inner.call(x)\n"
		"\treturn outer.call(5)\n");
	if (elf.empty()) {
		return;
	}

	auto machine = boot(elf);
	const Call answer = run(*machine, "f");

	check_eq<int64_t>(answer.value, 12, "the inner lambda saw the outer function's local");
	check_eq(g_host.callables_created, 2, "two Callables, one per lambda");

	std::cout << "  ✓ A nested lambda captures through the lambda around it" << std::endl;
}

void test_shapes_of_lambda_syntax() {
	std::cout << "Testing the shapes a lambda is written in..." << std::endl;

	// Inline inside an argument list: the lexer emits no newline inside
	// brackets, so the body has to end at the bracket.
	check(compile_error("func f(a):\n\treturn a.map(func(x): return x * 2)\n").empty(),
		"a lambda inline in an argument list");
	check(compile_error("func f(a):\n\treturn a.map(func(x): return x, 1)\n").empty(),
		"a lambda before a further argument");
	check(compile_error("func f():\n\tvar g = func(x): var y = x; return y\n\treturn g.call(1)\n").empty(),
		"statements separated by ';' in a one-line body");
	check(compile_error("func f():\n\tvar g = func(x):\n\t\treturn x\n\treturn g.call(1)\n").empty(),
		"an indented body");
	check(compile_error("func f():\n\tvar g = func named(x): return x\n\treturn g.call(1)\n").empty(),
		"a named lambda");
	check(compile_error("func f():\n\tvar g = func(): return 1\n\treturn g.call()\n").empty(),
		"no parameters");

	// The statement after a one-line lambda is still a statement.
	const std::vector<uint8_t> elf = compile(
		"func f():\n"
		"\tvar g = func(x): return x + 1\n"
		"\tvar n = 10\n"
		"\treturn g.call(n)\n");
	if (!elf.empty()) {
		auto machine = boot(elf);
		check_eq<int64_t>(run(*machine, "f").value, 11, "the line after the lambda ran");
	}

	std::cout << "  ✓ Every spelling of a lambda parses" << std::endl;
}

void test_the_callable_constructor() {
	std::cout << "Testing Callable(self, \"name\")..." << std::endl;

	const std::vector<uint8_t> constructed = compile(
		"func double(x):\n"
		"\treturn x * 2\n"
		"func f(n):\n"
		"\tvar c = Callable(self, \"double\")\n"
		"\treturn c.call(n)\n");
	const std::vector<uint8_t> bare_name = compile(
		"func double(x):\n"
		"\treturn x * 2\n"
		"func f(n):\n"
		"\tvar c = double\n"
		"\treturn c.call(n)\n");
	check(!constructed.empty() && constructed == bare_name,
		"Callable(self, \"f\") and f compile alike");

	if (!constructed.empty()) {
		auto machine = boot(constructed);
		check_eq<int64_t>(run(*machine, "f", { 21 }).value, 42, "the constructed Callable called f");
	}

	const std::vector<uint8_t> empty = compile(
		"func f():\n"
		"\treturn Callable()\n");
	if (!empty.empty()) {
		auto machine = boot(empty);
		run(*machine, "f");
		check_eq(g_host.callables_created, 1, "one Callable created");
		check(!g_host.scoped.empty() && g_host.scoped.back().type == Variant::CALLABLE,
			"Callable() is a CALLABLE");
		check(!g_host.scoped.empty() && g_host.scoped.back().address == 0,
			"Callable() names no guest function");
	}

	const std::string missing = compile_error(
		"func f():\n"
		"\treturn Callable(self, \"nope\")\n");
	check(missing.find("no function named") != std::string::npos,
		"an undeclared method name is refused: " + missing);

	const std::string not_self = compile_error(
		"func f(n):\n"
		"\treturn Callable(n, \"f\")\n");
	check(not_self.find("must be 'self'") != std::string::npos,
		"a Callable over another object is refused: " + not_self);

	const std::string computed = compile_error(
		"func f(n):\n"
		"\treturn Callable(self, n)\n");
	check(computed.find("compile-time string") != std::string::npos,
		"a run-time method name is refused: " + computed);

	const std::string arity = compile_error(
		"func f():\n"
		"\treturn Callable(self)\n");
	check(arity.find("0 or 2 arguments") != std::string::npos,
		"Callable(self) is refused: " + arity);

	check(compile_error(
		"func Callable(a, b):\n"
		"\treturn a\n"
		"func f():\n"
		"\treturn Callable(1, 2)\n").empty(),
		"a script function named Callable wins over the constructor");

	std::cout << "  \u2713 Callable(self, \"name\") is the name it stands for" << std::endl;
}

void test_what_is_refused() {
	std::cout << "Testing the refusals..." << std::endl;

	// A parameter slot is spoken for by the captures, so the arity is one less.
	const std::string too_many = compile_error(
		"func f():\n"
		"\tvar n = 1\n"
		"\tvar g = func(a, b, c, d, e, f, h): return a + n\n"
		"\treturn g\n");
	check(too_many.find("parameter slots") != std::string::npos,
		"a lambda that needs more slots than the ABI has is refused: " + too_many);

	// A variable of a type that cannot be called says so at compile time,
	// rather than reaching the host with a Variant it will refuse.
	const std::string not_callable = compile_error(
		"func f():\n"
		"\tvar n = 1\n"
		"\treturn n()\n");
	check(not_callable.find("cannot be called") != std::string::npos,
		"calling an integer variable is refused: " + not_callable);

	std::cout << "  ✓ What cannot work is refused at compile time" << std::endl;
}

void test_resolution_order() {
	std::cout << "Testing what a bare name in call position reaches..." << std::endl;

	// A script function of the same name wins over a variable: this is a call
	// to helper(), not a call of the Callable in `helper`.
	const std::vector<uint8_t> elf = compile(
		"func helper():\n"
		"\treturn 7\n"
		"func f():\n"
		"\tvar helper = 1\n"
		"\treturn helper()\n");
	if (!elf.empty()) {
		auto machine = boot(elf);
		check_eq<int64_t>(run(*machine, "f").value, 7, "the script function won");
		check_eq(g_host.callables_created, 0, "no Callable was made");
	}

	// And @GlobalScope wins over a local: a variable named `abs` does not
	// capture abs().
	const std::vector<uint8_t> global = compile(
		"func f():\n"
		"\tvar abs = 1\n"
		"\treturn abs(-5)\n");
	if (!global.empty()) {
		auto machine = boot(global);
		check_eq<int64_t>(run(*machine, "f").value, 5, "the global function won");
	}

	std::cout << "  ✓ Script functions and globals still win in call position" << std::endl;
}

void test_lifted_names_stay_out_of_the_way() {
	std::cout << "Testing the names lifted lambdas get..." << std::endl;

	Compiler compiler;
	CompilerOptions options;
	const std::vector<uint8_t> elf = compiler.compile(
		"func f():\n"
		"\tvar a = func(): return 1\n"
		"\tvar b = func(): return 2\n"
		"\treturn a.call() + b.call()\n", options);
	if (elf.empty()) {
		std::cerr << "FAILED to compile: " << compiler.get_error() << std::endl;
		failures++;
		return;
	}

	// One signature per function, in function order: backtraces index this list
	// by position, so a lifted lambda has to have an entry of its own.
	const std::vector<FunctionSignature>& signatures = compiler.get_function_signatures();
	check_eq<size_t>(signatures.size(), 3, "one signature per function, lambdas included");
	if (signatures.size() == 3) {
		check(signatures[0].name == "f", "the declared function comes first");
		check(signatures[1].name.rfind("@lambda_", 0) == 0,
			"a lifted lambda is named @lambda_N: " + signatures[1].name);
		check(signatures[1].name != signatures[2].name, "two lambdas get two names");
	}

	std::cout << "  ✓ Lifted lambdas are named where no GDScript identifier can reach" << std::endl;
}

} // namespace

int main() {
	std::cout << "=== Callable Tests ===" << std::endl << std::endl;

	test_a_lambda_is_a_callable();
	test_calling_a_lambda();
	test_captures_arrive_prepended();
	test_captures_are_by_value();
	test_a_function_name_is_a_callable();
	test_calling_a_callable_variable();
	test_a_lambda_called_through_its_variable();
	test_calling_an_expression();
	test_a_lambda_inside_a_lambda();
	test_shapes_of_lambda_syntax();
	test_the_callable_constructor();
	test_what_is_refused();
	test_resolution_order();
	test_lifted_names_stay_out_of_the_way();

	if (failures != 0) {
		std::cerr << std::endl << failures << " callable test(s) failed" << std::endl;
		return 1;
	}
	std::cout << std::endl << "All callable tests passed!" << std::endl;
	return 0;
}
