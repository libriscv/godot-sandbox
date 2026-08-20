// Fuzzing the compiler against itself.
//
// The IR verifier and optimization invariance are checks; the generator is what
// turns them into fuzzers. Every generated program is put through both:
//
//   V1  the IR has to verify after code generation and after every optimizer
//       pass, so a pass that corrupts the IR fails here.
//   V2  the optimized program has to compute what the unoptimized one computes,
//       for the full pipeline and for every prefix of it.
//
// Neither needs an expected output written down, which is the point: the
// programs are ones nobody thought to write.
//
// Per commit this runs a fixed seed corpus, so it is deterministic and quick.
// Nightly it is meant to be run with `--seed <random> --count <many>`; a
// failure prints the seed and the shrunk program, and re-running with that seed
// reproduces it exactly.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../lexer.h"
#include "../parser.h"
#include "gdscript_generator.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;
using gdscript_test::GeneratedProgram;

namespace {

// The number of programs the per-commit run covers, from seed 0 upwards. Enough
// to exercise the grammar without making the test suite slow.
constexpr uint64_t DEFAULT_COUNT = 400;

struct RunResult {
	IRInterpreter::Value value;
	std::vector<IRInterpreter::Value> globals;
};

IRProgram build_ir(const std::string& source, size_t pass_limit) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	ir_verify(ir, "codegen");
	if (pass_limit > 0) {
		IROptimizer optimizer;
		optimizer.set_pass_limit(pass_limit);
		optimizer.optimize(ir);
	}
	return ir;
}

RunResult run(const std::string& source, size_t pass_limit) {
	IRProgram ir = build_ir(source, pass_limit);
	IRInterpreter interpreter(ir);
	RunResult result;
	result.value = interpreter.call("test");
	for (size_t i = 0; i < interpreter.global_count(); i++) {
		result.globals.push_back(interpreter.global(i));
	}
	return result;
}

// int and bool are the same answer: the interpreter produces either for a truth
// value. int against float is a real difference.
bool values_equal(const IRInterpreter::Value& a, const IRInterpreter::Value& b) {
	const bool a_float = std::holds_alternative<double>(a);
	const bool b_float = std::holds_alternative<double>(b);
	if (a_float != b_float) {
		return false;
	}
	if (a_float) {
		const double x = std::get<double>(a);
		const double y = std::get<double>(b);
		return x == y || (std::isnan(x) && std::isnan(y));
	}
	const bool a_string = std::holds_alternative<std::string>(a);
	const bool b_string = std::holds_alternative<std::string>(b);
	if (a_string || b_string) {
		return a_string && b_string && std::get<std::string>(a) == std::get<std::string>(b);
	}
	const int64_t x = std::holds_alternative<bool>(a) ? (std::get<bool>(a) ? 1 : 0) : std::get<int64_t>(a);
	const int64_t y = std::holds_alternative<bool>(b) ? (std::get<bool>(b) ? 1 : 0) : std::get<int64_t>(b);
	return x == y;
}

std::string describe(const IRInterpreter::Value& value) {
	if (std::holds_alternative<int64_t>(value)) return std::to_string(std::get<int64_t>(value)) + " (int)";
	if (std::holds_alternative<double>(value)) return std::to_string(std::get<double>(value)) + " (float)";
	if (std::holds_alternative<bool>(value)) return std::string(std::get<bool>(value) ? "true" : "false") + " (bool)";
	return "\"" + std::get<std::string>(value) + "\" (string)";
}

// What went wrong with one program. `kind` is what makes two failures the same
// failure, and is what shrinking is held to: a shrunk program that fails a
// different way is a different bug, and reporting it in place of the original
// would send whoever reads it after the wrong thing.
struct Failure {
	std::string kind;    // empty when the program is fine
	std::string detail;

	bool ok() const { return kind.empty(); }
};

