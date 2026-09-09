#include "../compiler.h"
#include "../ir_interpreter.h"
#include "../ir_verifier.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace gdscript;

static void check(bool condition, const std::string& message) {
	if (!condition) throw std::runtime_error(message);
}

int main() try {
	Compiler compiler;
	for (bool optimize : {false, true}) {
		CompilerOptions options;
		options.optimize = optimize;
		auto ir = compiler.compile_to_ir(
			"@tool\nclass_name FrontendExample\n"
			"var count: int = 7\n"
			"func answer(value: int) -> int:\n\treturn value * 2 + 2\n", options);
		check(ir.has_value(), compiler.get_error());
		ir_verify(*ir);
		IRInterpreter interpreter(*ir);
		check(std::get<int64_t>(interpreter.call("answer", {int64_t(20)})) == 42,
			"Optimized/unoptimized frontend semantics differ");
		check(compiler.is_tool() && compiler.get_class_name() == "FrontendExample", "Lost script metadata");
		check(compiler.get_property_signatures().size() == 1 && ir->properties.size() == 1, "Lost property metadata");
		check(!compiler.get_function_signatures().empty(), "Lost signatures");
		check(compiler.get_line_table().entries.empty(), "Frontend emitted machine addresses");
	}

	auto invalid = compiler.compile_to_ir("func broken(:\n\tpass\n");
	check(!invalid && compiler.get_error_info().has_error && !compiler.get_error().empty(), "Missing diagnostics");
	check(!compiler.is_tool() && compiler.get_class_name().empty(), "Stale script metadata after failure");
	auto empty = compiler.compile_to_ir("");
	check(empty.has_value() && !compiler.get_error_info().has_error && compiler.get_error().empty(),
		"Empty success must differ from failure and clear diagnostics");

	CompilerOptions options;
	options.emit_tests = false;
	auto shipping = compiler.compile_to_ir("@test\nfunc check_answer():\n\tpass\nfunc answer():\n\treturn 42\n", options);
	check(shipping.has_value(), compiler.get_error());
	check(shipping->tests.empty() && std::none_of(shipping->functions.begin(), shipping->functions.end(),
		[](const IRFunction& function) { return function.name == "check_answer"; }), "Frontend retained shipping tests");

	options = {};
	options.base_sources.push_back({"Base", "res://base.sgd", "func inherited() -> int:\n\treturn 42\n", false});
	auto derived = compiler.compile_to_ir("extends Base\nfunc answer() -> int:\n\treturn inherited()\n", options);
	check(derived.has_value(), compiler.get_error());
	ir_verify(*derived);
	IRInterpreter inherited(*derived);
	check(std::get<int64_t>(inherited.call("answer")) == 42, "Frontend skipped base merging");
	options.restricted = true;
	check(!compiler.compile_to_ir("extends Base\n", options), "Frontend skipped restricted policy");
	std::cout << "Standalone frontend tests passed\n";
	return 0;
} catch (const std::exception& error) {
	std::cerr << error.what() << '\n';
	return 1;
}
