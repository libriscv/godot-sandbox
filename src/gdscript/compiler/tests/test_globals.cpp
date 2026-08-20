// GDScript's global functions.
//
// print(), abs(), sin(), clamp(), str() and the rest are @GlobalScope
// functions rather than methods on the owner node, so a call to one cannot
// fall through to the self-call path: Godot would drop it without a word.
// globals.h's table is what the compiler knows about them, and what these
// tests pin down is that the table drives everything -- the arity check, the
// result type, and which of the lowerings each call becomes.
//
// What a global *computes* is checked in three other places, all of which read
// the same corpus: test_opt_invariance (the optimizer must not change it),
// test_differential (the IR interpreter and a real RISC-V machine must agree
// on it), and the Godot integration tests (Godot must agree too). What is
// checked here is the compile-time half.
#include "../codegen.h"
#include "../compiler_exception.h"
#include "../globals.h"
#include "../ir_interpreter.h"
#include "../ir_optimizer.h"
#include "../lexer.h"
#include "../parser.h"
#include "../riscv_codegen.h"
#include "../variant_layout.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

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

static int count_opcode(const IRFunction& func, IROpcode opcode) {
	int count = 0;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == opcode) {
			count++;
		}
	}
	return count;
}

// The GlobalFn every GLOBAL_CALL in a function names, in order.
static std::vector<GlobalFn> called_globals(const IRFunction& func) {
	std::vector<GlobalFn> names;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::GLOBAL_CALL) {
			names.push_back(static_cast<GlobalFn>(std::get<int64_t>(instr.operands.at(1).value)));
		}
	}
	return names;
}

// Whether every GLOBAL_CALL says its arguments are already the type its form
// works in, which is what lets the backend skip the run-time type test.
static bool all_calls_typed(const IRFunction& func) {
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::GLOBAL_CALL && std::get<int64_t>(instr.operands.at(2).value) == 0) {
			return false;
		}
	}
	return true;
}

// The value `test()` returns, through the IR interpreter.
static IRInterpreter::Value run(const std::string& source, bool optimize = false) {
	IRProgram ir = compile_to_ir(source, optimize);
	IRInterpreter interpreter(ir);
	return interpreter.call("test");
}

static int64_t run_int(const std::string& source, bool optimize = false) {
	const IRInterpreter::Value value = run(source, optimize);
	assert(std::holds_alternative<int64_t>(value));
	return std::get<int64_t>(value);
}

static double run_float(const std::string& source, bool optimize = false) {
	const IRInterpreter::Value value = run(source, optimize);
	assert(std::holds_alternative<double>(value));
	return std::get<double>(value);
}

static bool run_bool(const std::string& source, bool optimize = false) {
	const IRInterpreter::Value value = run(source, optimize);
	assert(std::holds_alternative<bool>(value));
	return std::get<bool>(value);
}

static bool close_enough(double a, double b) {
	return std::fabs(a - b) < 1e-9;
}

// The message from a program that must not compile.
static std::string compile_error(const std::string& source) {
	try {
		compile_to_ir(source);
	} catch (const CompilerException& e) {
		return e.what();
	} catch (const std::exception& e) {
		return e.what();
	}
	return "";
}

static bool mentions(const std::string& haystack, const std::string& needle) {
	return haystack.find(needle) != std::string::npos;
}

// -= The table =-

