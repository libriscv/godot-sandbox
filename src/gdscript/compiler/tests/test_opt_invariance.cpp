// Optimization-invariance testing.
//
// An optimizer pass must not change what a program computes. That is
// mechanically checkable without any new oracle: interpret the unoptimized IR,
// interpret the optimized IR, and require the same answer.
//
// When they disagree, the test bisects the pipeline -- run the first n passes
// for each n -- and names the pass that first changed the answer, so the report
// points at a pass rather than at a program.
//
// The same bisection is available from a shell through the GDSC_PASSES
// environment variable, which lists the passes to enable.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../traits.h"
#include "test_corpus.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace gdscript;
using gdscript_test::CorpusProgram;

namespace {

// The observable result of a run: the return value plus every global the run
// left behind. A pass that changes a side effect is as wrong as one that
// changes the return value.
struct RunResult {
	IRInterpreter::Value value;
	std::vector<IRInterpreter::Value> globals;
};

IRProgram build_ir(const std::string& source, size_t pass_limit) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	apply_traits(program);
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
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

std::string to_string(const IRInterpreter::Value& value) {
	std::ostringstream oss;
	if (std::holds_alternative<std::monostate>(value)) {
		oss << "null (nil)";
	} else if (std::holds_alternative<int64_t>(value)) {
		oss << std::get<int64_t>(value) << " (int)";
	} else if (std::holds_alternative<double>(value)) {
		oss.precision(17);
		oss << std::get<double>(value) << " (float)";
	} else if (std::holds_alternative<bool>(value)) {
		oss << (std::get<bool>(value) ? "true" : "false") << " (bool)";
	} else {
		oss << "\"" << std::get<std::string>(value) << "\" (string)";
	}
	return oss.str();
}

// int and bool are deliberately the same category: the interpreter is free to
// produce either for a truth value, and 'and' already yields an integer where
// 'not' yields a bool. int against float is a real difference -- GDScript keeps
// them apart -- and so is a string against anything else.
enum class Category { NIL, INT, FLOAT, STRING };

Category category_of(const IRInterpreter::Value& value) {
	if (std::holds_alternative<std::monostate>(value)) return Category::NIL;
	if (std::holds_alternative<double>(value)) return Category::FLOAT;
	if (std::holds_alternative<std::string>(value)) return Category::STRING;
	return Category::INT;
}

bool values_equal(const IRInterpreter::Value& a, const IRInterpreter::Value& b) {
	if (category_of(a) != category_of(b)) {
		return false;
	}
	switch (category_of(a)) {
		case Category::NIL:
			return true;
		case Category::STRING:
			return std::get<std::string>(a) == std::get<std::string>(b);
		case Category::FLOAT: {
			const double x = std::get<double>(a);
			const double y = std::get<double>(b);
			// Bit-identical, or both NaN. Constant folding evaluates the same
			// operations in the same order, so there is no rounding slack to
			// allow for here: a difference is a difference.
			return (x == y) || (std::isnan(x) && std::isnan(y));
		}
		case Category::INT: {
			const int64_t x = std::holds_alternative<bool>(a) ? (std::get<bool>(a) ? 1 : 0) : std::get<int64_t>(a);
			const int64_t y = std::holds_alternative<bool>(b) ? (std::get<bool>(b) ? 1 : 0) : std::get<int64_t>(b);
			return x == y;
		}
	}
	return false;
}

bool results_equal(const RunResult& a, const RunResult& b, std::string& what) {
	if (!values_equal(a.value, b.value)) {
		what = "return value: " + to_string(a.value) + " vs " + to_string(b.value);
		return false;
	}
	if (a.globals.size() != b.globals.size()) {
		what = "global count differs";
		return false;
	}
	for (size_t i = 0; i < a.globals.size(); i++) {
		if (!values_equal(a.globals[i], b.globals[i])) {
			what = "global " + std::to_string(i) + ": " +
				to_string(a.globals[i]) + " vs " + to_string(b.globals[i]);
			return false;
		}
	}
	return true;
}

// Run the pipeline one step at a time until the answer moves. The step that
// moves it is the guilty pass.
std::string bisect(const CorpusProgram& program, const RunResult& reference) {
	const auto& passes = IROptimizer::pipeline();
	for (size_t n = 1; n <= passes.size(); n++) {
		std::string what;
		try {
			RunResult result = run(program.source, n);
			if (!results_equal(reference, result, what)) {
				return std::string("pass ") + std::to_string(n) + " (" + passes[n - 1].name +
					") changed the result -- " + what;
			}
		} catch (const std::exception& e) {
			return std::string("pass ") + std::to_string(n) + " (" + passes[n - 1].name +
				") made the program fail to run: " + e.what();
		}
	}
	return "no single pass prefix reproduced the difference";
}

int g_failures = 0;

void check_program(const CorpusProgram& program) {
	RunResult unoptimized;
	try {
		unoptimized = run(program.source, 0);
	} catch (const std::exception& e) {
		std::cerr << "FAIL " << program.name << ": unoptimized run failed: " << e.what() << std::endl;
		g_failures++;
		return;
	}

	RunResult optimized;
	try {
		optimized = run(program.source, SIZE_MAX);
	} catch (const std::exception& e) {
		std::cerr << "FAIL " << program.name << ": optimized run failed: " << e.what() << std::endl;
		std::cerr << "      " << bisect(program, unoptimized) << std::endl;
		g_failures++;
		return;
	}

	std::string what;
	if (!results_equal(unoptimized, optimized, what)) {
		std::cerr << "FAIL " << program.name << ": optimization changed the result -- " << what << std::endl;
		std::cerr << "      " << bisect(program, unoptimized) << std::endl;
		g_failures++;
		return;
	}
}

// Every prefix of the pipeline has to agree with the unoptimized run, not just
// the full pipeline: two passes can cancel each other's mistake and hide both.
void check_every_prefix(const CorpusProgram& program) {
	RunResult unoptimized;
	try {
		unoptimized = run(program.source, 0);
	} catch (const std::exception& e) {
		// Already reported by check_program().
		(void) e;
		return;
	}

	const auto& passes = IROptimizer::pipeline();
	for (size_t n = 1; n <= passes.size(); n++) {
		std::string what;
		try {
			RunResult result = run(program.source, n);
			if (!results_equal(unoptimized, result, what)) {
				std::cerr << "FAIL " << program.name << ": passes 1.." << n << " ("
					<< passes[n - 1].name << ") changed the result -- " << what << std::endl;
				g_failures++;
				return;
			}
		} catch (const std::exception& e) {
			std::cerr << "FAIL " << program.name << ": passes 1.." << n << " ("
				<< passes[n - 1].name << ") made the program fail to run: " << e.what() << std::endl;
			g_failures++;
			return;
		}
	}
}

size_t count_opcode(const IRProgram& ir, IROpcode opcode) {
	size_t count = 0;
	for (const auto& func : ir.functions) {
		for (const auto& instr : func.instructions) {
			if (instr.opcode == opcode) {
				count++;
			}
		}
	}
	return count;
}

void test_pass_selection() {
	std::cout << "Testing pass selection..." << std::endl;

	const std::string source = R"(
func test():
	var a = 2
	var b = 3
	return a + b
)";

