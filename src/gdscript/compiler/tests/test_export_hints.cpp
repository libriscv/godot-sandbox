#include "../codegen.h"
#include "../compiler.h"
#include "../export_hints.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include <iostream>
#include <string>
#include <vector>

using namespace gdscript;

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

std::string compile_error(const std::string& source) {
	Compiler compiler;
	CompilerOptions options;
	if (!compiler.compile(source, options).empty()) {
		return "";
	}
	return compiler.get_error();
}

const IRGlobalVar* find_global(const IRProgram& ir, const std::string& name) {
	for (const IRGlobalVar& global : ir.globals) {
		if (global.name == name) {
			return &global;
		}
	}
	return nullptr;
}

struct Case {
	const char* declaration;
	const char* name;
	int32_t hint;
	const char* hint_string;
	int32_t usage;
};

void test_every_annotation() {
	std::cout << "Testing what each @export_* publishes..." << std::endl;

	const Case cases[] = {
		{ "@export var plain := 1", "plain", 0, "", 0 },
		{ "@export_range(1, 5) var r := 1", "r", 1, "1.0,5.0", 0 },
		{ "@export_range(-1.5, 5.25, 0.25, \"or_greater\") var rf := 0.0",
			"rf", 1, "-1.5,5.25,0.25,or_greater", 0 },
		{ "@export_enum(\"A\", \"B:5\") var en := 0", "en", 2, "A,B:5", 0 },
		{ "@export_exp_easing var ee := 1.0", "ee", 4, "", 0 },
		{ "@export_exp_easing(\"attenuation\") var ee2 := 1.0", "ee2", 4, "attenuation", 0 },
		{ "@export_flags(\"F:1\", \"W:2\") var fl := 0", "fl", 6, "F:1,W:2", 0 },
		{ "@export_flags_2d_render var f2r := 0", "f2r", 7, "", 0 },
		{ "@export_flags_2d_physics var f2p := 0", "f2p", 8, "", 0 },
		{ "@export_flags_2d_navigation var f2n := 0", "f2n", 9, "", 0 },
		{ "@export_flags_3d_render var f3r := 0", "f3r", 10, "", 0 },
		{ "@export_flags_3d_physics var f3p := 0", "f3p", 11, "", 0 },
		{ "@export_flags_3d_navigation var f3n := 0", "f3n", 12, "", 0 },
		{ "@export_flags_avoidance var fav := 0", "fav", 37, "", 0 },
		{ "@export_file var fa := \"\"", "fa", 13, "", 0 },
		{ "@export_file(\"*.png\", \"*.jpg\") var f2 := \"\"", "f2", 13, "*.png,*.jpg", 0 },
		{ "@export_dir var dd := \"\"", "dd", 14, "", 0 },
		{ "@export_global_file(\"*.txt\") var gf := \"\"", "gf", 15, "*.txt", 0 },
		{ "@export_global_dir var gd := \"\"", "gd", 16, "", 0 },
		{ "@export_multiline var ml := \"\"", "ml", 18, "", 0 },
		{ "@export_placeholder(\"hint text\") var ph := \"\"", "ph", 20, "hint text", 0 },
		{ "@export_color_no_alpha var cna := 1", "cna", 21, "", 0 },
		{ "@export_node_path(\"Node2D\", \"Control\") var np := \"\"", "np", 26, "Node2D,Control", 0 },
		{ "@export_storage var stor := 1", "stor", 0, "", 2 | 4096 },
		{ "@export_custom(PROPERTY_HINT_ENUM, \"X,Y\") var cu := 0", "cu", 2, "X,Y", 0 },
	};

	for (const Case& one : cases) {
		const std::string source = std::string(one.declaration) + "\nfunc test():\n\treturn 1\n";
		const IRProgram ir = compile_to_ir(source);
		const IRGlobalVar* global = find_global(ir, one.name);
		if (global == nullptr) {
			check(false, std::string("no global named ") + one.name);
			continue;
		}
		check(global->is_property, std::string(one.declaration) + " names a property");
		check(global->export_hint.hint == one.hint,
			std::string(one.declaration) + ": hint " + std::to_string(global->export_hint.hint) +
				", expected " + std::to_string(one.hint));
		check(global->export_hint.hint_string == one.hint_string,
			std::string(one.declaration) + ": hint string '" + global->export_hint.hint_string +
				"', expected '" + one.hint_string + "'");
		check(global->export_hint.usage == one.usage,
			std::string(one.declaration) + ": usage " + std::to_string(global->export_hint.usage) +
				", expected " + std::to_string(one.usage));
	}

	std::cout << "  ✓ Every annotation publishes what the engine publishes" << std::endl;
}

void test_range_numbers_are_written_as_the_engine_writes_them() {
	std::cout << "Testing how a range bound is spelled..." << std::endl;

	check(format_hint_number(1.0) == "1.0", "an integral bound keeps one decimal");
	check(format_hint_number(0.0) == "0.0", "zero is 0.0");
	check(format_hint_number(-1.5) == "-1.5", "a negative bound keeps its sign");
	check(format_hint_number(0.000001) == "0.000001", "a small bound is not rounded away");
	check(format_hint_number(1.0 / 3.0) == "0.33333333333333", "fourteen decimals below ten");
	check(format_hint_number(123456789.123456789) == "123456789.123457",
		"the decimals shrink as the integer part grows: " + format_hint_number(123456789.123456789));
	check(format_hint_number(1e20) == "100000000000000000000.0",
		"a bound past fourteen digits keeps the point: " + format_hint_number(1e20));

	std::cout << "  ✓ A range bound reads the way the engine writes it" << std::endl;
}

