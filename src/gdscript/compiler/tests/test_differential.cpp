// Differential testing: the IR interpreter against the real machine.
//
// The IR interpreter and the RISC-V backend are two independent
// implementations of the same language, and until this existed nothing
// compared them. test_primitives ran the interpreter, the Godot tests ran the
// backend, and the two corpora barely overlapped -- which is why `not x`
// returned false at run time while `test_primitives` asserted `not 0 == 1` and
// passed.
//
// This runs every corpus program both ways -- through the IR interpreter, and
// through a bare libriscv machine over the ELF the compiler produced -- and
// requires the two to agree.
//
// The machine has a minimal shim for the one host call the generated code
// cannot avoid: Variant::evaluate(), which is how every untyped arithmetic
// operation and comparison is performed. The shim covers the Variant types the
// interpreter can represent (NIL, BOOL, INT, FLOAT). Anything else -- strings,
// arrays, objects, property access -- needs the real host, and the harness
// skips those programs with the syscall named, rather than silently passing.
//
// Modes:
//   (no arguments)          the shared corpus; this is what ctest runs
//   --fuzz [--seed --count] generated programs, with shrinking on failure
//   --file program.gd       one program, for reducing a failure by hand
//
// Two environment variables help when reducing:
//   GDSC_PASSES=<list>      which optimizer passes to run (see IROptimizer)
//   GDSC_DIFF_NO_OPT=1      skip the optimizer entirely
//   GDSC_DIFF_DEBUG=1       print the return Variant's address and payload
#include "../compiler.h"
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../elf_builder.h"
#include "../ir_interpreter.h"
#include "../globals.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../variant_layout.h"
#include "gdscript_generator.h"
#include "scope_stub.h"
#include "test_corpus.h"
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <libriscv/machine.hpp>

using namespace gdscript;
using machine_t = riscv::Machine<riscv::RISCV64>;