static void test_the_table_is_consistent() {
	// Every row's arity is sane, every SYSCALL row names an op and says how
	// many of fa0-fa4 it fills, and every NUMERIC row's two forms exist.
	static const char* NAMES[] = {
		"print", "abs", "absi", "absf", "sign", "signi", "signf",
		"floor", "floorf", "floori", "ceil", "ceilf", "ceili",
		"round", "roundf", "roundi", "snapped", "snappedf", "snappedi",
		"min", "mini", "minf", "max", "maxi", "maxf", "clamp", "clampi", "clampf",
		"posmod", "fmod", "fposmod", "wrap", "wrapi", "wrapf",
		"sqrt", "pow", "exp", "log",
		"sin", "cos", "tan", "asin", "acos", "atan", "atan2",
		"sinh", "cosh", "tanh", "asinh", "acosh", "atanh",
		"deg_to_rad", "rad_to_deg", "angle_difference", "linear_to_db", "db_to_linear",
		"lerp", "lerpf", "lerp_angle", "inverse_lerp", "remap", "smoothstep",
		"move_toward", "rotate_toward", "pingpong",
		"cubic_interpolate", "bezier_interpolate", "bezier_derivative",
		"is_nan", "is_inf", "is_finite", "is_zero_approx", "is_equal_approx",
		"str", "len",
	};

	for (const char* name : NAMES) {
		const GlobalFunction* info = find_global_function(name);
		assert(info != nullptr);
		assert(std::string(info->name) == name);
		assert(info->min_args <= info->max_args);

		// The row a GlobalFn resolves to has to be the row it came from.
		assert(&global_function(info->fn) == info);

		if (info->kind == GlobalKind::SYSCALL) {
			assert(info->utility_op >= 0 && info->utility_op < UTILITY_OP_COUNT);
			assert(info->float_args >= 1 && info->float_args <= UTILITY_MAX_FLOAT_ARGS);
			// A syscall form takes exactly as many arguments as it passes.
			assert(info->min_args == info->float_args && info->max_args == info->float_args);
		}
		if (info->kind == GlobalKind::NUMERIC) {
			assert(info->result == GlobalResult::NUMERIC);
			const GlobalFunction& as_int = global_function(info->int_form);
			const GlobalFunction& as_float = global_function(info->float_form);
			assert(as_int.result == GlobalResult::INT);
			assert(as_float.result == GlobalResult::INT || as_float.result == GlobalResult::FLOAT);
			assert(resolve_numeric_form(*info, true) == info->int_form);
			assert(resolve_numeric_form(*info, false) == info->float_form);
		}
	}

	// A name GDScript does not have is not a global, and neither is the
	// internal form that has no GDScript name.
	assert(find_global_function("no_such_global") == nullptr);
	assert(find_global_function("randi") == nullptr); // deliberately left out
	assert(find_global_function(".int_identity") != nullptr);

	std::cout << "  ✓ the table agrees with itself" << std::endl;
}

// -= Lowering =-

static void test_print_is_still_print() {
	// print() is the one global with a side effect, and the one that keeps its
	// own opcode: ECALL_PRINT takes a contiguous array of Variants.
	IRProgram ir = compile_to_ir(
		"func test():\n"
		"\tprint(1, 2, 3)\n"
		"\treturn 0\n");
	const IRFunction& func = find_function(ir, "test");
	assert(count_opcode(func, IROpcode::PRINT) == 1);
	assert(count_opcode(func, IROpcode::GLOBAL_CALL) == 0);

	// Nothing about a global becomes a VCALL on the owner node, which is what
	// the self-call fallback would have produced.
	assert(count_opcode(func, IROpcode::VCALL) == 0);

	std::cout << "  ✓ print() is a PRINT and nothing else" << std::endl;
}

static void test_every_other_global_is_one_opcode() {
	IRProgram ir = compile_to_ir(
		"func test():\n"
		"\treturn absi(-1) + roundi(1.5) + len(\"ab\")\n");
	const IRFunction& func = find_function(ir, "test");
	assert(count_opcode(func, IROpcode::GLOBAL_CALL) == 3);
	assert(count_opcode(func, IROpcode::VCALL) == 0);

	const std::vector<GlobalFn> called = called_globals(func);
	assert(called.size() == 3);
	assert(called[0] == GlobalFn::ABSI);
	assert(called[1] == GlobalFn::ROUNDI);
	assert(called[2] == GlobalFn::LEN);

	std::cout << "  ✓ every other global is a GLOBAL_CALL" << std::endl;
}

