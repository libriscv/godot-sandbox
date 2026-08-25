#include "../compiler.h"
#include "scope_stub.h"
#include "../codegen.h"
#include "../instance_layout.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
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
	if (condition) {
		return;
	}
	std::cerr << "FAILED: " << what << std::endl;
	failures++;
}

IRProgram compile_to_ir(const std::string& source) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program ast = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(ast);
	IROptimizer optimizer;
	optimizer.optimize(ir);
	return ir;
}

const IRGlobalVar& find_global(const IRProgram& ir, const std::string& name) {
	for (const auto& global : ir.globals) {
		if (global.name == name) {
			return global;
		}
	}
	std::cerr << "FAILED: no global named " << name << std::endl;
	failures++;
	static IRGlobalVar missing;
	return missing;
}

void test_storage_classification() {
	std::cout << "Testing which declarations are members..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"const LIMIT = 10\n"
		"static var shared = 0\n"
		"var member = 0\n"
		"@export var exported = 1\n"
		"var typed: Array\n"
		"func test():\n\treturn member\n");

	check(!find_global(ir, "LIMIT").is_member(), "a const is not a member");
	check(!find_global(ir, "shared").is_member(), "a static var is not a member");
	check(find_global(ir, "member").is_member(), "a var is a member");
	check(find_global(ir, "exported").is_member(), "an @export var is a member");
	check(find_global(ir, "typed").is_member(), "a type-hinted var is a member");

	std::cout << "  ✓ Storage classification" << std::endl;
}

void test_initializers_are_split() {
	std::cout << "Testing that members initialize per instance..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"static var shared = [1, 2]\n"
		"var mine = [3, 4]\n"
		"func test():\n\treturn mine\n");

	check(ir.has_global_init, "the static var's initializer runs at startup");
	check(ir.has_member_init, "the member's initializer runs per instance");

	const auto stores = [](const IRFunction& fn) {
		int count = 0;
		for (const auto& instr : fn.instructions) {
			count += instr.opcode == IROpcode::STORE_GLOBAL ? 1 : 0;
		}
		return count;
	};
	check(stores(ir.global_init) == 1, "one store at startup");
	check(stores(ir.member_init) == 1, "one store per instance");

	const IRProgram folded = compile_to_ir("var a = 1\nfunc test():\n\treturn a\n");
	check(!folded.has_global_init && !folded.has_member_init,
		"a folding initializer needs no init function");

	std::cout << "  ✓ Initializers are split by storage" << std::endl;
}

void test_shared_initializer_cannot_read_a_member() {
	std::cout << "Testing that a shared initializer cannot read a member..." << std::endl;

	const auto rejected = [](const std::string& source) {
		Compiler compiler;
		CompilerOptions options;
		options.output_elf = true;
		const std::vector<uint8_t> elf = compiler.compile(source, options);
		return elf.empty() && compiler.get_error_info().has_error &&
				compiler.get_error_info().message.find("member variable 'a'") != std::string::npos;
	};

	check(rejected("var a = 5\nstatic var b = a + 1\nfunc test():\n\treturn b\n"),
		"a static var may not read a member");
	check(rejected("var a = 5\nconst C = a + 1\nfunc test():\n\treturn C\n"),
		"a const may not read a member");

	const IRProgram ir = compile_to_ir(
		"static var s = [7]\n"
		"var m = s\n"
		"func test():\n\treturn m\n");
	check(ir.has_global_init, "the static var still initializes at startup");
	check(ir.has_member_init, "and the member reads it per instance");
	check(find_global(ir, "m").is_member(), "m is the member");
	check(!find_global(ir, "s").is_member(), "s is not");

	std::cout << "  ✓ Shared initializers cannot read members" << std::endl;
}

void test_no_members_no_record() {
	std::cout << "Testing a program with no members..." << std::endl;

	const IRProgram ir = compile_to_ir("const A = 1\nfunc test():\n\treturn A\n");
	RISCVCodeGen codegen{ VariantLayout(false) };
	codegen.generate(ir);
	check(codegen.get_instance_member_count() == 0, "no members");
	check(codegen.get_instance_blob_address() == 0, "no record to describe");
	check(codegen.get_instance_blob_size() == 0, "no blob emitted");

	std::cout << "  ✓ No members, no record" << std::endl;
}

