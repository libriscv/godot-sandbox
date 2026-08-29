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
#include "../syscall_numbers.h"
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
			names.push_back(static_cast<GlobalFn>(instr.operands.at(1).immediate()));
		}
	}
	return names;
}

// Whether every GLOBAL_CALL says its arguments are already the type its form
// works in, which is what lets the backend skip the run-time type test.
static bool all_calls_typed(const IRFunction& func) {
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::GLOBAL_CALL && instr.operands.at(2).immediate() == 0) {
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

// A zero-argument GLOBAL_CALL naming `fn`, which is all the purity question
// needs: which global a call calls is an operand, not the opcode.
static IRInstruction global_call_instr(GlobalFn fn) {
	IRInstruction instr(IROpcode::GLOBAL_CALL);
	instr.operands.push_back(IRValue::reg(1));
	instr.operands.push_back(IRValue::imm(static_cast<int64_t>(fn)));
	instr.operands.push_back(IRValue::imm(1));
	instr.operands.push_back(IRValue::imm(0));
	return instr;
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
		"cubic_interpolate", "cubic_interpolate_angle",
		"cubic_interpolate_in_time", "cubic_interpolate_angle_in_time",
		"bezier_interpolate", "bezier_derivative",
		"is_nan", "is_inf", "is_finite", "is_zero_approx", "is_equal_approx",
		"str", "len",
		"hash", "var_to_str", "str_to_var", "var_to_bytes", "bytes_to_var",
		"type_string", "type_convert", "error_string", "is_same", "rand_from_seed",
		"ease", "step_decimals", "nearest_po2",
		"int", "float", "bool", "String",
		"randi", "randf", "randi_range", "randf_range", "randfn", "randomize", "seed",
		"prints", "printt", "printraw", "print_rich", "printerr",
		"print_verbose", "push_error", "push_warning",
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
			assert(info->float_args <= UTILITY_MAX_FLOAT_ARGS);
			// A syscall form takes exactly as many arguments as it passes.
			assert(info->min_args == info->float_args && info->max_args == info->float_args);
		}
		if (info->kind == GlobalKind::SYSCALL_INT) {
			assert(info->utility_op >= 0 && info->utility_op < UTILITY_OP_COUNT);
			// a1-a3 is all there is to pass integers in.
			assert(info->min_args == info->max_args);
			assert(info->max_args <= UTILITY_MAX_INT_ARGS);
			// The answer comes back in a0, as an integer or a boolean.
			assert(info->result == GlobalResult::INT || info->result == GlobalResult::BOOL);
		}
		if (info->kind == GlobalKind::PRINT) {
			// Each output global must have a valid, unique channel.
			assert(info->utility_op >= 0 && info->utility_op < int16_t(Print_Channel::CHANNEL_COUNT));
			assert(info->result == GlobalResult::NIL);
		}
		if (info->kind == GlobalKind::CAST) {
			// One argument, a host op to perform it with, and an inline form
			// for the case where the argument is already a number or a bool.
			assert(info->min_args == 1 && info->max_args == 1);
			assert(info->utility_op >= 0 && info->utility_op < UTILITY_OP_COUNT);
			const GlobalFunction& inline_form = global_function(info->int_form);
			assert(inline_form.result == info->result);
			assert(resolve_cast_form(*info, Variant::INT) == info->int_form);
			assert(resolve_cast_form(*info, Variant::FLOAT) == info->int_form);
			assert(resolve_cast_form(*info, Variant::BOOL) == info->int_form);
			// A String is the reason the host has to be asked at all.
			assert(resolve_cast_form(*info, Variant::STRING) == info->fn);
			assert(resolve_cast_form(*info, IRInstruction::TypeHint_NONE) == info->fn);
		}
		// The random draws are the only rows with a side effect, and print()
		// is not in this table's impure column because it has its own opcode.
		// rand_from_seed() shares the prefix but not the property: it draws
		// from the seed it is handed, not from the project's generator.
		const bool is_random = (std::string(info->name).rfind("rand", 0) == 0 &&
			std::string(info->name) != "rand_from_seed") || std::string(info->name) == "seed";
		assert(info->impure == is_random);
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

	// No two output globals share a channel.
	{
		bool seen[int(Print_Channel::CHANNEL_COUNT)] = {};
		for (const char* name : NAMES) {
			const GlobalFunction* info = find_global_function(name);
			if (info->kind != GlobalKind::PRINT) {
				continue;
			}
			assert(!seen[info->utility_op]);
			seen[info->utility_op] = true;
		}
	}

	// A name GDScript does not have is not a global, and neither is the
	// internal form that has no GDScript name.
	assert(find_global_function("no_such_global") == nullptr);
	assert(find_global_function("randomize") != nullptr);
	assert(find_global_function("seed") != nullptr);
	assert(find_global_function("randomize")->unrestricted_only);
	assert(find_global_function("seed")->unrestricted_only);
	assert(find_global_function(".int_identity") != nullptr);
	assert(find_global_function(".float_identity") != nullptr);
	assert(find_global_function(".booleanize") != nullptr);

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

static void test_the_output_channels_are_one_opcode() {
	// All output globals lower to PRINT with channel, never VCALL.
	static const struct { const char* call; Print_Channel channel; } CASES[] = {
		{ "print(1)",            Print_Channel::PRINT },
		{ "prints(1, 2)",        Print_Channel::SPACED },
		{ "printt(1, 2)",        Print_Channel::TABBED },
		{ "printraw(1)",         Print_Channel::RAW },
		{ "print_rich(1)",       Print_Channel::RICH },
		{ "printerr(1)",         Print_Channel::ERROR },
		{ "print_verbose(1)",    Print_Channel::VERBOSE },
		{ "push_error(1)",       Print_Channel::PUSH_ERROR },
		{ "push_warning(1)",     Print_Channel::PUSH_WARNING },
	};

	for (const auto& one : CASES) {
		IRProgram ir = compile_to_ir(
			std::string("func test():\n\t") + one.call + "\n\treturn 0\n");
		const IRFunction& func = find_function(ir, "test");
		assert(count_opcode(func, IROpcode::PRINT) == 1);
		assert(count_opcode(func, IROpcode::VCALL) == 0);
		for (const auto& instr : func.instructions) {
			if (instr.opcode == IROpcode::PRINT) {
				assert(instr.operands[1].immediate() == int64_t(one.channel));
			}
		}
	}

	// push_error() and push_warning() require an argument.
	assert(!compile_error("func test():\n\tpush_error()\n").empty());
	assert(!compile_error("func test():\n\tpush_warning()\n").empty());

	std::cout << "  ✓ every output channel is one PRINT" << std::endl;
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

static void test_only_print_and_the_random_draws_have_side_effects() {
	// No global except print() and the random draws does anything a program
	// can observe beyond its result, so every other one is free to be moved or
	// dropped. print() is not: it prints. randi() is not: it advances the
	// generator the whole project draws from.
	//
	// Dead-code elimination is deliberately narrower than purity -- it only
	// removes a pure instruction that reads no register at all -- so a call
	// with arguments still survives it. What matters here is that the
	// metadata says the truth, because that is what every pass reads.
	assert(ir_is_pure(IROpcode::GLOBAL_CALL));
	assert(!ir_is_pure(IROpcode::PRINT));

	// The opcode is pure; a particular GLOBAL_CALL need not be. Which global
	// it calls is an operand, so a pass has to ask about the instruction.
	assert(ir_instruction_is_pure(global_call_instr(GlobalFn::ABSI)));
	assert(!ir_instruction_is_pure(global_call_instr(GlobalFn::RANDI)));
	assert(!ir_instruction_is_pure(global_call_instr(GlobalFn::RANDF)));

	// print() survives the optimizer whatever is done with its result.
	IRProgram printed = compile_to_ir(
		"func test():\n"
		"\tvar unused = print(1)\n"
		"\treturn 1\n",
		true);
	assert(count_opcode(find_function(printed, "test"), IROpcode::PRINT) == 1);

	std::cout << "  ✓ only print() and the random draws have side effects" << std::endl;
}

static void test_random_calls_survive_the_optimizer() {
	// A randi() whose result nobody reads still has to be called: it advances
	// the generator every other draw in the project shares. Dead-code
	// elimination deletes a pure instruction that defines an unread register
	// and reads nothing itself, which is exactly the shape of this call, so
	// this is the case that asking the opcode instead of the instruction would
	// get wrong.
	IRProgram program;
	IRFunction func;
	func.name = "test";
	func.max_registers = 3;
	// A dead pure load, so the pass is known to be running at all.
	func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(1), IRValue::imm(7));
	func.instructions.push_back(global_call_instr(GlobalFn::RANDI));
	func.instructions.back().operands[0] = IRValue::reg(2); // and nothing reads r2
	func.instructions.back().type_hint = Variant::INT;
	func.instructions.emplace_back(IROpcode::LOAD_IMM, IRValue::reg(0), IRValue::imm(5));
	func.instructions.emplace_back(IROpcode::RETURN);
	program.functions.push_back(func);

	IROptimizer optimizer;
	optimizer.optimize(program);

	const IRFunction& optimized = find_function(program, "test");
	assert(count_opcode(optimized, IROpcode::GLOBAL_CALL) == 1);
	// The dead load is gone, so the survival above is not simply a pass that
	// did nothing.
	int dead_loads = 0;
	for (const auto& instr : optimized.instructions) {
		if (instr.opcode == IROpcode::LOAD_IMM && instr.operands.at(0).reg_index() == 1) {
			dead_loads++;
		}
	}
	assert(dead_loads == 0);

	std::cout << "  ✓ an unread randi() survives the optimizer" << std::endl;
}

// -= The type constructors =-

static void test_type_constructors_lower_inline_when_the_type_is_known() {
	// int(x)/float(x) of a known numeric value is a CONVERT, not an identity
	// GLOBAL_CALL. bool(x) still uses the inline BOOLEANIZE global form.
	IRProgram ir = compile_to_ir(
		"func test():\n"
		"\tvar i: int = 2\n"
		"\tvar f: float = 2.5\n"
		"\treturn int(f) + float(i) + bool(i)\n");
	const std::vector<GlobalFn> called = called_globals(find_function(ir, "test"));
	assert(called.size() == 1);
	assert(called[0] == GlobalFn::BOOLEANIZE);
	assert(count_opcode(find_function(ir, "test"), IROpcode::CONVERT) >= 2);

	// An argument whose type is not known could be a String, and only Godot
	// knows that int("42") is 42, so the call stays a CAST for the host.
	IRProgram untyped = compile_to_ir(
		"func convert(x):\n"
		"\treturn int(x) + float(x) + bool(x)\n"
		"func test():\n"
		"\treturn convert(1)\n");
	const std::vector<GlobalFn> host = called_globals(find_function(untyped, "convert"));
	assert(host.size() == 3);
	assert(host[0] == GlobalFn::TO_INT);
	assert(host[1] == GlobalFn::TO_FLOAT);
	assert(host[2] == GlobalFn::TO_BOOL);

	// String(x) is str() of one argument, and has no inline form at all.
	IRProgram stringified = compile_to_ir(
		"func test():\n"
		"\treturn String(2)\n");
	const std::vector<GlobalFn> as_string = called_globals(find_function(stringified, "test"));
	assert(as_string.size() == 1);
	assert(as_string[0] == GlobalFn::TO_STRING);

	// And a call to one is not a VCALL on the owner node, which is what the
	// self-call fallback used to make of int(x): a call Godot drops in silence.
	assert(count_opcode(find_function(ir, "test"), IROpcode::VCALL) == 0);
	assert(count_opcode(find_function(untyped, "convert"), IROpcode::VCALL) == 0);

	std::cout << "  ✓ the type constructors lower inline when the type is known" << std::endl;
}

static void test_constants_fold_through_casts_and_inline_globals() {
	IRProgram ir = compile_to_ir(
		"func test():\n"
		"\treturn abs(-7) + int(2.9)\n", true);
	const IRFunction func = find_function(ir, "test");
	assert(count_opcode(func, IROpcode::GLOBAL_CALL) == 0);
	assert(count_opcode(func, IROpcode::CONVERT) == 0);
	assert(count_opcode(func, IROpcode::ADD) == 0);
	bool found_nine = false;
	for (const IRInstruction& instr : func.instructions) {
		found_nine = found_nine || (instr.opcode == IROpcode::LOAD_IMM &&
			instr.operands[1].immediate() == 9);
	}
	assert(found_nine);
	std::cout << "  ✓ constants fold through casts and inline globals" << std::endl;
}

static void test_type_constructors_compute() {
	assert(run_int("func test():\n\tvar f: float = 2.9\n\treturn int(f)\n") == 2);
	assert(run_int("func test():\n\tvar f: float = -2.9\n\treturn int(f)\n") == -2);
	assert(run_int("func test():\n\tvar i: int = 7\n\treturn int(i)\n") == 7);
	assert(close_enough(run_float("func test():\n\tvar i: int = 7\n\treturn float(i)\n"), 7.0));
	assert(close_enough(run_float("func test():\n\tvar f: float = 2.5\n\treturn float(f)\n"), 2.5));
	assert(run_bool("func test():\n\tvar i: int = 7\n\treturn bool(i)\n") == true);
	assert(run_bool("func test():\n\tvar i: int = 0\n\treturn bool(i)\n") == false);
	assert(run_bool("func test():\n\tvar f: float = 0.5\n\treturn bool(f)\n") == true);
	assert(run_bool("func test():\n\tvar f: float = 0.0\n\treturn bool(f)\n") == false);

	// Through a parameter, where the compiler does not know the type and the
	// interpreter converts the value it was actually handed.
	assert(run_int(
		"func convert(x):\n"
		"\treturn int(x)\n"
		"func test():\n"
		"\treturn convert(2.9)\n") == 2);
	assert(run_bool(
		"func convert(x):\n"
		"\treturn bool(x)\n"
		"func test():\n"
		"\treturn convert(\"a string is true when it is not empty\")\n") == true);

	// int() of a String is Godot's parse, and the interpreter says so rather
	// than inventing a second definition of what int("42") means.
	bool threw = false;
	try {
		run("func convert(x):\n\treturn int(x)\nfunc test():\n\treturn convert(\"42\")\n");
	} catch (const std::exception& e) {
		threw = true;
		assert(mentions(e.what(), "int"));
	}
	assert(threw);

	std::cout << "  ✓ the type constructors convert" << std::endl;
}

// -= Randomness =-

static void test_random_is_not_evaluated_by_the_interpreter() {
	// There is no answer for the interpreter to give: the host's generator has
	// the state these read, and a number of the interpreter's own choosing is
	// a number the machine would not have produced -- which every test that
	// compares the two would read as a miscompilation.
	for (const char* source : {
			"func test():\n\treturn randi()\n",
			"func test():\n\treturn randf()\n",
			"func test():\n\treturn randi_range(1, 6)\n" }) {
		bool threw = false;
		try {
			run(source);
		} catch (const std::exception& e) {
			threw = true;
			assert(mentions(e.what(), "random"));
		}
		assert(threw);
	}

	// The arity check still names them.
	assert(mentions(compile_error("func test():\n\treturn randi(1)\n"),
		"randi() takes 0 arguments, got 1"));
	assert(mentions(compile_error("func test():\n\treturn randf_range(1.0)\n"),
		"randf_range() takes 2 arguments, got 1"));

	std::cout << "  ✓ the random draws need the host" << std::endl;
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
		"\tt = t + cubic_interpolate_angle(1.0, 2.0, 0.0, 3.0, 0.5)\n"
		"\tt = t + cubic_interpolate_in_time(1.0, 2.0, 0.0, 3.0, 0.5, 1.0, -0.5, 1.5)\n"
		"\tt = t + cubic_interpolate_angle_in_time(1.0, 2.0, 0.0, 3.0, 0.5, 1.0, -0.5, 1.5)\n"
		"\tt = t + bezier_interpolate(0.0, 1.0, 2.0, 3.0, 0.5) + bezier_derivative(0.0, 1.0, 2.0, 3.0, 0.5)\n"
		"\tif is_nan(x) or is_inf(x) or is_finite(x) or is_zero_approx(x) or is_equal_approx(x, y):\n"
		"\t\tt = t + 1.0\n"
		"\tt = t + len(str(x))\n"
		"\tt = t + int(x) + float(x) + len(String(x))\n"
		"\tt = t + int(y) + float(y)\n"
		"\tif bool(x) and bool(1):\n"
		"\t\tt = t + 1.0\n"
		"\tt = t + randi() + randf() + randi_range(1, 6) + randf_range(1.0, 2.0) + randfn(0.0, 1.0)\n"
		"\tt = t + ease(0.5, 2.0) + step_decimals(0.25) + nearest_po2(100)\n"
		"\tt = t + hash(x) + len(var_to_str(x)) + len(type_string(1)) + len(error_string(1))\n"
		"\tif is_same(x, y):\n"
		"\t\tt = t + 1.0\n"
		"\tt = t + int(type_convert(x, 2)) + len(str(str_to_var(\"1\")))\n"
		"\tt = t + len(var_to_bytes(x)) + int(bytes_to_var(var_to_bytes(1)))\n"
		"\tt = t + len(rand_from_seed(12345))\n"
		"\tprints(x, y)\n"
		"\tprintt(x, y)\n"
		"\tprintraw(x)\n"
		"\tprint_rich(x)\n"
		"\tprinterr(x)\n"
		"\tprint_verbose(x)\n"
		"\tpush_error(x)\n"
		"\tpush_warning(x)\n"
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

// -= @GlobalScope enumerations =-

static void test_a_global_enum_member_is_an_immediate() {
	// Side.SIDE_LEFT is a compile-time integer, the same as the bare
	// SIDE_LEFT constant beside it. Nothing reaches the engine.
	const IRProgram ir = compile_to_ir(
		"func test():\n"
		"\treturn Side.SIDE_BOTTOM\n");
	const IRFunction& func = find_function(ir, "test");
	assert(count_opcode(func, IROpcode::VCALL) == 0);
	assert(count_opcode(func, IROpcode::CALL_SYSCALL) == 0);
	assert(run_int("func test():\n\treturn Side.SIDE_BOTTOM\n") == 3);
	assert(run_int("func test():\n\treturn Corner.CORNER_BOTTOM_LEFT\n") == 3);
	assert(run_int("func test():\n\treturn Orientation.VERTICAL\n") == 1);
	assert(run_int("func test():\n\treturn ClockDirection.COUNTERCLOCKWISE\n") == 1);
	assert(run_int("func test():\n\treturn Error.ERR_FILE_NOT_FOUND\n") == 7);
	assert(run_int("func test():\n\treturn Key.KEY_A\n") == 65);
	assert(run_int("func test():\n\treturn KeyModifierMask.KEY_MASK_SHIFT\n") == 33554432);
	// Nested under Variant: the only two whose name carries a dot, and the
	// only members with no bare @GlobalScope spelling.
	assert(run_int("func test():\n\treturn Variant.Type.TYPE_INT\n") == 2);
	assert(run_int("func test():\n\treturn Variant.Operator.OP_ADD\n") == 6);

	std::cout << "  ✓ a global enum member folds to an integer immediate" << std::endl;
}

static void test_a_global_enum_agrees_with_the_bare_constant() {
	// SIDE_LEFT and Side.SIDE_LEFT are two spellings of one value in GDScript,
	// and here they come from two tables. Variant.Type and Variant.Operator
	// are exempt: TYPE_* is a @GlobalScope constant, OP_* is not a name
	// GDScript can reach at all without the qualifier.
	size_t checked = 0;
	for (size_t i = 0; i < global_enum_value_count(); i++) {
		const GlobalConstant* bare = find_global_constant(global_enum_value_name(i));
		if (bare == nullptr) {
			assert(std::string(global_enum_value_enum(i)).rfind("Variant.", 0) == 0);
			continue;
		}
		assert(!bare->is_float);
		assert(bare->int_value == global_enum_value(i));
		checked++;
	}
	// The two Variant enums aside, every member has a bare spelling.
	assert(checked > 400);

	std::cout << "  ✓ every enum member matches the @GlobalScope constant of the same name"
		<< std::endl;
}

static void test_a_declared_enum_shadows_the_global_one() {
	assert(run_int(
		"enum Side { SIDE_LEFT = 99 }\n"
		"func test():\n"
		"\treturn Side.SIDE_LEFT\n") == 99);

	std::cout << "  ✓ a script's own enum shadows the @GlobalScope one" << std::endl;
}

static void test_a_global_enum_is_an_int_type() {
	// `var s: Side` is an int slot, so the arithmetic on it is typed INT --
	// which is what lets the backend emit an add instead of the host's
	// Variant evaluator.
	const IRProgram ir = compile_to_ir(
		"func test(n : int):\n"
		"\tvar s: Side = n\n"
		"\treturn s + 1\n");
	const IRFunction& func = find_function(ir, "test");
	bool typed_add = false;
	for (const auto& instr : func.instructions) {
		if (instr.opcode == IROpcode::ADD) {
			typed_add = instr.type_hint == Variant::INT;
		}
	}
	assert(typed_add);
	assert(run_int(
		"func test():\n"
		"\tvar s: Side = SIDE_TOP\n"
		"\treturn s + 1\n") == 2);

	std::cout << "  ✓ a global enum names the int type" << std::endl;
}

static void test_a_global_enum_is_not_a_value() {
	// GDScript's own words: a native enum is not a Dictionary and "cannot be
	// used on its own". Without this it fell through to a property read on the
	// owner node and answered null.
	assert(mentions(compile_error(
		"func test():\n"
		"\treturn Side\n"), "cannot be used on its own"));
	assert(mentions(compile_error(
		"func test():\n"
		"\treturn Side.keys()\n"), "cannot be used on its own"));
	assert(mentions(compile_error(
		"func test():\n"
		"\treturn Side.SIDE_NOPE\n"), "no member named 'SIDE_NOPE'"));

	std::cout << "  ✓ a global enum is a type, not a value" << std::endl;
}

static void test_a_local_name_wins_over_a_global_enum() {
	assert(run_int(
		"func test():\n"
		"\tvar Side = 7\n"
		"\treturn Side\n") == 7);

	std::cout << "  ✓ a local of the same name shadows a global enum" << std::endl;
}

int main() {
	std::cout << "=== Global Function Tests ===" << std::endl << std::endl;

	test_the_table_is_consistent();
	test_print_is_still_print();
	test_the_output_channels_are_one_opcode();
	test_every_other_global_is_one_opcode();
	test_numeric_dispatch_resolves_when_the_types_are_known();
	test_numeric_dispatch_survives_to_the_backend();
	test_int_arguments_widen_for_float_forms();
	test_variadic_min_and_max_fold();
	test_a_global_result_is_typed();
	test_only_print_and_the_random_draws_have_side_effects();
	test_random_calls_survive_the_optimizer();
	test_type_constructors_lower_inline_when_the_type_is_known();
	test_constants_fold_through_casts_and_inline_globals();
	test_type_constructors_compute();
	test_random_is_not_evaluated_by_the_interpreter();
	test_arity_is_checked_by_name();
	test_a_local_function_wins();
	test_host_globals_are_rejected_by_the_interpreter();
	test_integer_forms();
	test_numeric_forms_keep_the_type();
	test_float_forms();
	test_predicates();
	test_optimizing_does_not_change_the_answer();
	test_every_global_reaches_riscv();
	test_a_global_enum_member_is_an_immediate();
	test_a_global_enum_agrees_with_the_bare_constant();
	test_a_declared_enum_shadows_the_global_one();
	test_a_global_enum_is_an_int_type();
	test_a_global_enum_is_not_a_value();
	test_a_local_name_wins_over_a_global_enum();

	std::cout << std::endl << "All global function tests passed!" << std::endl;
	return 0;
}