namespace {

// -= Godot's Variant, as the guest sees it =-

// Variant::Operator, the numbers Variant::evaluate() is indexed by.
enum VariantOperator : int {
	OP_EQUAL = 0,
	OP_NOT_EQUAL = 1,
	OP_LESS = 2,
	OP_LESS_EQUAL = 3,
	OP_GREATER = 4,
	OP_GREATER_EQUAL = 5,
	OP_ADD = 6,
	OP_SUBTRACT = 7,
	OP_MULTIPLY = 8,
	OP_DIVIDE = 9,
	OP_NEGATE = 10,
	OP_POSITIVE = 11,
	OP_MODULE = 12,
	OP_POWER = 13,
	OP_SHIFT_LEFT = 14,
	OP_SHIFT_RIGHT = 15,
	OP_BIT_AND = 16,
	OP_BIT_OR = 17,
	OP_BIT_XOR = 18,
	OP_BIT_NEGATE = 19,
	OP_AND = 20,
	OP_OR = 21,
	OP_XOR = 22,
	OP_NOT = 23,
};

// The syscall numbers the generated code can make. Only VEVAL is implemented;
// the rest are named so that a skip says which host call was needed.
struct SyscallName {
	int number;
	const char* name;
};

const SyscallName SYSCALL_NAMES[] = {
	{ 500, "ECALL_PRINT" },
	{ 501, "ECALL_VCALL" },
	{ 502, "ECALL_VEVAL" },
	{ 503, "ECALL_VASSIGN" },
	{ 504, "ECALL_GET_OBJ" },
	{ 507, "ECALL_GET_NODE" },
	{ 517, "ECALL_VCREATE" },
	{ 522, "ECALL_ARRAY_AT" },
	{ 523, "ECALL_ARRAY_SIZE" },
	{ 545, "ECALL_OBJ_PROP_GET" },
	{ 546, "ECALL_OBJ_PROP_SET" },
	{ 547, "ECALL_SANDBOX_ADD" },
	{ 548, "ECALL_PACKED_ARRAY_OPS" },
	{ 549, "ECALL_UTILITY" },
};

const char* syscall_name(int number) {
	for (const auto& entry : SYSCALL_NAMES) {
		if (entry.number == number) {
			return entry.name;
		}
	}
	return "an unknown syscall";
}

// A Variant read out of guest memory, in the subset the interpreter shares.
struct GuestValue {
	int32_t type = Variant::NIL;
	int64_t as_int = 0;
	double as_float = 0.0;
};

// Everything the run needs to carry through the syscall handlers, hung off the
// machine's userdata.
struct RunState {
	VariantLayout layout;
	// Set when the guest asked for something this shim cannot do. The run stops
	// and the program is skipped, naming this.
	std::string unsupported;
};

GuestValue read_variant(machine_t& machine, uint64_t address, const VariantLayout& layout) {
	GuestValue value;
	std::vector<uint8_t> bytes(static_cast<size_t>(layout.variant_size()));
	machine.copy_from_guest(bytes.data(), address, bytes.size());

	std::memcpy(&value.type, bytes.data() + VariantLayout::TYPE_OFFSET, sizeof(int32_t));
	std::memcpy(&value.as_int, bytes.data() + VariantLayout::DATA_OFFSET, sizeof(int64_t));
	std::memcpy(&value.as_float, bytes.data() + VariantLayout::DATA_OFFSET, sizeof(double));

	// A BOOL Variant is one byte. Godot reads only that byte, and so does the
	// generated code, so the seven above it are whatever the slot held before
	// -- reading all eight makes a false read as true.
	if (value.type == Variant::BOOL) {
		value.as_int &= 0xFF;
	}
	return value;
}

void write_variant(machine_t& machine, uint64_t address, const VariantLayout& layout,
	int32_t type, int64_t payload)
{
	std::vector<uint8_t> bytes(static_cast<size_t>(layout.variant_size()), 0);
	std::memcpy(bytes.data() + VariantLayout::TYPE_OFFSET, &type, sizeof(int32_t));
	std::memcpy(bytes.data() + VariantLayout::DATA_OFFSET, &payload, sizeof(int64_t));
	machine.copy_to_guest(address, bytes.data(), bytes.size());
}

bool is_numeric(int32_t type) {
	return type == Variant::BOOL || type == Variant::INT || type == Variant::FLOAT;
}

bool booleanize(const GuestValue& value) {
	switch (value.type) {
		case Variant::NIL: return false;
		case Variant::BOOL:
		case Variant::INT: return value.as_int != 0;
		case Variant::FLOAT: return value.as_float != 0.0;
		default: return true;
	}
}

int64_t to_int(const GuestValue& value) {
	if (value.type == Variant::FLOAT) {
		return static_cast<int64_t>(value.as_float);
	}
	return value.as_int;
}

double to_float(const GuestValue& value) {
	if (value.type == Variant::FLOAT) {
		return value.as_float;
	}
	return static_cast<double>(value.as_int);
}

// Variant::evaluate() for the types the interpreter can represent. Returns
// false for an operation Godot does not define on these types, which is what
// evaluate() reports through its `valid` out-parameter.
bool evaluate(int op, const GuestValue& lhs, const GuestValue& rhs,
	int32_t& result_type, int64_t& result_payload)
{
	auto set_bool = [&](bool value) {
		result_type = Variant::BOOL;
		result_payload = value ? 1 : 0;
	};
	auto set_int = [&](int64_t value) {
		result_type = Variant::INT;
		result_payload = value;
	};
	auto set_float = [&](double value) {
		result_type = Variant::FLOAT;
		std::memcpy(&result_payload, &value, sizeof(double));
	};

	// The unary operators are registered against a NIL right-hand side.
	switch (op) {
		case OP_NOT:
			set_bool(!booleanize(lhs));
			return true;
		case OP_NEGATE:
			if (lhs.type == Variant::FLOAT) {
				set_float(-lhs.as_float);
			} else if (lhs.type == Variant::INT || lhs.type == Variant::BOOL) {
				set_int(-lhs.as_int);
			} else {
				return false;
			}
			return true;
		case OP_POSITIVE:
			result_type = lhs.type;
			result_payload = lhs.as_int;
			return true;
		case OP_BIT_NEGATE:
			if (lhs.type != Variant::INT && lhs.type != Variant::BOOL) {
				return false;
			}
			set_int(~lhs.as_int);
			return true;
		case OP_AND:
			set_bool(booleanize(lhs) && booleanize(rhs));
			return true;
		case OP_OR:
			set_bool(booleanize(lhs) || booleanize(rhs));
			return true;
		case OP_XOR:
			set_bool(booleanize(lhs) != booleanize(rhs));
			return true;
		default:
			break;
	}

	// Equality against NIL is defined for every type; ordering is not.
	if (lhs.type == Variant::NIL || rhs.type == Variant::NIL) {
		if (op == OP_EQUAL) {
			set_bool(lhs.type == rhs.type);
			return true;
		}
		if (op == OP_NOT_EQUAL) {
			set_bool(lhs.type != rhs.type);
			return true;
		}
		return false;
	}

	if (!is_numeric(lhs.type) || !is_numeric(rhs.type)) {
		return false;
	}

	const bool float_op = lhs.type == Variant::FLOAT || rhs.type == Variant::FLOAT;

	switch (op) {
		case OP_EQUAL:
			set_bool(float_op ? to_float(lhs) == to_float(rhs) : to_int(lhs) == to_int(rhs));
			return true;
		case OP_NOT_EQUAL:
			set_bool(float_op ? to_float(lhs) != to_float(rhs) : to_int(lhs) != to_int(rhs));
			return true;
		case OP_LESS:
			set_bool(float_op ? to_float(lhs) < to_float(rhs) : to_int(lhs) < to_int(rhs));
			return true;
		case OP_LESS_EQUAL:
			set_bool(float_op ? to_float(lhs) <= to_float(rhs) : to_int(lhs) <= to_int(rhs));
			return true;
		case OP_GREATER:
			set_bool(float_op ? to_float(lhs) > to_float(rhs) : to_int(lhs) > to_int(rhs));
			return true;
		case OP_GREATER_EQUAL:
			set_bool(float_op ? to_float(lhs) >= to_float(rhs) : to_int(lhs) >= to_int(rhs));
			return true;
		case OP_ADD:
			if (float_op) set_float(to_float(lhs) + to_float(rhs));
			else set_int(to_int(lhs) + to_int(rhs));
			return true;
		case OP_SUBTRACT:
			if (float_op) set_float(to_float(lhs) - to_float(rhs));
			else set_int(to_int(lhs) - to_int(rhs));
			return true;
		case OP_MULTIPLY:
			if (float_op) set_float(to_float(lhs) * to_float(rhs));
			else set_int(to_int(lhs) * to_int(rhs));
			return true;
		case OP_DIVIDE:
			if (float_op) {
				set_float(to_float(lhs) / to_float(rhs));
			} else {
				// Godot reports an error and yields 0 for integer division by
				// zero rather than trapping.
				const int64_t divisor = to_int(rhs);
				set_int(divisor != 0 ? to_int(lhs) / divisor : 0);
			}
			return true;
		case OP_MODULE: {
			if (float_op) {
				set_float(std::fmod(to_float(lhs), to_float(rhs)));
			} else {
				const int64_t divisor = to_int(rhs);
				set_int(divisor != 0 ? to_int(lhs) % divisor : 0);
			}
			return true;
		}
		case OP_SHIFT_LEFT:
			if (float_op) return false;
			set_int(static_cast<int64_t>(static_cast<uint64_t>(to_int(lhs)) << (to_int(rhs) & 63)));
			return true;
		case OP_SHIFT_RIGHT:
			if (float_op) return false;
			set_int(to_int(lhs) >> (to_int(rhs) & 63));
			return true;
		case OP_BIT_AND:
			if (float_op) return false;
			set_int(to_int(lhs) & to_int(rhs));
			return true;
		case OP_BIT_OR:
			if (float_op) return false;
			set_int(to_int(lhs) | to_int(rhs));
			return true;
		case OP_BIT_XOR:
			if (float_op) return false;
			set_int(to_int(lhs) ^ to_int(rhs));
			return true;
		default:
			return false;
	}
}

// -= The syscall shim =-

void syscall_veval(machine_t& machine) {
	RunState& state = *machine.get_userdata<RunState>();

	const int op = static_cast<int>(machine.cpu.reg(riscv::REG_ARG0));
	const uint64_t lhs_addr = machine.cpu.reg(riscv::REG_ARG1);
	const uint64_t rhs_addr = machine.cpu.reg(riscv::REG_ARG2);
	const uint64_t result_addr = machine.cpu.reg(riscv::REG_ARG3);

	const GuestValue lhs = read_variant(machine, lhs_addr, state.layout);
	const GuestValue rhs = read_variant(machine, rhs_addr, state.layout);

	if (!is_numeric(lhs.type) && lhs.type != Variant::NIL) {
		state.unsupported = std::string("Variant::evaluate() on a ") + variant_type_name(lhs.type);
		machine.stop();
		return;
	}
	if (!is_numeric(rhs.type) && rhs.type != Variant::NIL) {
		state.unsupported = std::string("Variant::evaluate() on a ") + variant_type_name(rhs.type);
		machine.stop();
		return;
	}

	int32_t result_type = Variant::NIL;
	int64_t result_payload = 0;
	const bool valid = evaluate(op, lhs, rhs, result_type, result_payload);

	write_variant(machine, result_addr, state.layout, result_type, result_payload);
	machine.cpu.reg(riscv::REG_ARG0) = valid ? 1 : 0;
}

// ECALL_UTILITY, the one host call a global function makes. The floating-point
// ops are evaluated by globals.cpp -- the same code the IR interpreter runs --
// so what this compares is the emitted code around the call: the conversions
// into fa0-fa7, the op number, and what the answer is written back as. The real
// host implements the same formulas against Godot's Math::.
//
// str() and len() are not here: they need Variants this harness cannot make.
void syscall_utility(machine_t& machine) {
	RunState& state = *machine.get_userdata<RunState>();

	const int op = static_cast<int>(machine.cpu.reg(riscv::REG_ARG0));
	if (op == gdscript::UTILITY_STR || op == gdscript::UTILITY_LEN ||
	    op == gdscript::UTILITY_RAND_FROM_SEED) {
		state.unsupported = "str() or len(), which need the host Variant API";
		machine.stop();
		return;
	}

	double args[gdscript::UTILITY_MAX_FLOAT_ARGS];
	for (size_t i = 0; i < gdscript::UTILITY_MAX_FLOAT_ARGS; i++) {
		args[i] = machine.cpu.registers().getfl(riscv::REG_FA0 + i).f64;
	}

	double result = 0.0;
	try {
		result = gdscript::eval_utility_op(static_cast<int16_t>(op), args);
	} catch (const std::exception& e) {
		state.unsupported = std::string("ECALL_UTILITY op ") + std::to_string(op) + ": " + e.what();
		machine.stop();
		return;
	}
	machine.cpu.registers().getfl(riscv::REG_FA0).set_double(result);
}

// Every other host call: stop and say which one, so a skipped program says why.
template <int Number>
void syscall_unsupported(machine_t& machine) {
	RunState& state = *machine.get_userdata<RunState>();
	state.unsupported = syscall_name(Number);
	machine.stop();
}

// -= Running a program =-

struct Outcome {
	enum class Kind { AGREED, DISAGREED, SKIPPED, FAILED };
	Kind kind = Kind::AGREED;
	std::string detail;
};

// The IR interpreter's answer, expressed the way the machine reports one.
GuestValue interpreter_value(const IRInterpreter::Value& value) {
	GuestValue result;
	if (std::holds_alternative<std::monostate>(value)) {
		result.type = Variant::NIL;
	} else if (std::holds_alternative<bool>(value)) {
		result.type = Variant::BOOL;
		result.as_int = std::get<bool>(value) ? 1 : 0;
	} else if (std::holds_alternative<int64_t>(value)) {
		result.type = Variant::INT;
		result.as_int = std::get<int64_t>(value);
	} else if (std::holds_alternative<double>(value)) {
		result.type = Variant::FLOAT;
		result.as_float = std::get<double>(value);
	} else {
		result.type = Variant::STRING;
	}
	return result;
}

std::string describe(const GuestValue& value) {
	switch (value.type) {
		case Variant::BOOL: return std::string(value.as_int ? "true" : "false") + " (BOOL)";
		case Variant::INT: return std::to_string(value.as_int) + " (INT)";
		case Variant::FLOAT: return std::to_string(value.as_float) + " (FLOAT)";
		default: return std::string(variant_type_name(value.type));
	}
}

// BOOL and INT are the same answer: the interpreter is free to produce either
// for a truth value, and so is the backend. FLOAT against INT is a real
// difference -- GDScript keeps them apart.
bool agrees(const GuestValue& a, const GuestValue& b) {
	const bool a_float = a.type == Variant::FLOAT;
	const bool b_float = b.type == Variant::FLOAT;
	if (a_float != b_float) {
		return false;
	}
	if (a_float) {
		return a.as_float == b.as_float || (std::isnan(a.as_float) && std::isnan(b.as_float));
	}
	if (!is_numeric(a.type) || !is_numeric(b.type)) {
		return a.type == b.type;
	}
	return a.as_int == b.as_int;
}

Outcome run_source(const std::string& source) {
	Outcome outcome;

	const VariantLayout layout = native_variant_layout();

	// Compile once, and use the same IR for both runs.
	IRProgram ir;
	std::vector<uint8_t> elf;
	try {
		Lexer lexer(source);
		Parser parser(lexer.tokenize());
		Program parsed = parser.parse();
		CodeGenerator codegen;
		ir = codegen.generate(parsed);
		if (std::getenv("GDSC_DIFF_NO_OPT") == nullptr) {
			IROptimizer optimizer;
			optimizer.optimize(ir);
		}

		ElfBuilder builder;
		elf = builder.build(ir, layout);
	} catch (const std::exception& e) {
		outcome.kind = Outcome::Kind::FAILED;
		outcome.detail = std::string("compilation failed: ") + e.what();
		return outcome;
	}

	// The interpreter's answer.
	GuestValue expected;
	try {
		IRInterpreter interpreter(ir);
		expected = interpreter_value(interpreter.call("test"));
	} catch (const std::exception& e) {
		outcome.kind = Outcome::Kind::SKIPPED;
		outcome.detail = std::string("the IR interpreter cannot run it: ") + e.what();
		return outcome;
	}
	if (expected.type == Variant::STRING) {
		outcome.kind = Outcome::Kind::SKIPPED;
		outcome.detail = "returns a String, which needs the host Variant API";
		return outcome;
	}

	// The machine's answer.
	RunState state { layout, "" };
	try {
		machine_t machine { elf, riscv::MachineOptions<riscv::RISCV64> {
			.memory_max = 16ull << 20,
			.stack_size = 1ull << 20,
		} };
		machine.set_userdata(&state);

		machine_t::install_syscall_handler(502, syscall_veval);
		machine_t::install_syscall_handler(549, syscall_utility);
		install_scope_stub<machine_t>();
		machine_t::install_syscall_handler(500, syscall_unsupported<500>);
		machine_t::install_syscall_handler(501, syscall_unsupported<501>);
		machine_t::install_syscall_handler(503, syscall_unsupported<503>);
		machine_t::install_syscall_handler(504, syscall_unsupported<504>);
		machine_t::install_syscall_handler(507, syscall_unsupported<507>);
		machine_t::install_syscall_handler(517, syscall_unsupported<517>);
		machine_t::install_syscall_handler(522, syscall_unsupported<522>);
		machine_t::install_syscall_handler(523, syscall_unsupported<523>);
		machine_t::install_syscall_handler(545, syscall_unsupported<545>);
		machine_t::install_syscall_handler(546, syscall_unsupported<546>);
		machine_t::install_syscall_handler(547, syscall_unsupported<547>);
		machine_t::install_syscall_handler(548, syscall_unsupported<548>);

		// The entry point initializes the globals and then stops.
		machine.simulate(50'000'000ull);
		if (!state.unsupported.empty()) {
			outcome.kind = Outcome::Kind::SKIPPED;
			outcome.detail = "global initialization needs " + state.unsupported;
			return outcome;
		}

		const uint64_t address = machine.address_of("test");
		if (address == 0) {
			outcome.kind = Outcome::Kind::FAILED;
			outcome.detail = "the ELF has no symbol for test()";
			return outcome;
		}

		// The Godot Sandbox calling convention: a0 points at a Variant the
		// callee writes its return value into.
		auto& sp = machine.cpu.reg(riscv::REG_SP);
		sp = machine.memory.stack_initial();
		sp -= (static_cast<uint64_t>(layout.variant_size()) + 15) & ~15ull;
		const uint64_t return_variant = sp;
		write_variant(machine, return_variant, layout, Variant::NIL, 0);

		machine.cpu.reg(riscv::REG_RA) = machine.memory.exit_address();
		machine.cpu.reg(riscv::REG_ARG0) = return_variant;
		machine.cpu.jump(address);
		machine.simulate(50'000'000ull);

		if (!state.unsupported.empty()) {
			outcome.kind = Outcome::Kind::SKIPPED;
			outcome.detail = "needs " + state.unsupported;
			return outcome;
		}

		const GuestValue actual = read_variant(machine, return_variant, layout);
		if (std::getenv("GDSC_DIFF_DEBUG") != nullptr) {
			std::cerr << "    [debug] return variant at 0x" << std::hex << return_variant
				<< ", stack_initial 0x" << machine.memory.stack_initial()
				<< ", payload 0x" << actual.as_int << std::dec << std::endl;
		}
		if (!agrees(expected, actual)) {
			outcome.kind = Outcome::Kind::DISAGREED;
			outcome.detail = "interpreter says " + describe(expected) +
				", the machine says " + describe(actual);
		}
		return outcome;
	} catch (const std::exception& e) {
		if (!state.unsupported.empty()) {
			outcome.kind = Outcome::Kind::SKIPPED;
			outcome.detail = "needs " + state.unsupported;
			return outcome;
		}
		outcome.kind = Outcome::Kind::FAILED;
		outcome.detail = std::string("the machine faulted: ") + e.what();
		return outcome;
	}
}

} // namespace

