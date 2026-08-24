// Syntax the compiler previously rejected: node-path sugar, raw/typed string
// literals, signal, static var, attributes, `for i in <int>`, assert(), null.
// A parse failure takes the whole file down.
#include "../lexer.h"
#include "../parser.h"
#include "../codegen.h"
#include "../ir_optimizer.h"
#include "../ir_verifier.h"
#include "../riscv_codegen.h"
#include "../compiler_exception.h"
#include "../syscall_numbers.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace gdscript;

// -= Helpers =-

static IRProgram compile_to_ir(const std::string& source, bool optimize = false) {
	Lexer lexer(source);
	Parser parser(lexer.tokenize());
	Program program = parser.parse();
	CodeGenerator codegen;
	IRProgram ir = codegen.generate(program);
	if (optimize) {
		IROptimizer optimizer;
		optimizer.optimize(ir);
	}
	return ir;
}

static const IRFunction& find_function(const IRProgram& ir, const std::string& name) {
	for (const auto& func : ir.functions) {
		if (func.name == name) {
			return func;
		}
	}
	throw std::runtime_error("Function not found: " + name);
}

static bool refuses(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException&) {
		return true;
	}
	return false;
}

static int count_opcode(const IRFunction& func, IROpcode opcode) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

static bool machine_code_builds(const std::string& source) {
	IRProgram ir = compile_to_ir(source, true);
	RISCVCodeGen backend;
	return !backend.generate(ir).empty();
}

// Single string constant from the program.
static std::string only_string(const IRProgram& ir) {
	assert(ir.string_constants.size() == 1);
	return ir.string_constants[0];
}

// -= $Node and %Unique =-

static void test_node_path_sugar() {
	std::cout << "Testing $Node, $\"a/b\" and %Unique..." << std::endl;

	// All four spellings -> ECALL_GET_NODE.
	static const struct { const char* source; const char* path; } cases[] = {
		{ "$Sprite2D",         "Sprite2D" },
		{ "$Path/To/Node",     "Path/To/Node" },
		{ "$\"Path/To/Node\"", "Path/To/Node" },
		{ "%Unique",           "%Unique" },
		{ "%Unique/Child",     "%Unique/Child" },
		{ "$Parent/%Unique",   "Parent/%Unique" },
	};

	for (const auto& one : cases) {
		const std::string source = std::string("func test():\n\treturn ") + one.source + "\n";
		const IRProgram ir = compile_to_ir(source);
		const IRFunction& test = find_function(ir, "test");
		assert(count_opcode(test, IROpcode::GET_NODE) == 1);
		assert(count_opcode(test, IROpcode::CALL) == 0);
		// Path embedded in instruction; ECALL_GET_NODE reads raw characters.
		for (const auto& instr : test.instructions) {
			if (instr.opcode == IROpcode::GET_NODE) {
				const std::string& got = std::get<std::string>(instr.operands[1].value);
				if (got != one.path) {
					std::cerr << "  " << one.source << " -> \"" << got << "\"" << std::endl;
					assert(false);
				}
			}
		}
		assert(machine_code_builds(source));
	}

	// Run-time path -> VCALL on node (syscall takes raw characters only).
	const IRProgram computed = compile_to_ir("func test(p):\n\treturn get_node(p)\n");
	const IRFunction& computed_fn = find_function(computed, "test");
	assert(count_opcode(computed_fn, IROpcode::VCALL) == 1);
	assert(count_opcode(computed_fn, IROpcode::GET_NODE) == 1);

	// `%` is modulo in expression context.
	const IRProgram modulo = compile_to_ir("func test(a, b):\n\treturn a % b\n");
	assert(count_opcode(find_function(modulo, "test"), IROpcode::MOD) == 1);
	const IRProgram assign = compile_to_ir("func test(a):\n\ta %= 3\n\treturn a\n");
	assert(count_opcode(find_function(assign, "test"), IROpcode::MOD) == 1);

	// Missing path segment -> error.
	assert(refuses("func test():\n\treturn $\n"));
	assert(refuses("func test():\n\treturn $A/\n"));

	std::cout << "  ✓ node-path sugar becomes get_node()" << std::endl;
}

// -= String literal spellings =-