void test_a_hint_survives_the_other_annotations() {
	std::cout << "Testing stacked annotations..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"@export_range(0, 10) @warning_ignore(\"unused_variable\") var hp := 5\n"
		"func test():\n\treturn hp\n");
	const IRGlobalVar* global = find_global(ir, "hp");
	check(global != nullptr && global->export_hint.hint == 1,
		"an annotation stacked after @export_range does not drop the hint");

	const IRProgram sections = compile_to_ir(
		"@export_group(\"Stats\")\n"
		"@export_multiline var text := \"\"\n"
		"func test():\n\treturn text\n");
	const IRGlobalVar* text = find_global(sections, "text");
	check(text != nullptr && text->export_hint.hint == 18,
		"a property after @export_group keeps its own hint");

	std::cout << "  ✓ A hint belongs to the declaration it was written on" << std::endl;
}

void test_what_is_refused() {
	std::cout << "Testing the refusals..." << std::endl;

	const std::string arity = compile_error(
		"@export_range(0) var hp := 1\n"
		"func test():\n\treturn hp\n");
	check(arity.find("at least 2") != std::string::npos,
		"@export_range with one bound is refused: " + arity);

	const std::string wrong_type = compile_error(
		"@export_range(\"a\", \"b\") var hp := 1\n"
		"func test():\n\treturn hp\n");
	check(wrong_type.find("is a number") != std::string::npos,
		"@export_range over strings is refused: " + wrong_type);

	const std::string not_a_string = compile_error(
		"@export_enum(1, 2) var pick := 0\n"
		"func test():\n\treturn pick\n");
	check(not_a_string.find("takes strings") != std::string::npos,
		"@export_enum over integers is refused: " + not_a_string);

	const std::string placeholder = compile_error(
		"@export_placeholder(\"a\", \"b\") var text := \"\"\n"
		"func test():\n\treturn text\n");
	check(!placeholder.empty(), "@export_placeholder takes exactly one string");

	const IRProgram unknown = compile_to_ir(
		"@export_tool_button(\"Click\") var btn := 0\n"
		"func test():\n\treturn btn\n");
	const IRGlobalVar* btn = find_global(unknown, "btn");
	check(btn != nullptr && btn->is_property && btn->export_hint.is_default(),
		"an @export_* with no row is still a property, with no hint");

	std::cout << "  ✓ Arguments the engine refuses are refused here" << std::endl;
}

void test_constant_expressions_are_not_errors() {
	std::cout << "Testing arguments the scanner cannot evaluate..." << std::endl;

	struct Case {
		const char* source;
		const char* what;
	};
	const Case cases[] = {
		{ "const MAX_SPEED = 100\n@export_range(0, MAX_SPEED) var speed := 0.0\n",
			"@export_range over a script const" },
		{ "@export_range(0, 10.0 / 2) var speed := 0.0\n",
			"@export_range over an arithmetic expression" },
		{ "const NAMES = \"a,b\"\n@export_enum(NAMES) var pick := 0\n",
			"@export_enum over a script const" },
		{ "const H = 2\n@export_custom(H, \"a,b\") var pick := 0\n",
			"@export_custom over a script const" },
	};
	for (const Case& c : cases) {
		const std::string source = std::string(c.source) + "func test():\n\treturn 0\n";
		const std::string error = compile_error(source);
		check(error.empty(), std::string(c.what) + " compiles: " + error);

		const IRProgram program = compile_to_ir(source);
		const IRGlobalVar* var = find_global(program,
			std::string(c.source).find("speed") != std::string::npos ? "speed" : "pick");
		check(var != nullptr && var->is_property && var->export_hint.is_default(),
			std::string(c.what) + " leaves the property unconstrained");
	}

	const IRProgram resolved = compile_to_ir(
		"@export_range(0, PROPERTY_HINT_ENUM) var pick := 0\n"
		"func test():\n\treturn pick\n");
	const IRGlobalVar* pick = find_global(resolved, "pick");
	check(pick != nullptr && pick->export_hint.hint_string == "0.0,2.0",
		"a @GlobalScope constant in @export_range is still evaluated");

	const std::string wrong = compile_error(
		"@export_range(0, \"nope\") var hp := 1\n"
		"func test():\n\treturn hp\n");
	check(wrong.find("is a number") != std::string::npos,
		"a string where the engine wants a number is still refused: " + wrong);

	std::cout << "  ✓ An argument the scanner cannot read drops the hint" << std::endl;
}

} // namespace

int main() {
	std::cout << "=== Export Hint Tests ===" << std::endl << std::endl;

	test_every_annotation();
	test_range_numbers_are_written_as_the_engine_writes_them();
	test_a_hint_survives_the_other_annotations();
	test_what_is_refused();
	test_constant_expressions_are_not_errors();

	if (failures != 0) {
		std::cerr << std::endl << failures << " export hint test(s) failed" << std::endl;
		return 1;
	}
	std::cout << std::endl << "All export hint tests passed!" << std::endl;
	return 0;
}