void test_member_addressing_is_one_instruction() {
	std::cout << "Testing that a member address is one instruction..." << std::endl;

	// A member at a small offset is `addi rd, tp, off`; a data global is the
	// auipc+addi pair against .globals. Count both in one function.
	const IRProgram ir = compile_to_ir(
		"static var shared = 0\n"
		"var member = 0\n"
		"func test():\n\treturn member\n");
	RISCVCodeGen codegen{ VariantLayout(false) };
	const std::vector<uint8_t> code = codegen.generate(ir);

	const auto& functions = codegen.get_function_offsets();
	const size_t start = functions.at("test");
	size_t auipcs = 0;
	size_t tp_addis = 0;
	for (size_t offset = start; offset + 4 <= code.size(); offset += 4) {
		uint32_t instr = 0;
		std::memcpy(&instr, code.data() + offset, sizeof(instr));
		if ((instr & 0x7F) == 0x67) {
			break; // jalr: end of the function
		}
		if ((instr & 0x7F) == 0x17) {
			auipcs++;
		}
		if ((instr & 0x7F) == 0x13 && ((instr >> 12) & 7) == 0 && ((instr >> 15) & 0x1F) == 4) {
			tp_addis++;
		}
	}
	check(tp_addis >= 1, "the member is addressed off the base register");
	check(auipcs == 0, "and needs no pc-relative pair to do it");

	std::cout << "  ✓ Member addressing" << std::endl;
}

// -= What it does on a real machine =-

struct Booted {
	// The machine keeps a view into this, and address_of() re-reads the symbol
	// table out of it, so it has to outlive the machine.
	std::vector<uint8_t> elf;
	std::unique_ptr<machine_t> machine;
	uint64_t default_base = 0;
	uint64_t record_size = 0;
	uint64_t member_count = 0;
	uint64_t init_address = 0;
};

void unexpected_syscall(machine_t& machine) {
	std::cerr << "FAILED: the test program made syscall "
		<< machine.cpu.reg(riscv::REG_ARG7) << std::endl;
	failures++;
	machine.stop();
}

Booted boot(const std::string& source) {
	Booted out;
	Compiler compiler;
	out.elf = compiler.compile(source, CompilerOptions{});
	if (out.elf.empty()) {
		std::cerr << "FAILED to compile: " << compiler.get_error() << std::endl;
		failures++;
		return out;
	}
	out.machine = std::make_unique<machine_t>(out.elf, riscv::MachineOptions<riscv::RISCV64>{
		.memory_max = 32ull << 20,
		.stack_size = 4ull << 20,
	});
	for (int number = 500; number < 560; number++) {
		machine_t::install_syscall_handler(number, unexpected_syscall);
	}
	install_scope_stub<machine_t>();
	out.machine->simulate(50'000'000ull);

	const uint64_t blob = out.machine->address_of(INSTANCE_SYMBOL);
	check(blob != 0, "the ELF exports the instance layout");
	if (blob != 0) {
		uint32_t magic = 0, version = 0, size = 0, count = 0;
		uint64_t base = 0;
		out.machine->copy_from_guest(&magic, blob + InstanceLayout::MAGIC_OFF, sizeof(magic));
		out.machine->copy_from_guest(&version, blob + InstanceLayout::VERSION_OFF, sizeof(version));
		out.machine->copy_from_guest(&base, blob + InstanceLayout::DEFAULT_BASE_OFF, sizeof(base));
		out.machine->copy_from_guest(&size, blob + InstanceLayout::RECORD_SIZE_OFF, sizeof(size));
		out.machine->copy_from_guest(&count, blob + InstanceLayout::MEMBER_COUNT_OFF, sizeof(count));
		check(magic == InstanceLayout::MAGIC, "the blob carries its magic");
		check(version == InstanceLayout::LAYOUT_VERSION, "and its version");
		out.default_base = base;
		out.record_size = size;
		out.member_count = count;
	}
	out.init_address = out.machine->address_of(INSTANCE_INIT_SYMBOL);
	check(out.init_address != 0, "the ELF exports the instance initializer");
	return out;
}