static void test_string_literals() {
	std::cout << "Testing r\"raw\", &\"name\" and ^\"path\"..." << std::endl;

	// Raw string: backslash is literal but still escapes the quote.
	const IRProgram raw = compile_to_ir("func test():\n\treturn r\"a\\nb\"\n");
	assert(only_string(raw) == "a\\nb");
	const IRProgram raw_quote = compile_to_ir("func test():\n\treturn r\"a\\\"b\"\n");
	assert(only_string(raw_quote) == "a\\\"b");
	// Normal string escapes.
	const IRProgram escaped = compile_to_ir("func test():\n\treturn \"a\\nb\"\n");
	assert(only_string(escaped) == "a\nb");
	// Bare `r` is an identifier.
	assert(refuses("func test():\n\treturn r\n"));
	const IRProgram identifier = compile_to_ir("func test():\n\tvar r = 1\n\treturn r\n");
	assert(count_opcode(find_function(identifier, "test"), IROpcode::LOAD_STRING) == 0);

	// &"x" -> STRING_NAME, ^"a/b" -> NODE_PATH via LOAD_STRING_AS.
	const IRProgram string_name = compile_to_ir("func test():\n\treturn &\"speed\"\n");
	const IRFunction& string_name_fn = find_function(string_name, "test");
	assert(count_opcode(string_name_fn, IROpcode::LOAD_STRING_AS) == 1);
	assert(count_opcode(string_name_fn, IROpcode::LOAD_STRING) == 0);
	for (const auto& instr : string_name_fn.instructions) {
		if (instr.opcode == IROpcode::LOAD_STRING_AS) {
			assert(std::get<int64_t>(instr.operands[2].value) == Variant::STRING_NAME);
		}
	}
	const IRProgram node_path = compile_to_ir("func test():\n\treturn ^\"a/b\"\n");
	for (const auto& instr : find_function(node_path, "test").instructions) {
		if (instr.opcode == IROpcode::LOAD_STRING_AS) {
			assert(std::get<int64_t>(instr.operands[2].value) == Variant::NODE_PATH);
		}
	}
	assert(machine_code_builds("func test():\n\treturn &\"speed\"\n"));
	assert(machine_code_builds("func test():\n\treturn ^\"a/b\"\n"));

	// & and ^ remain bitwise operators without a string literal.
	const IRProgram bitwise = compile_to_ir("func test(a, b):\n\treturn (a & b) ^ 1\n");
	assert(count_opcode(find_function(bitwise, "test"), IROpcode::BIT_AND) == 1);
	assert(count_opcode(find_function(bitwise, "test"), IROpcode::BIT_XOR) == 1);

	std::cout << "  ✓ raw, StringName and NodePath literals" << std::endl;
}

// -= Declarations that used to take the whole file down =-

static void test_declarations() {
	std::cout << "Testing signal, static var and attributes with arguments..." << std::endl;

	const std::string script =
		"@tool\n"
		"extends Node\n"
		"class_name Enemy\n"
		"signal died(who)\n"
		"signal healed\n"
		"@export_range(0, 100) var hp = 100\n"
		"@export var speed : float = 1.5\n"
		"static var counter = 0\n"
		"@warning_ignore(\"unused\")\n"
		"static func bump() -> int:\n"
		"\tcounter += 1\n"
		"\treturn counter\n"
		"func test():\n"
		"\treturn hp + bump()\n";

	const IRProgram ir = compile_to_ir(script);
	assert(ir.globals.size() == 3);
	// @export_range: property is published, hint dropped.
	assert(ir.globals[0].name == "hp" && ir.globals[0].is_property);
	assert(ir.globals[1].name == "speed" && ir.globals[1].is_property);
	// `static var` -> global (no instance to differ from).
	assert(ir.globals[2].name == "counter" && !ir.globals[2].is_property);
	find_function(ir, "bump");
	find_function(ir, "test");
	assert(machine_code_builds(script));

	// @onready refused; load time is not available.
	assert(refuses("@onready var n = 1\nfunc test():\n\treturn n\n"));
	// Unknown attribute -> error.
	assert(refuses("@bogus var n = 1\nfunc test():\n\treturn n\n"));
	// Unterminated argument list -> error.
	assert(refuses("@export_range(0, 100 var hp = 1\nfunc test():\n\treturn hp\n"));
	// @export names a property; a function is not one, and dropping the
	// annotation silently would publish nothing.
	assert(refuses("@export func test():\n\treturn 1\n"));
	// A file-level annotation before a function is still fine.
	compile_to_ir("@tool\nfunc test():\n\treturn 1\n");

	// Grouping annotations stand alone (not attached to a declaration).
	for (const char* grouping : { "export_group", "export_subgroup", "export_category" }) {
		compile_to_ir(std::string("@") + grouping + "(\"Stats\")\nfunc test():\n\treturn 1\n");
		const IRProgram grouped = compile_to_ir(std::string("@") + grouping +
			"(\"Stats\")\n@export var hp = 1\nfunc test():\n\treturn hp\n");
		assert(grouped.globals.size() == 1);
		assert(grouped.globals[0].is_property);
	}
	compile_to_ir("@export_category(\"A\")\n@export_subgroup(\"B\")\n"
		"@export var hp = 1\nfunc test():\n\treturn hp\n");

	std::cout << "  ✓ signal, static var and @export_* parse" << std::endl;
}