static void test_numeric_dispatch_resolves_when_the_types_are_known() {
	// abs(2) is the integer 2 and abs(2.0) is the float 2.0, so the dispatcher
	// resolves to a concrete form whenever the argument types say which.
	IRProgram ir = compile_to_ir(
		"func test():\n"
		"\tvar i: int = -2\n"
		"\tvar f: float = -2.0\n"
		"\treturn abs(i) + abs(f) + min(i, 1) + min(f, 1.0)\n");
	const IRFunction& func = find_function(ir, "test");
	const std::vector<GlobalFn> called = called_globals(func);
	assert(called.size() == 4);
	assert(called[0] == GlobalFn::ABSI);
	assert(called[1] == GlobalFn::ABSF);
	assert(called[2] == GlobalFn::MINI);
	assert(called[3] == GlobalFn::MINF);
	assert(all_calls_typed(func));

	std::cout << "  ✓ a known argument type resolves the dispatcher" << std::endl;
}

static void test_numeric_dispatch_survives_to_the_backend() {
	// An untyped parameter leaves the choice to run time, which is the case the
	// backend emits the type test for. The dispatcher itself has to reach the
	// instruction, untyped.
	IRProgram ir = compile_to_ir(
		"func measure(x):\n"
		"\treturn abs(x)\n"
		"func test():\n"
		"\treturn measure(1)\n");
	const IRFunction& func = find_function(ir, "measure");
	const std::vector<GlobalFn> called = called_globals(func);
	assert(called.size() == 1);
	assert(called[0] == GlobalFn::ABS);
	assert(!all_calls_typed(func));

	std::cout << "  ✓ an unknown argument type reaches the backend as the dispatcher" << std::endl;
}

static void test_int_arguments_widen_for_float_forms() {
	// sqrt(16) has to reach the host as a double. GDScript's one implicit
	// numeric conversion happens in the IR, as a CONVERT the optimizer can
	// fold, rather than as a type test in the emitted code.
	IRProgram ir = compile_to_ir(
		"func test():\n"
		"\treturn sqrt(16)\n");
	const IRFunction& func = find_function(ir, "test");
	assert(count_opcode(func, IROpcode::CONVERT) == 1);
	assert(all_calls_typed(func));

	std::cout << "  ✓ an integer argument widens before a floating-point form" << std::endl;
}

static void test_variadic_min_and_max_fold() {
	// min(a, b, c, d) is three two-argument calls; nothing downstream has to
	// know that GDScript's min() is variadic.
	IRProgram ir = compile_to_ir(
		"func test():\n"
		"\treturn min(4, 3, 2, 1)\n");
	const IRFunction& func = find_function(ir, "test");
	assert(count_opcode(func, IROpcode::GLOBAL_CALL) == 3);
	for (GlobalFn fn : called_globals(func)) {
		assert(fn == GlobalFn::MINI);
	}
	assert(run_int("func test():\n\treturn min(4, 3, 2, 1)\n") == 1);
	assert(run_int("func test():\n\treturn max(4, 3, 2, 1)\n") == 4);

	std::cout << "  ✓ variadic min() and max() fold into pairs" << std::endl;
}

static void test_a_global_result_is_typed() {
	// The table's result column becomes the register's type hint, which is
	// what lets the arithmetic around a call take its typed path.
	IRProgram ir = compile_to_ir(
		"func test():\n"
		"\treturn absi(-1)\n");
	const IRFunction& func = find_function(ir, "test");
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::GLOBAL_CALL) {
			assert(instr.type_hint == Variant::INT);
		}
	}

	IRProgram floats = compile_to_ir(
		"func test():\n"
		"\treturn sin(1.0)\n");
	for (const auto& instr : find_function(floats, "test").instructions) {
		if (instr.opcode == IROpcode::GLOBAL_CALL) {
			assert(instr.type_hint == Variant::FLOAT);
		}
	}

	// An unresolved dispatcher has no result type, and must not claim one.
	IRProgram untyped = compile_to_ir(
		"func measure(x):\n"
		"\treturn abs(x)\n"
		"func test():\n"
		"\treturn measure(1)\n");
	for (const auto& instr : find_function(untyped, "measure").instructions) {
		if (instr.opcode == IROpcode::GLOBAL_CALL) {
			assert(instr.type_hint == IRInstruction::TypeHint_NONE);
		}
	}

	std::cout << "  ✓ the table's result column becomes the type hint" << std::endl;
}

