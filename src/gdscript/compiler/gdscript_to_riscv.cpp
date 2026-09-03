#include "compiler.h"
#include "tool_check.h"
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <unistd.h>
#include <vector>

using namespace gdscript;

std::string run_command(const char* cmd) {
	FILE* pipe = popen(cmd, "r");
	if (!pipe) {
		return "Error: Failed to run command";
	}

	char buffer[4096];
	std::string result;
	try {
		while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
			result += buffer;
		}
	} catch (...) {
		pclose(pipe);
		throw;
	}
	pclose(pipe);
	return result;
}

static void print_usage(const char* program) {
	std::cout <<
		"Usage: " << program << " [options] [file]\n"
		"\n"
		"Compiles SafeGDScript to a RISC-V ELF and disassembles it. Reads the script\n"
		"from [file], or from standard input when no file is given. Disassembly needs\n"
		"riscv64-linux-gnu-objdump on PATH.\n"
		"\n"
		"Options:\n"
		"  -f, --function NAME    Disassemble only this function\n"
		"  -o, --output PATH      Write the ELF to PATH instead of disassembling\n"
		"  -l, --program-headers  Show `readelf -l` instead of the disassembly\n"
		"      --no-optimize      Skip the optimizer (alias: --no-opt)\n"
		"      --check            Diagnostics only; exit 1 when the script has errors\n"
		"      --strip-tests      Leave @test functions out, as a shipping build does\n"
		"      --profiling        Emit self-instrumentation (wall clock)\n"
		"      --profiling-instructions  The same, counting instructions\n"
		"      --autoload NAME    Declare a project autoload. Repeatable\n"
		"      --global-class N=P Declare a class_name script at res:// path P. Repeatable\n"
		"      --base Name=path   Prepend a base script to the chain. Repeatable\n"
		"      --trait Name=path  Make a trait from `path` available. Repeatable\n"
		"      --double-precision Compile for a real_t = double host\n"
		"      --single-precision Compile for a real_t = float host\n"
		"  -h, --help             Show this text\n"
		"\n"
		"GDSC_PASSES=<names> selects optimizer passes; GDSC_PASSES=none disables them.\n";
}