static void test_statement_annotations() {
	std::cout << "Testing statement-level annotations..." << std::endl;

	const IRProgram ir = compile_to_ir(
		"func test():\n"
		"\t@warning_ignore(\"unused_variable\")\n"
		"\tvar unused = 1\n"
		"\treturn 2\n");
	find_function(ir, "test");

	compile_to_ir("func test():\n\t@warning_ignore(\"a\")\n\t@warning_ignore(\"b\")\n\treturn 1\n");
	compile_to_ir("func test():\n\tif true:\n\t\t@warning_ignore(\"a\")\n\t\treturn 1\n\treturn 0\n");
	compile_to_ir("func test():\n\t@warning_ignore_start(\"a\")\n\tvar x = 1\n"
		"\t@warning_ignore_restore(\"a\")\n\treturn x\n");

	assert(refuses("func test():\n\t@export var x = 1\n\treturn x\n"));
	assert(refuses("func test():\n\t@bogus var x = 1\n\treturn x\n"));

	std::cout << "  ✓ statement-level annotations parse and drop" << std::endl;
}

// Qualified type names (`A.B`) parse and drop; only the engine can resolve them.
static void test_qualified_type_names() {
	std::cout << "Testing qualified type names..." << std::endl;

	// Parameter, return type, variable, and inside a container's element type.
	compile_to_ir("func test(a : Node.Inner):\n\treturn a\n");
	compile_to_ir("func test() -> Node.Inner:\n\treturn null\n");
	compile_to_ir("var a : Node.Inner = null\nfunc test():\n\treturn a\n");
	compile_to_ir("func test(a : Array[Node.Inner]):\n\treturn a\n");
	compile_to_ir("func test(a : A.B.C):\n\treturn a\n");

	// Dropped entirely, not misread as the first segment.
	const IRProgram ir = compile_to_ir(
		"var qualified : Node.Inner = null\n"
		"var plain : Array = []\n");
	assert(ir.globals[0].type_hint == IRInstruction::TypeHint_NONE);
	assert(ir.globals[1].type_hint == Variant::ARRAY);

	// Unresolvable qualified type leaves the local untyped.
	const IRProgram local = compile_to_ir(
		"func test():\n"
		"\tvar a : Node.Inner = null\n"
		"\treturn a\n");
	find_function(local, "test");

	// `extends` with dotted name and path.
	compile_to_ir("extends Node.Inner\nfunc test():\n\treturn 1\n");
	compile_to_ir("extends \"res://other.gd\"\nfunc test():\n\treturn 1\n");
	compile_to_ir("extends Node\nfunc test():\n\treturn 1\n");

	// A dangling '.' is still a syntax error.
	assert(refuses("func test(a : Node.):\n\treturn a\n"));
	assert(refuses("extends Node.\nfunc test():\n\treturn 1\n"));

	assert(machine_code_builds("func test(a : Node.Inner) -> Node.Inner:\n\treturn a\n"));

	std::cout << "  ✓ a qualified type name parses and drops" << std::endl;
}

// -= for i in <int> =-