static void test_only_print_has_a_side_effect() {
	// No global except print() does anything a program can observe beyond its
	// result, so every one of them is free to be moved or dropped. print() is
	// not: it prints.
	//
	// Dead-code elimination is deliberately narrower than purity -- it only
	// removes a pure instruction that reads no register at all -- so a call
	// with arguments still survives it. What matters here is that the
	// metadata says the truth, because that is what every pass reads.
	assert(ir_is_pure(IROpcode::GLOBAL_CALL));
	assert(!ir_is_pure(IROpcode::PRINT));

	// print() survives the optimizer whatever is done with its result.
	IRProgram printed = compile_to_ir(
		"func test():\n"
		"\tvar unused = print(1)\n"
		"\treturn 1\n",
		true);
	assert(count_opcode(find_function(printed, "test"), IROpcode::PRINT) == 1);

	std::cout << "  ✓ only print() has a side effect" << std::endl;
}

// -= Diagnostics =-

static void test_arity_is_checked_by_name() {
	assert(mentions(compile_error("func test():\n\treturn abs()\n"),
		"abs() takes 1 argument, got 0"));
	assert(mentions(compile_error("func test():\n\treturn abs(1, 2)\n"),
		"abs() takes 1 argument, got 2"));
	assert(mentions(compile_error("func test():\n\treturn clamp(1, 2)\n"),
		"clamp() takes 3 arguments, got 2"));
	assert(mentions(compile_error("func test():\n\treturn min(1)\n"),
		"min() takes at least 2 arguments, got 1"));
	assert(mentions(compile_error("func test():\n\treturn len()\n"),
		"len() takes 1 argument, got 0"));

	std::cout << "  ✓ the arity check names the function" << std::endl;
}

static void test_a_local_function_wins() {
	// A script that defines its own abs() calls its own. The globals are
	// checked after the local functions for exactly this reason.
	IRProgram ir = compile_to_ir(
		"func abs(x):\n"
		"\treturn x + 100\n"
		"func test():\n"
		"\treturn abs(1)\n");
	const IRFunction& func = find_function(ir, "test");
	assert(count_opcode(func, IROpcode::GLOBAL_CALL) == 0);
	assert(count_opcode(func, IROpcode::CALL) == 1);
	assert(run_int(
		"func abs(x):\n"
		"\treturn x + 100\n"
		"func test():\n"
		"\treturn abs(1)\n") == 101);

	std::cout << "  ✓ a local function of the same name wins" << std::endl;
}

static void test_host_globals_are_rejected_by_the_interpreter() {
	// str() and len() need the host's Variant API. The IR interpreter has to
	// say so rather than answer something plausible.
	bool threw = false;
	try {
		run("func test():\n\treturn len(\"abc\")\n");
	} catch (const std::exception& e) {
		threw = true;
		assert(mentions(e.what(), "len"));
	}
	assert(threw);

	std::cout << "  ✓ str() and len() are not evaluated without a host" << std::endl;
}

// -= What the globals compute =-

static void test_integer_forms() {
	assert(run_int("func test():\n\treturn absi(-7)\n") == 7);
	assert(run_int("func test():\n\treturn absi(7)\n") == 7);
	assert(run_int("func test():\n\treturn signi(-7)\n") == -1);
	assert(run_int("func test():\n\treturn signi(0)\n") == 0);
	assert(run_int("func test():\n\treturn signi(7)\n") == 1);
	assert(run_int("func test():\n\treturn mini(3, 5)\n") == 3);
	assert(run_int("func test():\n\treturn maxi(3, 5)\n") == 5);
	assert(run_int("func test():\n\treturn clampi(9, 0, 5)\n") == 5);
	assert(run_int("func test():\n\treturn clampi(-9, 0, 5)\n") == 0);
	assert(run_int("func test():\n\treturn clampi(3, 0, 5)\n") == 3);
	assert(run_int("func test():\n\treturn posmod(-3, 5)\n") == 2);
	assert(run_int("func test():\n\treturn posmod(3, 5)\n") == 3);
	// A zero divisor answers zero rather than trapping, the same way integer
	// division by zero does here.
	assert(run_int("func test():\n\treturn posmod(3, 0)\n") == 0);
	assert(run_int("func test():\n\treturn wrapi(11, 0, 10)\n") == 1);
	assert(run_int("func test():\n\treturn wrapi(-1, 0, 10)\n") == 9);
	assert(run_int("func test():\n\treturn wrapi(5, 3, 3)\n") == 3);

	std::cout << "  ✓ the integer forms" << std::endl;
}