// Sandbox ABI: a0 = the Variant the callee writes into, and the base register
// says which instance is calling.
int64_t call(machine_t& machine, const std::string& function, uint64_t base) {
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
	machine.cpu.reg(riscv::REG_TP) = base;
	machine.cpu.jump(address);
	machine.simulate(200'000'000ull);

	int64_t value = 0;
	machine.copy_from_guest(&value, retvar + 8, sizeof(value));
	return value;
}

void init_record(machine_t& machine, uint64_t init_address, uint64_t base) {
	auto& sp = machine.cpu.reg(riscv::REG_SP);
	sp = machine.memory.stack_initial();
	sp -= 64;
	machine.cpu.reg(riscv::REG_RA) = machine.memory.exit_address();
	machine.cpu.reg(riscv::REG_ARG0) = base;
	machine.cpu.reg(riscv::REG_TP) = base;
	machine.cpu.jump(init_address);
	machine.simulate(200'000'000ull);
}

void test_two_records_do_not_see_each_other() {
	std::cout << "Testing two instances of one script..." << std::endl;

	Booted booted = boot(
		"static var shared = 0\n"
		"var bump = 0\n"
		"func step():\n"
		"\tbump += 1\n"
		"\tshared += 1\n"
		"\treturn bump\n"
		"func total():\n"
		"\treturn shared\n");
	if (booted.machine == nullptr || booted.init_address == 0) {
		return;
	}
	machine_t& machine = *booted.machine;

	check(booted.member_count == 1, "one member");
	check(booted.record_size == booted.member_count * 24, "sized by the Variant");

	// A second record, out of the guest's own memory, initialized the way the
	// host initializes one.
	const uint64_t second = machine.memory.mmap_allocate(booted.record_size);
	init_record(machine, booted.init_address, second);

	check(call(machine, "step", booted.default_base) == 1, "first instance, first step");
	check(call(machine, "step", second) == 1, "second instance starts from its own 0");
	check(call(machine, "step", booted.default_base) == 2, "and the first one kept counting");
	check(call(machine, "step", second) == 2, "so did the second");

	// The static var is one variable, whoever incremented it.
	check(call(machine, "total", booted.default_base) == 4, "static var counted every step");
	check(call(machine, "total", second) == 4, "from either instance");

	std::cout << "  ✓ Two records" << std::endl;
}

void test_a_record_initializes_its_members() {
	std::cout << "Testing that a fresh record gets the declared values..." << std::endl;

	Booted booted = boot(
		"var a = 7\n"
		"var b = 0\n"
		"func read_a():\n\treturn a\n"
		"func bump_b():\n\tb += 1\n\treturn b\n");
	if (booted.machine == nullptr || booted.init_address == 0) {
		return;
	}
	machine_t& machine = *booted.machine;

	check(call(machine, "bump_b", booted.default_base) == 1, "default record counts");

	const uint64_t second = machine.memory.mmap_allocate(booted.record_size);
	init_record(machine, booted.init_address, second);
	check(call(machine, "read_a", second) == 7, "a fresh record has the declared value");
	check(call(machine, "bump_b", second) == 1, "and its own zero");

	// Re-initializing puts it back, which is what an instance being replaced by
	// another at the same address has to see.
	init_record(machine, booted.init_address, second);
	check(call(machine, "bump_b", second) == 1, "re-initialized to the declared value");
	check(call(machine, "bump_b", booted.default_base) == 2, "and the other record is untouched");

	std::cout << "  ✓ Fresh records" << std::endl;
}

} // namespace

int main() {
	std::cout << "=== Instance record tests ===" << std::endl;
	test_storage_classification();
	test_initializers_are_split();
	test_shared_initializer_cannot_read_a_member();
	test_no_members_no_record();
	test_member_addressing_is_one_instruction();
	test_two_records_do_not_see_each_other();
	test_a_record_initializes_its_members();

	if (failures > 0) {
		std::cerr << failures << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "All instance record tests passed" << std::endl;
	return 0;
}
