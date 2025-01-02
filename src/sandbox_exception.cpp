#include "sandbox.h"

#include "cpp/script_cpp.h"
#include <charconv>

static constexpr bool VERBOSE_EXCEPTIONS = false;

static inline String to_hex(gaddr_t value) {
	char str[20] = { 0 };
	char *end = std::to_chars(std::begin(str), std::end(str), value, 16).ptr;
	return String::utf8(str, int64_t(end - str));
}

void Sandbox::handle_exception(gaddr_t address) {
	riscv::Memory<RISCV_ARCH>::Callsite callsite = machine().memory.lookup(address);
	// If the callsite is not found, try to use the cache to find the address
	if (callsite.address == 0x0) {
		callsite.address = address;
		auto it = m_lookup.find(address);
		if (it != m_lookup.end()) {
			const auto u8str = it->second.name.utf8();
			callsite = riscv::Memory<RISCV_ARCH>::Callsite{
				.name = std::string(u8str.ptr(), u8str.length()),
				.address = it->second.address,
				.offset = 0x0,
				.size = 0
			};
		}
	}
	UtilityFunctions::print(
			"[", get_name(), "] Exception when calling:\n  ", callsite.name.c_str(), " (0x",
			to_hex(callsite.address), ")\n", "Backtrace:");

	this->m_exceptions++;
	Sandbox::m_global_exceptions++;

	if (m_machine->memory.binary().empty()) {
		ERR_PRINT("No binary loaded. Remember to assign a program to the Sandbox!");
		return;
	}

	this->print_backtrace(address);

	try {
		throw; // re-throw
	} catch (const riscv::MachineTimeoutException &e) {
		this->handle_timeout(address);
		return; // NOTE: might wanna stay
	} catch (const riscv::MachineException &e) {
		const String instr(machine().cpu.current_instruction_to_string().c_str());
		const String regs(machine().cpu.registers().to_string().c_str());

		UtilityFunctions::print(
				"\nException: ", e.what(), "  (data: ", to_hex(e.data()), ")\n",
				">>> ", instr, "\n",
				">>> Machine registers:\n[PC\t", to_hex(machine().cpu.pc()),
				"] ", regs, "\n");
	} catch (const std::exception &e) {
		UtilityFunctions::print("\nMessage: ", e.what(), "\n\n");
		ERR_PRINT(("Exception: " + std::string(e.what())).c_str());
	}

#if defined(__linux__) || defined(__APPLE__)
	// Attempt to print the source code line using addr2line from the C++ Docker container
	// It's not unthinkable that this works for every ELF, regardless of the language
	Ref<ELFScript> script = this->get_program();
	if (!script.is_null()) {
		Array line_out;
		String elfpath = get_program()->get_dockerized_program_path();
		CPPScript::DockerContainerExecute({ "/usr/api/build.sh", "--line", to_hex(address), elfpath }, line_out, false);
		if (line_out.size() > 0) {
			const String line = String(line_out[0]).replace("\n", "").replace("/usr/src/", "res://");
			UtilityFunctions::print("Exception in Sandbox calling function: ", line);
		}
		// Additional line for the current PC, if it's not the same as the call address
		if (machine().cpu.pc() != address) {
			CPPScript::DockerContainerExecute({ "/usr/api/build.sh", "--line", to_hex(machine().cpu.pc()), elfpath }, line_out, false);
			if (line_out.size() > 0) {
				const String line = String(line_out[0]).replace("\n", "").replace("/usr/src/", "res://");
				UtilityFunctions::print("Exception in Sandbox at PC: ", line);
			}
		}
	}
#endif

	if constexpr (VERBOSE_EXCEPTIONS) {
		UtilityFunctions::print(
				"Program page: ", machine().memory.get_page_info(machine().cpu.pc()).c_str());
		UtilityFunctions::print(
				"Stack page: ", machine().memory.get_page_info(machine().cpu.reg(2)).c_str());
	}
}

void Sandbox::handle_timeout(gaddr_t address) {
	this->m_timeouts++;
	Sandbox::m_global_timeouts++;
	auto callsite = machine().memory.lookup(address);
	UtilityFunctions::print(
			"Sandbox: Timeout for '", callsite.name.c_str(),
			"' (Timeouts: ", m_timeouts, ")\n");
}

void Sandbox::print_backtrace(const gaddr_t addr) {
	machine().memory.print_backtrace(
			[](std::string_view line) {
				String line_str(std::string(line).c_str());
				UtilityFunctions::print("-> ", line_str);
			});
	auto origin = machine().memory.lookup(addr);
	String name(origin.name.c_str());
	UtilityFunctions::print(
			"-> [-] 0x", to_hex(origin.address), " + 0x", to_hex(origin.offset),
			": ", name);
}