static void test_numeric_forms_keep_the_type() {
	// The whole point of the dispatcher: an integer in, an integer out.
	assert(std::holds_alternative<int64_t>(run("func test():\n\treturn abs(-2)\n")));
	assert(std::holds_alternative<double>(run("func test():\n\treturn abs(-2.0)\n")));
	assert(run_int("func test():\n\treturn floor(-2)\n") == -2);
	assert(close_enough(run_float("func test():\n\treturn floor(-2.5)\n"), -3.0));
	assert(close_enough(run_float("func test():\n\treturn ceil(-2.5)\n"), -2.0));
	assert(close_enough(run_float("func test():\n\treturn round(-2.5)\n"), -3.0));
	assert(close_enough(run_float("func test():\n\treturn round(2.5)\n"), 3.0));
	assert(run_int("func test():\n\treturn clamp(9, 0, 5)\n") == 5);
	assert(close_enough(run_float("func test():\n\treturn clamp(9.0, 0.0, 5.0)\n"), 5.0));

	// And through a parameter, where the choice is made at run time.
	const char* dispatched =
		"func measure(x):\n"
		"\treturn abs(x)\n"
		"func test():\n"
		"\treturn measure(-3)\n";
	assert(run_int(dispatched) == 3);
	const char* dispatched_float =
		"func measure(x):\n"
		"\treturn abs(x)\n"
		"func test():\n"
		"\treturn measure(-3.5)\n";
	assert(close_enough(run_float(dispatched_float), 3.5));

	std::cout << "  ✓ the type-preserving forms keep the type" << std::endl;
}

static void test_float_forms() {
	assert(close_enough(run_float("func test():\n\treturn absf(-2.5)\n"), 2.5));
	assert(close_enough(run_float("func test():\n\treturn sqrt(9.0)\n"), 3.0));
	assert(close_enough(run_float("func test():\n\treturn minf(1.5, 2.5)\n"), 1.5));
	assert(close_enough(run_float("func test():\n\treturn maxf(1.5, 2.5)\n"), 2.5));
	assert(close_enough(run_float("func test():\n\treturn clampf(9.5, 0.0, 5.0)\n"), 5.0));
	assert(close_enough(run_float("func test():\n\treturn pow(2.0, 10.0)\n"), 1024.0));
	assert(close_enough(run_float("func test():\n\treturn exp(0.0)\n"), 1.0));
	assert(close_enough(run_float("func test():\n\treturn log(1.0)\n"), 0.0));
	assert(close_enough(run_float("func test():\n\treturn sin(0.0)\n"), 0.0));
	assert(close_enough(run_float("func test():\n\treturn cos(0.0)\n"), 1.0));
	assert(close_enough(run_float("func test():\n\treturn atan2(0.0, 1.0)\n"), 0.0));
	assert(close_enough(run_float("func test():\n\treturn fmod(7.5, 2.0)\n"), 1.5));
	assert(close_enough(run_float("func test():\n\treturn fposmod(-7.5, 2.0)\n"), 0.5));
	assert(close_enough(run_float("func test():\n\treturn deg_to_rad(180.0)\n"), 3.141592653589793));
	assert(close_enough(run_float("func test():\n\treturn rad_to_deg(3.141592653589793)\n"), 180.0));
	assert(close_enough(run_float("func test():\n\treturn lerp(1.0, 5.0, 0.25)\n"), 2.0));
	assert(close_enough(run_float("func test():\n\treturn inverse_lerp(1.0, 5.0, 2.0)\n"), 0.25));
	assert(close_enough(run_float("func test():\n\treturn remap(5.0, 0.0, 10.0, 100.0, 200.0)\n"), 150.0));
	assert(close_enough(run_float("func test():\n\treturn smoothstep(0.0, 1.0, 0.5)\n"), 0.5));
	assert(close_enough(run_float("func test():\n\treturn move_toward(1.0, 5.0, 0.5)\n"), 1.5));
	assert(close_enough(run_float("func test():\n\treturn snappedf(2.6, 0.5)\n"), 2.5));
	assert(close_enough(run_float("func test():\n\treturn wrapf(11.0, 0.0, 10.0)\n"), 1.0));

	// The forms that take a float and answer an integer.
	assert(run_int("func test():\n\treturn floori(2.6)\n") == 2);
	assert(run_int("func test():\n\treturn ceili(2.1)\n") == 3);
	assert(run_int("func test():\n\treturn roundi(2.6)\n") == 3);
	assert(run_int("func test():\n\treturn snappedi(2.6, 2)\n") == 2);

	std::cout << "  ✓ the floating-point forms" << std::endl;
}