int main(int argc, char** argv)
{
	std::string source;
	std::string output_function; // Function to disassemble
	std::string temp_elf = "/tmp/gdscript_temp_XXXXXX";
	bool no_optimize = false;
	bool show_program_headers = false;
	std::string output_elf_path;
	bool double_precision = native_variant_layout().double_precision;
	bool profiling = false;
	bool strip_tests = false;
	bool check_only = false;
	ProfilingClock profiling_clock = ProfilingClock::TIME;
	std::vector<std::string> autoloads;
	std::vector<std::pair<std::string, std::string>> global_classes;
	std::vector<CompilerOptions::BaseSource> base_sources;

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--no-opt" || arg == "--no-optimize") {
			no_optimize = true;
		} else if (arg == "-f" || arg == "--function") {
			if (i + 1 < argc) {
				output_function = argv[++i];
			}
		} else if (arg == "-l" || arg == "--program-headers") {
			show_program_headers = true;
		} else if (arg == "-o" || arg == "--output") {
			if (i + 1 < argc) {
				output_elf_path = argv[++i];
			}
		} else if (arg == "--double-precision") {
			double_precision = true;
		} else if (arg == "--single-precision") {
			double_precision = false;
		} else if (arg == "--autoload") {
			if (i + 1 < argc) {
				autoloads.push_back(argv[++i]);
			}
		} else if (arg == "--global-class") {
			if (i + 1 < argc) {
				const std::string pair = argv[++i];
				const size_t eq = pair.find('=');
				if (eq != std::string::npos) {
					global_classes.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
				}
			}
		} else if (arg == "--base" || arg == "--trait") {
			if (i + 1 < argc) {
				const std::string pair = argv[++i];
				const size_t eq = pair.find('=');
				if (eq == std::string::npos) {
					std::cerr << "Error: " << arg << " wants Name=path" << std::endl;
					return 1;
				}
				CompilerOptions::BaseSource base;
				base.name = pair.substr(0, eq);
				base.trait_only = arg == "--trait";
				base.path = pair.substr(eq + 1);
				std::ifstream in(base.path);
				if (!in) {
					std::cerr << "Error: cannot read base script " << base.path << std::endl;
					return 1;
				}
				base.source.assign(std::istreambuf_iterator<char>(in),
					std::istreambuf_iterator<char>());
				base_sources.push_back(std::move(base));
			}
		} else if (arg == "--strip-tests") {
			strip_tests = true;
		} else if (arg == "--check") {
			check_only = true;
		} else if (arg == "--help" || arg == "-h") {
			print_usage(argv[0]);
			return 0;
		} else if (arg == "--profiling") {
			profiling = true;
		} else if (arg == "--profiling-instructions") {
			profiling = true;
			profiling_clock = ProfilingClock::INSTRUCTIONS;
		} else if (source.empty()) {
			source = arg;
		}
	}

	std::string source_path;
	if (!source.empty()) {
		std::ifstream in(source);
		if (in) {
			source_path = source;
			source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
	}
	if (source.empty()) {
		std::string line;
		while (std::getline(std::cin, line)) {
			source += line + "\n";
		}
	}

	if (check_only) {
		CompilerOptions options;
		options.optimize = !no_optimize;
		options.double_precision = double_precision;
		options.emit_tests = !strip_tests;
		options.autoloads = autoloads;
		options.global_script_classes = global_classes;
		options.base_sources = base_sources;
		return check_source(source, source_path, options);
	}

	try {
		mkstemp(temp_elf.data());

		Compiler compiler;
		CompilerOptions options;
		options.output_elf = true;
		options.optimize = !no_optimize;
		options.double_precision = double_precision;
		options.profiling = profiling;
		// What a shipping build produces: no @test function reaches codegen.
		options.emit_tests = !strip_tests;
		options.profiling_clock = profiling_clock;
		options.autoloads = autoloads;
		options.global_script_classes = global_classes;
		options.base_sources = base_sources;
		std::vector<uint8_t> elf = compiler.compile(source, options);
		if (elf.empty()) {
			// Empty ELF = compile error.
			std::cerr << "Error: " << compiler.get_error() << std::endl;
			unlink(temp_elf.c_str());
			return 1;
		}

		if (!output_elf_path.empty()) {
			std::ofstream out(output_elf_path, std::ios::binary);
			if (!out) {
				std::cerr << "Error: cannot write " << output_elf_path << std::endl;
				unlink(temp_elf.c_str());
				return 1;
			}
			out.write(reinterpret_cast<const char*>(elf.data()), elf.size());
			unlink(temp_elf.c_str());
			return 0;
		}

		{
			std::ofstream out(temp_elf, std::ios::binary);
			out.write(reinterpret_cast<const char*>(elf.data()), elf.size());
		}

		if (show_program_headers) {
			std::ostringstream cmd;
			cmd << "readelf -l " << temp_elf << " 2>&1";
			std::string output = run_command(cmd.str().c_str());
			std::cout << output;
			unlink(temp_elf.c_str());
			return 0;
		}

		std::ostringstream cmd;
		cmd << "riscv64-linux-gnu-objdump -d " << temp_elf << " 2>&1";
		std::string disasm = run_command(cmd.str().c_str());

		std::istringstream stream(disasm);
		std::string line;
		bool function_found = output_function.empty(); // If no function specified, print all
		bool in_function = function_found;

		while (std::getline(stream, line)) {
			if (line.find("<" + output_function + ">") != std::string::npos ||
			    line.find("<" + output_function + ">") != std::string::npos) {
				in_function = true;
				function_found = true;
				std::cout << line << std::endl;
				continue;
			}

			if (in_function) {
				if (!output_function.empty() && !line.empty() && line[0] != ' ' && line.find("Disassembly") == std::string::npos) {
					in_function = false;
					break;
				}
				std::cout << line << std::endl;
			}
		}

		if (!function_found) {
			std::cerr << "Warning: Function '" << output_function << "' not found in disassembly." << std::endl;
			std::cerr << "Available functions:" << std::endl;

			stream = std::istringstream(disasm);
			while (std::getline(stream, line)) {
				if (line.find("<") != std::string::npos && line.find(">:") != std::string::npos) {
					size_t start = line.find("<");
					size_t end = line.find(">:");
					if (start != std::string::npos && end != std::string::npos) {
						std::cerr << "  " << line.substr(start + 1, end - start - 1) << std::endl;
					}
				}
			}
		}

		unlink(temp_elf.c_str());

		return function_found ? 0 : 1;
	} catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}
}