static void test_for_over_an_integer() {
	std::cout << "Testing `for i in 10`..." << std::endl;

	// The counted loop, not the container walk: ECALL_ARRAY_SIZE on an
	// integer throws in the host.
	const IRProgram literal = compile_to_ir(
		"func test():\n\tvar t = 0\n\tfor i in 10:\n\t\tt += i\n\treturn t\n", true);
	const IRFunction& literal_fn = find_function(literal, "test");
	assert(count_opcode(literal_fn, IROpcode::CALL_SYSCALL) == 0);
	assert(count_opcode(literal_fn, IROpcode::VCALL) == 0);

	// Same for a bound whose type is declared rather than literal.
	const IRProgram declared = compile_to_ir(
		"func test(n : int):\n\tvar t = 0\n\tfor i in n:\n\t\tt += i\n\treturn t\n", true);
	assert(count_opcode(find_function(declared, "test"), IROpcode::CALL_SYSCALL) == 0);

	// An untyped bound cannot be told from a container, so it stays a walk.
	const IRProgram untyped = compile_to_ir(
		"func test(n):\n\tfor i in n:\n\t\tpass\n");
	assert(count_opcode(find_function(untyped, "test"), IROpcode::CALL_SYSCALL) > 0);

	// Float bound: counted loop, no syscall.
	for (const char* source : { "func test():\n\tfor i in 2.5:\n\t\tpass\n",
			"func test():\n\tvar f : float = 3.0\n\tfor i in f:\n\t\tpass\n" }) {
		const IRProgram floated = compile_to_ir(source);
		assert(count_opcode(find_function(floated, "test"), IROpcode::CALL_SYSCALL) == 0);
	}

	// A bool or null is not iterable in the engine either.
	assert(refuses("func test():\n\tfor i in true:\n\t\tpass\n"));
	assert(refuses("func test():\n\tfor i in null:\n\t\tpass\n"));

	// `for i: int in ...` parses; the type is the loop's, not the hint's.
	compile_to_ir("func test():\n\tfor i: int in range(3):\n\t\tpass\n");
	compile_to_ir("func test(a):\n\tfor v: Vector2 in a:\n\t\tpass\n");

	std::cout << "  ✓ an integer bound takes the counted loop" << std::endl;
}

// -= assert() =-

static void test_assert() {
	std::cout << "Testing assert()..." << std::endl;

	// A branch over a THROW. A dropped assert is worse than no assert, so
	// this must not be a self-call the engine silently ignores.
	const IRProgram with_message = compile_to_ir(
		"func test(x):\n\tassert(x > 0, \"x must be positive\")\n\treturn x\n");
	const IRFunction& with_message_fn = find_function(with_message, "test");
	assert(count_opcode(with_message_fn, IROpcode::THROW) == 1);
	assert(count_opcode(with_message_fn, IROpcode::VCALL) == 0);
	for (const auto& instr : with_message_fn.instructions) {
		if (instr.opcode == IROpcode::THROW) {
			assert(std::get<std::string>(instr.operands[0].value) == "assert");
			assert(std::get<std::string>(instr.operands[1].value) == "x must be positive");
		}
	}

	// One argument gets a default message.
	const IRProgram bare = compile_to_ir("func test(x):\n\tassert(x)\n\treturn 1\n");
	assert(count_opcode(find_function(bare, "test"), IROpcode::THROW) == 1);

	// The optimizer must not delete it, and the backend must emit it.
	const IRProgram optimized = compile_to_ir(
		"func test(x):\n\tassert(x)\n\treturn 1\n", true);
	assert(count_opcode(find_function(optimized, "test"), IROpcode::THROW) == 1);
	assert(machine_code_builds("func test(x):\n\tassert(x, \"no\")\n\treturn 1\n"));

	// The host reads the message as bytes, so it has to be a literal.
	assert(refuses("func test(x):\n\tassert(x, x)\n"));
	assert(refuses("func test(x):\n\tassert()\n"));
	assert(refuses("func test(x):\n\tassert(x, \"a\", \"b\")\n"));

	// A local function of that name still wins.
	const IRProgram shadowed = compile_to_ir(
		"func assert(x):\n\treturn x\nfunc test():\n\treturn assert(1)\n");
	assert(count_opcode(find_function(shadowed, "test"), IROpcode::THROW) == 0);

	std::cout << "  ✓ assert() branches over a THROW" << std::endl;
}

// -= null =-