static void test_predicates() {
	assert(run_bool("func test():\n\treturn is_nan(0.0)\n") == false);
	assert(run_bool("func test():\n\treturn is_finite(1.0)\n") == true);
	assert(run_bool("func test():\n\treturn is_inf(1.0)\n") == false);
	assert(run_bool("func test():\n\treturn is_zero_approx(0.0000001)\n") == true);
	assert(run_bool("func test():\n\treturn is_zero_approx(1.0)\n") == false);
	assert(run_bool("func test():\n\treturn is_equal_approx(1.0, 1.0)\n") == true);
	assert(run_bool("func test():\n\treturn is_equal_approx(1.0, 2.0)\n") == false);
	// 1.0 / 0.0 is an infinity in GDScript rather than an error.
	assert(run_bool("func test():\n\treturn is_inf(1.0 / 0.0)\n") == true);

	std::cout << "  ✓ the predicates" << std::endl;
}

static void test_optimizing_does_not_change_the_answer() {
	// The corpus checks this across the whole pipeline; this is the spot check
	// that says the new opcode is not simply skipped by every pass.
	static const char* PROGRAMS[] = {
		"func test():\n\treturn absi(-7) + mini(2, 9) + clampi(11, 0, 10)\n",
		"func test():\n\treturn sqrt(16.0) + pow(2.0, 8.0)\n",
		"func test():\n\tvar i = 0\n\tvar total = 0\n\twhile i < 5:\n\t\ttotal = total + absi(i - 3)\n\t\ti = i + 1\n\treturn total\n",
	};
	for (const char* source : PROGRAMS) {
		const IRInterpreter::Value plain = run(source, false);
		const IRInterpreter::Value optimized = run(source, true);
		assert(plain.index() == optimized.index());
		if (std::holds_alternative<int64_t>(plain)) {
			assert(std::get<int64_t>(plain) == std::get<int64_t>(optimized));
		} else {
			assert(close_enough(std::get<double>(plain), std::get<double>(optimized)));
		}
	}

	std::cout << "  ✓ optimizing does not change the answer" << std::endl;
}

// -= Reaching RISC-V =-

