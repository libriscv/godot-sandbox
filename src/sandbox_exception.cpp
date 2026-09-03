#include "sandbox.h"

#include "cpp/script_cpp.h"
#include <charconv>
#ifdef __linux__
#include <libriscv/rsp_server.hpp>

#include <unistd.h>
static const uint16_t RSP_PORT = 2159;

static const char *getenv_with_default(const char *str, const char *defval) {
	const char *value = getenv(str);
	if (value)
		return value;
	return defval;
}
#endif

static constexpr bool VERBOSE_EXCEPTIONS = false;

// False for non-.sgd programs; falls back to machine-level backtrace.
#ifndef SAFEGDSCRIPT_DISABLED
bool safegdscript_print_backtrace(Sandbox &p_sandbox, gaddr_t p_pc);
void safegdscript_report_runtime_error(Sandbox &p_sandbox, const String &p_message);
String safegdscript_source_location(Sandbox &p_sandbox, gaddr_t p_pc);
#else
static bool safegdscript_print_backtrace(Sandbox &, gaddr_t) { return false; }
static void safegdscript_report_runtime_error(Sandbox &, const String &) {}
static String safegdscript_source_location(Sandbox &, gaddr_t) { return {}; }
#endif

static inline String to_hex(gaddr_t value) {
	char str[20] = { 0 };
	char *end = std::to_chars(std::begin(str), std::end(str), value, 16).ptr;
	return String::utf8(str, int64_t(end - str));
}

void Sandbox::set_last_exception(const String &p_message, const String &p_location) {
	if (m_last_exception == nullptr)
		m_last_exception = std::make_unique<LastException>();
	m_last_exception->message = p_message;
	m_last_exception->location = p_location;
	m_last_exception->id = m_exceptions;
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

	// Prefer .sgd line table / shadow stack over symbol + offset.
	if (!safegdscript_print_backtrace(*this, machine().cpu.pc())) {
		this->print_backtrace(address);
	}

	// The PC is only meaningful inside the callsite. The callsite's offset is the actual faulting instruction.
	// safegdscript_source_location() ends in ": " for use as a message prefix;
	// a report wants the bare "path:line".
	String exception_location = safegdscript_source_location(*this, machine().cpu.pc());
	if (exception_location.ends_with(": ")) {
		exception_location = exception_location.substr(0, exception_location.length() - 2);
	}

	try {
		throw; // re-throw
	} catch (const riscv::MachineTimeoutException &e) {
		this->handle_timeout(address);
		this->set_last_exception(String("Guest timeout: ") + e.what(), exception_location);
		safegdscript_report_runtime_error(*this, String("Guest timeout: ") + e.what());
		return; // NOTE: might wanna stay
	} catch (const riscv::MachineException &e) {
		const String instr(machine().cpu.current_instruction_to_string().c_str());
		const String regs(machine().cpu.registers().to_string().c_str());

		UtilityFunctions::print(
				"\nException: ", e.what(), "  (data: ", to_hex(e.data()), ")\n",
				">>> ", instr, "\n",
				">>> Machine registers:\n[PC\t", to_hex(machine().cpu.pc()),
				"] ", regs, "\n");
		this->set_last_exception(String("Guest fault: ") + e.what(), exception_location);
		safegdscript_report_runtime_error(*this, String("Guest fault: ") + e.what());
	} catch (const std::exception &e) {
		UtilityFunctions::print("\nMessage: ", e.what(), "\n\n");
		ERR_PRINT(("Exception: " + std::string(e.what())).c_str());
		// api_throw's text already reads as a report line
		this->set_last_exception(String::utf8(e.what()), exception_location);
		safegdscript_report_runtime_error(*this, String("Guest error: ") + e.what());
	}

	String elfpath = "";
#if defined(__linux__) || defined(__APPLE__)
	// Attempt to print the source code line using addr2line from the C++ Docker container
	// It's not unthinkable that this works for every ELF, regardless of the language
	Ref<ELFScript> script = this->get_program();
	if (!script.is_null()) {
		Array line_out;
		elfpath = get_program()->get_dockerized_program_path();
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

#ifdef __linux__
	if (getenv("GDB")) {
		const bool oneFrameUp = false;
		const uint16_t port = RSP_PORT;

		if (0 == fork()) {
			char scrname[64];
			strncpy(scrname, "/tmp/dbgscript-XXXXXX", sizeof(scrname));
			const int fd = mkstemp(scrname);
			if (fd < 0) {
				throw std::runtime_error("Unable to create script for debugging");
			}

			const std::string debugscript =
				// Delete the script file (after GDB closes it)
				"shell unlink " + std::string(scrname)
				+ "\n"
				// Load the original file used by the script
				"file "
				+ std::string(getenv("GDB"))
				+ "\n"
				// Connect remotely to the given port @port
				"target remote localhost:"
				+ std::to_string(port)
				+ "\n"
				// Enable the fancy TUI
				"layout next\nlayout next\n"
				// Disable pagination for the message
				"set pagination off\n"
				// Print the message given by the caller
				"echo Remote debugging session started\n"
				+ "\n"
				// Go up one step from the syscall wrapper (which can fail)
				+ std::string(oneFrameUp ? "up\n" : "");

			ssize_t len = write(fd, debugscript.c_str(), debugscript.size());
			if (len < (ssize_t)debugscript.size()) {
				throw std::runtime_error(
					"Unable to write script file for debugging");
			}
			close(fd);

			const char* argv[]
				= {getenv_with_default("GDBPATH", "/usr/bin/gdb-multiarch"), "-x",
				scrname, nullptr};
			// XXX: This is not kosher, but GDB is open-source, safe and let's not
			// pretend that anyone downloads gdb-multiarch from a website anyway.
			// There is a finite list of things we should pass to GDB to make it
			// behave well, but I haven't been able to find the right combination.
			extern char** environ;
			if (-1 == execve(argv[0], (char* const*)argv, environ)) {
				throw std::runtime_error(
					"Unable to start gdb-multiarch for debugging");
			}
		} // child

		riscv::RSP<RISCV_ARCH> server { this->machine(), port };
		auto client = server.accept();
		if (client != nullptr) {
			printf("GDB connected\n");
			// client->set_verbose(true);
			while (client->process_one())
				;
		}
	} // if getenv("GDB")
#endif
} // handle_exception()

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