	// Nothing enabled: the ADD survives, untouched.
	{
		IRProgram ir = build_ir(source, 0);
		const size_t before = ir.functions.at(0).instructions.size();
		assert(count_opcode(ir, IROpcode::ADD) == 1);

		IROptimizer optimizer;
		optimizer.disable_all_passes();
		optimizer.optimize(ir);
		assert(ir.functions.at(0).instructions.size() == before);
		assert(count_opcode(ir, IROpcode::ADD) == 1);
	}

	// Constant folding alone turns it into a load of 5.
	{
		IRProgram ir = build_ir(source, 0);
		IROptimizer optimizer;
		optimizer.set_enabled_passes({ "constant-folding" });
		optimizer.optimize(ir);
		assert(count_opcode(ir, IROpcode::ADD) == 0);
	}

	// A pass that is not the one folding constants leaves the ADD alone.
	{
		IRProgram ir = build_ir(source, 0);
		IROptimizer optimizer;
		optimizer.set_enabled_passes({ "dead-code" });
		optimizer.optimize(ir);
		assert(count_opcode(ir, IROpcode::ADD) == 1);
	}

	// The step limit is a prefix of the pipeline: constant folding is step 1.
	{
		IRProgram ir = build_ir(source, 1);
		assert(count_opcode(ir, IROpcode::ADD) == 0);
	}

	// GDSC_PASSES is the same selection from a shell.
	{
		setenv("GDSC_PASSES", "dead-code", 1);
		IRProgram ir = build_ir(source, 0);
		IROptimizer optimizer;
		optimizer.optimize(ir);
		assert(count_opcode(ir, IROpcode::ADD) == 1);
		unsetenv("GDSC_PASSES");
	}

	// A pass name that does not exist is a mistake, not a silently ignored one.
	{
		bool threw = false;
		try {
			IROptimizer optimizer;
			optimizer.set_enabled_passes({ "no-such-pass" });
		} catch (const CompilerException&) {
			threw = true;
		}
		assert(threw);
	}

	std::cout << "  Pass selection OK" << std::endl;
}

} // namespace

int main() {
	std::cout << "=== Optimization invariance ===" << std::endl;
	std::cout << "Corpus: " << gdscript_test::corpus().size() << " programs, pipeline: "
		<< IROptimizer::pipeline().size() << " passes" << std::endl;

	// GDSC_PASSES is read by the IROptimizer constructor and would silently
	// override the explicit pass selection this test relies on.
	if (std::getenv("GDSC_PASSES") != nullptr) {
		std::cerr << "GDSC_PASSES is set; unset it to run this test" << std::endl;
		return 1;
	}

	for (const auto& program : gdscript_test::corpus()) {
		check_program(program);
		check_every_prefix(program);
	}

	test_pass_selection();

	if (g_failures > 0) {
		std::cerr << g_failures << " invariance failure(s)" << std::endl;
		return 1;
	}

	std::cout << "All optimization-invariance checks passed!" << std::endl;
	return 0;
}