static void test_every_global_reaches_riscv() {
	// One call to every global the table knows, in one function, compiled all
	// the way down. This is what catches a row whose form the backend has no
	// emission for -- which used to be a run-time surprise inside the sandbox.
	const std::string source =
		"func test(x, y, z):\n"
		"\tprint(x)\n"
		"\tvar t = 0.0\n"
		"\tt = t + abs(x) + absi(1) + absf(1.0)\n"
		"\tt = t + sign(x) + signi(1) + signf(1.0)\n"
		"\tt = t + floor(x) + floorf(1.0) + floori(1.0)\n"
		"\tt = t + ceil(x) + ceilf(1.0) + ceili(1.0)\n"
		"\tt = t + round(x) + roundf(1.0) + roundi(1.0)\n"
		"\tt = t + snapped(x, y) + snappedf(1.0, 0.5) + snappedi(1.0, 2)\n"
		"\tt = t + min(x, y) + mini(1, 2) + minf(1.0, 2.0)\n"
		"\tt = t + max(x, y) + maxi(1, 2) + maxf(1.0, 2.0)\n"
		"\tt = t + clamp(x, y, z) + clampi(1, 2, 3) + clampf(1.0, 2.0, 3.0)\n"
		"\tt = t + posmod(1, 2) + fmod(1.0, 2.0) + fposmod(1.0, 2.0)\n"
		"\tt = t + wrap(x, y, z) + wrapi(1, 2, 3) + wrapf(1.0, 2.0, 3.0)\n"
		"\tt = t + sqrt(1.0) + pow(1.0, 2.0) + exp(1.0) + log(1.0)\n"
		"\tt = t + sin(1.0) + cos(1.0) + tan(1.0) + asin(1.0) + acos(1.0) + atan(1.0) + atan2(1.0, 2.0)\n"
		"\tt = t + sinh(1.0) + cosh(1.0) + tanh(1.0) + asinh(1.0) + acosh(1.0) + atanh(1.0)\n"
		"\tt = t + deg_to_rad(1.0) + rad_to_deg(1.0) + angle_difference(1.0, 2.0)\n"
		"\tt = t + linear_to_db(1.0) + db_to_linear(1.0)\n"
		"\tt = t + lerp(1.0, 2.0, 0.5) + lerpf(1.0, 2.0, 0.5) + lerp_angle(1.0, 2.0, 0.5)\n"
		"\tt = t + inverse_lerp(1.0, 2.0, 0.5) + remap(1.0, 0.0, 2.0, 0.0, 4.0)\n"
		"\tt = t + smoothstep(0.0, 1.0, 0.5) + move_toward(1.0, 2.0, 0.5) + rotate_toward(1.0, 2.0, 0.5)\n"
		"\tt = t + pingpong(1.0, 2.0)\n"
		"\tt = t + cubic_interpolate(1.0, 2.0, 0.0, 3.0, 0.5)\n"
		"\tt = t + bezier_interpolate(0.0, 1.0, 2.0, 3.0, 0.5) + bezier_derivative(0.0, 1.0, 2.0, 3.0, 0.5)\n"
		"\tif is_nan(x) or is_inf(x) or is_finite(x) or is_zero_approx(x) or is_equal_approx(x, y):\n"
		"\t\tt = t + 1.0\n"
		"\tt = t + len(str(x))\n"
		"\treturn t\n";

	// Both Variant layouts: a global's arguments and result are Variants, and
	// the stride between two of them differs between the two builds.
	for (bool double_precision : { false, true }) {
		IRProgram ir = compile_to_ir(source, true);
		RISCVCodeGen riscv { VariantLayout(double_precision) };
		const std::vector<uint8_t> code = riscv.generate(ir);
		assert(!code.empty());
		assert(code.size() % 4 == 0);
	}

	std::cout << "  ✓ every global compiles to RISC-V, in both Variant layouts" << std::endl;
}

int main() {
	std::cout << "=== Global Function Tests ===" << std::endl << std::endl;

	test_the_table_is_consistent();
	test_print_is_still_print();
	test_every_other_global_is_one_opcode();
	test_numeric_dispatch_resolves_when_the_types_are_known();
	test_numeric_dispatch_survives_to_the_backend();
	test_int_arguments_widen_for_float_forms();
	test_variadic_min_and_max_fold();
	test_a_global_result_is_typed();
	test_only_print_has_a_side_effect();
	test_arity_is_checked_by_name();
	test_a_local_function_wins();
	test_host_globals_are_rejected_by_the_interpreter();
	test_integer_forms();
	test_numeric_forms_keep_the_type();
	test_float_forms();
	test_predicates();
	test_optimizing_does_not_change_the_answer();
	test_every_global_reaches_riscv();

	std::cout << std::endl << "All global function tests passed!" << std::endl;
	return 0;
}
