#include "codegen.h"
#include "compiler.h"
#include "elf_builder.h"
#include "ir_optimizer.h"
#include "lexer.h"
#include "parser.h"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <vector>

using namespace gdscript;
using Clock = std::chrono::steady_clock;

namespace {

struct Phases {
	double lex = 0;
	double parse = 0;
	double codegen = 0;
	double optimize = 0;
	double elf = 0;

	double total() const { return lex + parse + codegen + optimize + elf; }

	void min_with(const Phases& other) {
		lex = std::min(lex, other.lex);
		parse = std::min(parse, other.parse);
		codegen = std::min(codegen, other.codegen);
		optimize = std::min(optimize, other.optimize);
		elf = std::min(elf, other.elf);
	}
};

double elapsed_ms(Clock::time_point from, Clock::time_point to) {
	return std::chrono::duration<double, std::milli>(to - from).count();
}

Phases run_once(const std::string& source, bool optimize, bool output_elf, size_t& elf_size) {
	Phases phases;

	auto t0 = Clock::now();
	Lexer lexer(source);
	auto tokens = lexer.tokenize();
	auto t1 = Clock::now();

	Parser parser(tokens);
	parser.set_doc_comments(lexer.doc_comments());
	Program program = parser.parse();
	auto t2 = Clock::now();

	CodeGenerator codegen;
	IRProgram ir_program = codegen.generate(program);
	auto t3 = Clock::now();

	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir_program);
	}
	auto t4 = Clock::now();

	if (output_elf) {
		ElfBuilder elf_builder;
		auto elf = elf_builder.build(ir_program, VariantLayout(native_variant_layout().double_precision),
			false, ProfilingClock::TIME, false, {});
		elf_size = elf.size();
	}
	auto t5 = Clock::now();

	phases.lex = elapsed_ms(t0, t1);
	phases.parse = elapsed_ms(t1, t2);
	phases.codegen = elapsed_ms(t2, t3);
	phases.optimize = elapsed_ms(t3, t4);
	phases.elf = elapsed_ms(t4, t5);
	return phases;
}

void report(const char* label, double ms, double total) {
	std::printf("| %-12s | %8.2f | %4.0f%% |\n", label, ms, total > 0 ? 100.0 * ms / total : 0.0);
}

} // namespace

int main(int argc, char** argv) {
	int repetitions = 3;
	bool optimize = true;
	bool output_elf = true;
	std::vector<std::string> paths;

	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		if (arg == "-n" && i + 1 < argc) {
			repetitions = std::atoi(argv[++i]);
		} else if (arg == "--no-optimize" || arg == "--no-opt") {
			optimize = false;
		} else if (arg == "--no-elf") {
			output_elf = false;
		} else {
			paths.push_back(arg);
		}
	}

	std::vector<std::pair<std::string, std::string>> inputs;
	if (paths.empty()) {
		std::stringstream buffer;
		buffer << std::cin.rdbuf();
		inputs.emplace_back("<stdin>", buffer.str());
	} else {
		for (const auto& path : paths) {
			FILE* file = std::fopen(path.c_str(), "rb");
			if (!file) {
				std::cerr << "Failed to open " << path << std::endl;
				return 1;
			}
			std::string source;
			char chunk[65536];
			size_t got;
			while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
				source.append(chunk, got);
			}
			std::fclose(file);
			inputs.emplace_back(path, std::move(source));
		}
	}

	for (const auto& input : inputs) {
		size_t elf_size = 0;
		Phases best;
		try {
			best = run_once(input.second, optimize, output_elf, elf_size);
			for (int i = 1; i < repetitions; i++) {
				best.min_with(run_once(input.second, optimize, output_elf, elf_size));
			}
		} catch (const std::exception& e) {
			std::cerr << input.first << ": " << e.what() << std::endl;
			return 1;
		}

		std::printf("%s (%zu bytes source, %zu bytes ELF, best of %d)\n",
			input.first.c_str(), input.second.size(), elf_size, repetitions);
		std::printf("| phase        |       ms | share |\n");
		std::printf("| ------------ | -------- | ----- |\n");
		report("lex", best.lex, best.total());
		report("parse", best.parse, best.total());
		report("codegen", best.codegen, best.total());
		report("optimize", best.optimize, best.total());
		report("elf", best.elf, best.total());
		report("total", best.total(), best.total());
		std::printf("\n");
	}
	return 0;
}