int main(int argc, char** argv) {
	// Two modes. Without arguments it runs the shared corpus, which is what the
	// test suite does. With --fuzz it runs generated programs instead, which is
	// what turns this check into a fuzzer: the generator produces programs
	// nobody wrote, and the machine and the interpreter still have to agree.
	bool fuzz = false;
	uint64_t seed = 0;
	uint64_t count = 200;
	const char* file = nullptr;

	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--fuzz") == 0) {
			fuzz = true;
		} else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
			seed = std::strtoull(argv[++i], nullptr, 10);
		} else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
			count = std::strtoull(argv[++i], nullptr, 10);
		} else if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
			file = argv[++i];
		} else {
			std::cerr << "usage: " << argv[0]
				<< " [--fuzz [--seed N] [--count N]] [--file program.gd]" << std::endl;
			return 2;
		}
	}

	// One program from a file, for reducing a failure by hand.
	if (file != nullptr) {
		std::ifstream stream(file);
		if (!stream) {
			std::cerr << "cannot read " << file << std::endl;
			return 2;
		}
		const std::string source((std::istreambuf_iterator<char>(stream)),
			std::istreambuf_iterator<char>());
		const Outcome outcome = run_source(source);
		switch (outcome.kind) {
			case Outcome::Kind::AGREED:
				std::cout << "agreed" << std::endl;
				return 0;
			case Outcome::Kind::SKIPPED:
				std::cout << "skipped: " << outcome.detail << std::endl;
				return 0;
			default:
				std::cerr << "FAIL: " << outcome.detail << std::endl;
				return 1;
		}
	}

	int agreed = 0;
	int skipped = 0;
	int failures = 0;

	auto record = [&](const std::string& name, const Outcome& outcome) {
		switch (outcome.kind) {
			case Outcome::Kind::AGREED:
				agreed++;
				break;
			case Outcome::Kind::SKIPPED:
				// A skip always says why. A harness that skips silently is a
				// harness that stops testing without anyone noticing.
				skipped++;
				std::cout << "  SKIP " << name << ": " << outcome.detail << std::endl;
				break;
			case Outcome::Kind::DISAGREED:
			case Outcome::Kind::FAILED:
				failures++;
				std::cerr << "  FAIL " << name << ": " << outcome.detail << std::endl;
				break;
		}
	};

	if (!fuzz) {
		std::cout << "=== Differential: IR interpreter against libriscv ===" << std::endl;
		for (const auto& program : gdscript_test::corpus()) {
			record(program.name, run_source(program.source));
		}
		std::cout << agreed << " agreed, " << skipped << " skipped, " << failures << " failed"
			<< " (of " << gdscript_test::corpus().size() << " programs)" << std::endl;
	} else {
		std::cout << "=== Differential fuzzing: IR interpreter against libriscv ===" << std::endl;
		std::cout << "Seeds " << seed << ".." << (seed + count - 1) << std::endl;

		for (uint64_t i = 0; i < count; i++) {
			const uint64_t current = seed + i;
			gdscript_test::GenOptions generator_options;
			generator_options.allow_structs = false;
			gdscript_test::Generator generator(current, generator_options);
			const gdscript_test::GeneratedProgram program = generator.generate();
			const std::string name = "seed " + std::to_string(current);

			const Outcome outcome = run_source(program.source());
			if (outcome.kind == Outcome::Kind::AGREED || outcome.kind == Outcome::Kind::SKIPPED) {
				record(name, outcome);
				continue;
			}

			failures++;

			// Shrink to the smallest program that still disagrees, so what gets
			// reported is small enough to read.
			const gdscript_test::GeneratedProgram smallest = gdscript_test::shrink(program,
				[](const std::string& candidate) {
					const Outcome result = run_source(candidate);
					return result.kind == Outcome::Kind::DISAGREED;
				});

			const Outcome shrunk = run_source(smallest.source());
			std::cerr << "\nDIFFERENTIAL FAILURE (seed " << current << ")\n"
				<< "  " << (shrunk.kind == Outcome::Kind::DISAGREED ? shrunk.detail : outcome.detail) << "\n"
				<< "  Reproduce with: test_differential --fuzz --seed " << current << " --count 1\n"
				<< "  Shrunk program:\n"
				<< smallest.source() << std::endl;

			if (failures >= 10) {
				std::cerr << "Stopping after " << failures << " failures" << std::endl;
				break;
			}
		}

		std::cout << agreed << " agreed, " << skipped << " skipped, " << failures << " failed"
			<< " (of " << count << " generated programs)" << std::endl;
	}

	if (failures > 0) {
		return 1;
	}
	// A run where nothing was actually compared would pass while testing
	// nothing at all.
	if (agreed == 0) {
		std::cerr << "No program was compared: the harness is not testing anything" << std::endl;
		return 1;
	}

	std::cout << "Interpreter and machine agree!" << std::endl;
	return 0;
}