static void test_null_is_not_zero() {
	std::cout << "Testing that null is a NIL Variant..." << std::endl;

	// LOAD_IMM 0 is an INT Variant holding zero, which Godot does not treat
	// as null: `x == null` would compare against the integer.
	const IRProgram returned = compile_to_ir("func test():\n\treturn null\n");
	const IRFunction& returned_fn = find_function(returned, "test");
	assert(count_opcode(returned_fn, IROpcode::LOAD_NIL) == 1);
	assert(count_opcode(returned_fn, IROpcode::LOAD_IMM) == 0);

	// A variable with no initializer is null too, unless its declared type
	// has a default of its own.
	const IRProgram untyped = compile_to_ir("func test():\n\tvar x\n\treturn x\n");
	assert(count_opcode(find_function(untyped, "test"), IROpcode::LOAD_NIL) == 1);
	const IRProgram object = compile_to_ir("func test():\n\tvar n : Node\n\treturn n\n");
	assert(count_opcode(find_function(object, "test"), IROpcode::LOAD_NIL) == 1);

	// A declared type that the guest can build gets that type's default, not
	// an integer zero wearing the type's label.
	const IRProgram string_var = compile_to_ir("func test():\n\tvar s : String\n\treturn s\n");
	assert(count_opcode(find_function(string_var, "test"), IROpcode::LOAD_STRING) == 1);
	const IRProgram array_var = compile_to_ir("func test():\n\tvar a : Array\n\treturn a\n");
	assert(count_opcode(find_function(array_var, "test"), IROpcode::MAKE_ARRAY) == 1);

	assert(machine_code_builds("func test():\n\treturn null\n"));

	std::cout << "  ✓ null and uninitialized declarations" << std::endl;
}

// -= @GlobalScope constants =-

static void test_global_constants() {
	std::cout << "Testing PI, TYPE_INT and the rest..." << std::endl;

	// Compile-time numbers: an immediate, not a global load or a syscall.
	const IRProgram pi = compile_to_ir("func test():\n\treturn PI\n");
	const IRFunction& pi_fn = find_function(pi, "test");
	assert(count_opcode(pi_fn, IROpcode::LOAD_FLOAT_IMM) == 1);
	assert(count_opcode(pi_fn, IROpcode::LOAD_GLOBAL) == 0);

	// The type tags pair with typeof(), and both sides fold to integers, so
	// the compare stays off the VEVAL path.
	const IRProgram tags = compile_to_ir("func test(x):\n\treturn typeof(x) == TYPE_INT\n");
	const IRFunction& tags_fn = find_function(tags, "test");
	assert(count_opcode(tags_fn, IROpcode::TYPE_OF) == 1);
	for (const auto& instr : tags_fn.instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM) {
			assert(std::get<int64_t>(instr.operands[1].value) == Variant::INT);
		}
	}

	compile_to_ir("func test():\n\treturn TAU\n");
	compile_to_ir("func test():\n\treturn INF\n");
	compile_to_ir("func test():\n\treturn NAN\n");
	compile_to_ir("func test():\n\treturn OK\n");
	compile_to_ir("func test():\n\treturn FAILED\n");
	compile_to_ir("func test():\n\treturn ERR_FILE_NOT_FOUND\n");
	compile_to_ir("func test():\n\treturn KEY_ESCAPE\n");
	compile_to_ir("func test():\n\treturn MOUSE_BUTTON_LEFT\n");
	compile_to_ir("func test():\n\treturn JOY_AXIS_LEFT_X\n");
	compile_to_ir("func test():\n\treturn TYPE_MAX\n");

	// A declaration of the same name still shadows it.
	const IRProgram shadowed = compile_to_ir(
		"func test():\n\tvar PI = 3\n\treturn PI\n");
	assert(count_opcode(find_function(shadowed, "test"), IROpcode::LOAD_FLOAT_IMM) == 0);
	const IRProgram global = compile_to_ir("var OK = 7\nfunc test():\n\treturn OK\n");
	assert(count_opcode(find_function(global, "test"), IROpcode::LOAD_GLOBAL) == 1);

	// A name that is not one is still undefined.
	assert(refuses("func test():\n\treturn TYPE_NOT_A_TYPE\n"));

	std::cout << "  ✓ @GlobalScope constants fold to immediates" << std::endl;
}

int main() {
	std::cout << "=== Syntax and Declaration Tests ===" << std::endl << std::endl;

	try {
		test_node_path_sugar();
		test_string_literals();
		test_declarations();
		test_statement_annotations();
		test_qualified_type_names();
		test_for_over_an_integer();
		test_assert();
		test_null_is_not_zero();
		test_global_constants();
	} catch (const CompilerException& e) {
		std::cerr << "Unexpected compiler error: " << e.what() << std::endl;
		return 1;
	}

	std::cout << std::endl << "All syntax tests passed." << std::endl;
	return 0;
}