Failure check(const std::string& source) {
	RunResult unoptimized;
	try {
		unoptimized = run(source, 0);
	} catch (const CompilerException& e) {
		// A generated program the compiler rejects is either a generator bug or
		// a missing feature. Either is worth seeing rather than skipping, and
		// the error type keeps one kind of rejection apart from another.
		return { std::string("rejected (") + e.error_type_string() + ")", e.what() };
	} catch (const std::exception& e) {
		return { "unoptimized run failed", e.what() };
	}

	const auto& passes = IROptimizer::pipeline();
	for (size_t n = 1; n <= passes.size(); n++) {
		try {
			// Verification runs inside optimize_function() between passes, so a
			// pass that breaks the IR is caught before the answer is compared.
			const RunResult optimized = run(source, n);
			if (!values_equal(unoptimized.value, optimized.value)) {
				return { std::string("pass '") + passes[n - 1].name + "' changed the result",
					"passes 1.." + std::to_string(n) + ": " + describe(unoptimized.value) +
					" became " + describe(optimized.value) };
			}
			if (optimized.globals.size() != unoptimized.globals.size()) {
				return { "optimization changed the number of globals", "" };
			}
			for (size_t i = 0; i < unoptimized.globals.size(); i++) {
				if (!values_equal(unoptimized.globals[i], optimized.globals[i])) {
					return { std::string("pass '") + passes[n - 1].name + "' changed a global",
						"passes 1.." + std::to_string(n) + ", global " + std::to_string(i) + ": " +
						describe(unoptimized.globals[i]) + " became " + describe(optimized.globals[i]) };
				}
			}
		} catch (const CompilerException& e) {
			return { std::string("pass '") + passes[n - 1].name + "' produced " + e.error_type_string(),
				e.what() };
		} catch (const std::exception& e) {
			return { std::string("pass '") + passes[n - 1].name + "' made it fail", e.what() };
		}
	}

	return {};
}

std::string describe(const Failure& failure) {
	if (failure.detail.empty()) {
		return failure.kind;
	}
	return failure.kind + ": " + failure.detail;
}

// Print a program indented, so it stands apart from the report around it.
void print_source(const std::string& source) {
	std::string line;
	for (char c : source) {
		if (c == '\n') {
			std::cerr << "    | " << line << "\n";
			line.clear();
		} else {
			line += c;
		}
	}
	if (!line.empty()) {
		std::cerr << "    | " << line << "\n";
	}
}

void report(const GeneratedProgram& original, const Failure& original_failure,
	const GeneratedProgram& shrunk, const Failure& shrunk_failure)
{
	std::cerr << "\nFUZZ FAILURE (seed " << original.seed << ")\n"
		<< "  " << describe(shrunk_failure) << "\n"
		<< "  Reproduce with: test_fuzz --seed " << original.seed << " --count 1\n"
		<< "  Shrunk program:\n";
	print_source(shrunk.source());

	if (shrunk.source() != original.source()) {
		std::cerr << "  As generated (" << describe(original_failure) << "):\n";
		print_source(original.source());
	}
	std::cerr << std::endl;
}

} // namespace

int main(int argc, char** argv) {
	uint64_t seed = 0;
	uint64_t count = DEFAULT_COUNT;

	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
			seed = std::strtoull(argv[++i], nullptr, 10);
		} else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
			count = std::strtoull(argv[++i], nullptr, 10);
		} else {
			std::cerr << "usage: " << argv[0] << " [--seed N] [--count N]" << std::endl;
			return 2;
		}
	}

	std::cout << "=== Fuzzing: verifier and optimization invariance ===" << std::endl;
	std::cout << "Seeds " << seed << ".." << (seed + count - 1) << std::endl;

	// Verification is on regardless of the build type: a fuzz run with the
	// verifier off is only running half the checks.
	set_ir_verification_enabled(true);

	int failures = 0;
	for (uint64_t i = 0; i < count; i++) {
		const uint64_t current = seed + i;
		gdscript_test::Generator generator(current);
		const GeneratedProgram program = generator.generate();

		const Failure failure = check(program.source());
		if (failure.ok()) {
			continue;
		}

		failures++;

		// Shrink to the smallest program that still fails the same way, so the
		// report is something a person can read. Accepting any failure would
		// let the shrinker walk into a program that fails because deleting a
		// declaration left a name undefined, which is a different bug from the
		// one being reported.
		const GeneratedProgram smallest = gdscript_test::shrink(program,
			[&failure](const std::string& source) {
				return check(source).kind == failure.kind;
			});

		// Report the failure the shrunk program produces: it is the one being
		// shown, so it has to be the one described.
		const Failure shrunk_failure = check(smallest.source());
		report(program, failure, smallest, shrunk_failure.ok() ? failure : shrunk_failure);

		// Ten reports is enough to work from; more is noise.
		if (failures >= 10) {
			std::cerr << "Stopping after " << failures << " failures" << std::endl;
			break;
		}
	}

	if (failures > 0) {
		std::cerr << failures << " generated program(s) failed" << std::endl;
		return 1;
	}

	std::cout << count << " generated programs passed the verifier and optimization invariance"
		<< std::endl;
	return 0;
}
