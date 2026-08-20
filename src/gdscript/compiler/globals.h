#pragma once
// -= GDScript's global functions, in one table =-
//
// `print`, `abs`, `sin`, `clamp`, `str` and the rest are not methods on the
// owner node: a call to one cannot be lowered to `self.print(...)`, because the
// Node has no such method and Godot drops the call without a word. They are
// @GlobalScope functions, and the compiler has to know each one.
//
// Before this table there was one: `is_global_function()` returned true for
// "print" and `gen_global_function()` had an `if` for it. Every function added
// that way costs an entry in two places that must agree, a hand-written arity
// check, a hand-written result type, and a new opcode. So instead every global
// is one row here, saying what it is called, how many arguments it takes, what
// it returns, and which of a handful of lowerings performs it:
//
//   PRINT     ECALL_PRINT, the one global with a side effect
//   INT_OP    integer arithmetic, emitted inline
//   FLOAT_OP  double arithmetic, emitted inline
//   SYSCALL   double arithmetic performed by the host (ECALL_UTILITY)
//   NUMERIC   int_form when every argument is an integer at run time,
//             float_form otherwise -- GDScript's `abs(2)` is 2 and `abs(2.0)`
//             is 2.0, and the two are different Variants
//   HOST      needs the host's Variant API: str() and len()
//
// Everything except print lowers to a single opcode, GLOBAL_CALL, carrying the
// GlobalFn as an immediate. Adding a function is a row here, a case in
// globals.cpp's evaluator, and -- for a SYSCALL -- a case in the host's
// api_utility(). It is never a new opcode.
//
// -= What is in here and what is not =-
//
// Only functions that are safe to call and need no scrutiny: pure arithmetic
// on numbers, and the two Variant queries str() and len(). Nothing that
// reaches the scene tree, loads a resource, or depends on engine state. The
// randomness family (randi, randf, ...) is left out for a different reason: it
// would make a program's result depend on host state, which the differential
// test and the optimizer-invariance test both rely on not happening.
//
// -= Where the compiler differs from Godot =-
//
// Godot's abs(), floor(), min() and friends also accept vectors, and raise a
// call error on a Variant they do not accept. Here a non-numeric argument
// converts the way Variant::operator double() would (a bool by its value, and
// anything else as zero) instead of raising. Vectors are not supported.
#include <cstddef>
#include <cstdint>
#include <string>

namespace gdscript {

// -= ECALL_UTILITY, mirrored from src/syscalls.h =-
//
// The host performs a SYSCALL global through ECALL_UTILITY (500 + 49) with the
// op below in a0. Floating-point arguments arrive in fa0-fa4 and the result
// leaves in fa0; STR and LEN instead take a1 = result Variant, a2 = argument
// array, a3 = argument count.
//
// These numbers are part of the guest ABI: an existing number may never change
// its meaning, so a new op goes on the end. The compiler cannot include
// src/syscalls.h -- it is also built standalone, and cross-compiled to run
// inside the sandbox -- so the values are repeated here, the way the syscall
// numbers already are elsewhere in the backend.
enum UtilityOp : int16_t {
	UTILITY_STR = 0,
	UTILITY_LEN = 1,

	// Unary: fa0
	UTILITY_FLOOR = 2,
	UTILITY_CEIL = 3,
	UTILITY_ROUND = 4,
	UTILITY_SIGN = 5,
	UTILITY_SIN = 6,
	UTILITY_COS = 7,
	UTILITY_TAN = 8,
	UTILITY_ASIN = 9,
	UTILITY_ACOS = 10,
	UTILITY_ATAN = 11,
	UTILITY_SINH = 12,
	UTILITY_COSH = 13,
	UTILITY_TANH = 14,
	UTILITY_ASINH = 15,
	UTILITY_ACOSH = 16,
	UTILITY_ATANH = 17,
	UTILITY_EXP = 18,
	UTILITY_LOG = 19,
	UTILITY_DEG_TO_RAD = 20,
	UTILITY_RAD_TO_DEG = 21,
	UTILITY_LINEAR_TO_DB = 22,
	UTILITY_DB_TO_LINEAR = 23,
	UTILITY_IS_NAN = 24,
	UTILITY_IS_INF = 25,
	UTILITY_IS_FINITE = 26,
	UTILITY_IS_ZERO_APPROX = 27,

	// Binary: fa0, fa1
	UTILITY_ATAN2 = 28,
	UTILITY_POW = 29,
	UTILITY_FMOD = 30,
	UTILITY_FPOSMOD = 31,
	UTILITY_SNAPPED = 32,
	UTILITY_IS_EQUAL_APPROX = 33,
	UTILITY_ANGLE_DIFFERENCE = 34,
	UTILITY_PINGPONG = 35,

	// Ternary: fa0, fa1, fa2
	UTILITY_LERP = 36,
	UTILITY_INVERSE_LERP = 37,
	UTILITY_SMOOTHSTEP = 38,
	UTILITY_MOVE_TOWARD = 39,
	UTILITY_LERP_ANGLE = 40,
	UTILITY_ROTATE_TOWARD = 41,
	UTILITY_WRAP = 42,

	// Five arguments: fa0 - fa4
	UTILITY_REMAP = 43,
	UTILITY_CUBIC_INTERPOLATE = 44,
	UTILITY_BEZIER_INTERPOLATE = 45,
	UTILITY_BEZIER_DERIVATIVE = 46,

	UTILITY_OP_COUNT,
};

// The most floating-point arguments any utility op takes. They travel in
// fa0-fa4, which is why this is five and not more.
static constexpr size_t UTILITY_MAX_FLOAT_ARGS = 5;

// One global function. The name is the GDScript spelling; everything else says
// how to compile a call to it.
enum class GlobalFn : int16_t {
	PRINT,

	// -= Type-preserving dispatchers (GlobalKind::NUMERIC) =-
	ABS,
	SIGN,
	FLOOR,
	CEIL,
	ROUND,
	MIN,
	MAX,
	CLAMP,
	SNAPPED,
	WRAP,

	// -= Integer forms (GlobalKind::INT_OP) =-
	ABSI,
	SIGNI,
	MINI,
	MAXI,
	CLAMPI,
	WRAPI,
	POSMOD,
	// floor(), ceil() and round() of an integer are that integer.
	INT_IDENTITY,

	// -= Float forms emitted inline (GlobalKind::FLOAT_OP) =-
	//
	// Only operations that are a single exact IEEE-754 primitive live here.
	// Anything that computes -- even `a + t * (b - a)` -- goes to the host
	// instead, because the C++ the interpreter and the differential harness run
	// is free to contract it into a fused multiply-add and the emitted RISC-V
	// is not, and then the two disagree in the last bit.
	ABSF,
	MINF,
	MAXF,
	CLAMPF,
	SQRT,

	// -= Performed by the host (GlobalKind::SYSCALL) =-
	SIGNF,
	FLOORF,
	CEILF,
	ROUNDF,
	FLOORI,
	CEILI,
	ROUNDI,
	SNAPPEDF,
	SNAPPEDI,
	WRAPF,
	SIN,
	COS,
	TAN,
	ASIN,
	ACOS,
	ATAN,
	ATAN2,
	SINH,
	COSH,
	TANH,
	ASINH,
	ACOSH,
	ATANH,
	EXP,
	LOG,
	POW,
	FMOD,
	FPOSMOD,
	DEG_TO_RAD,
	RAD_TO_DEG,
	LINEAR_TO_DB,
	DB_TO_LINEAR,
	LERP,
	LERPF,
	INVERSE_LERP,
	REMAP,
	SMOOTHSTEP,
	MOVE_TOWARD,
	LERP_ANGLE,
	ANGLE_DIFFERENCE,
	ROTATE_TOWARD,
	PINGPONG,
	CUBIC_INTERPOLATE,
	BEZIER_INTERPOLATE,
	BEZIER_DERIVATIVE,
	IS_NAN,
	IS_INF,
	IS_FINITE,
	IS_ZERO_APPROX,
	IS_EQUAL_APPROX,

	// -= The host's Variant API (GlobalKind::HOST) =-
	STR,
	LEN,
};

// How a call is performed. See the header comment.
enum class GlobalKind : uint8_t {
	PRINT,
	NUMERIC,
	INT_OP,
	FLOAT_OP,
	SYSCALL,
	HOST,
};

// The Variant type a call evaluates to.
enum class GlobalResult : uint8_t {
	NIL,
	BOOL,
	INT,
	FLOAT,
	STRING,
	// Follows the chosen form: NUMERIC entries only.
	NUMERIC,
};

struct GlobalFunction {
	const char* name;
	GlobalFn fn;
	GlobalKind kind;
	uint8_t min_args;
	uint8_t max_args;
	GlobalResult result;
	// SYSCALL only: the ECALL_UTILITY op, and how many of fa0-fa4 it reads.
	int16_t utility_op;
	uint8_t float_args;
	// NUMERIC only: the entry to use when every argument is an integer, and the
	// entry to use otherwise.
	GlobalFn int_form;
	GlobalFn float_form;
};

// The global function `name`, or nullptr when GDScript has no such global (or
// the compiler does not implement it, which is the same thing to a caller: the
// call then goes through the normal self-call path).
const GlobalFunction* find_global_function(const std::string& name);

// The row for a GlobalFn. Every GlobalFn has one, including the forms that no
// GDScript name maps to directly (INT_IDENTITY).
const GlobalFunction& global_function(GlobalFn fn);

// -= Evaluation =-
//
// The meaning of every global, as C++, in one place. The IR interpreter
// evaluates a GLOBAL_CALL through these, and so does the differential
// harness's ECALL_UTILITY shim -- so the interpreter and the machine cannot
// disagree about what `smoothstep` means. The host's api_utility() implements
// the same formulas against Godot's Math:: for the SYSCALL ops.

// A SYSCALL op, given fa0-fa4. Unused arguments are ignored, so a caller that
// does not know the op's arity may pass all five.
double eval_utility_op(int16_t utility_op, const double args[UTILITY_MAX_FLOAT_ARGS]);

// An INT_OP form. `count` must be within the entry's arity.
int64_t eval_global_int(GlobalFn fn, const int64_t* args, size_t count);

// A FLOAT_OP or SYSCALL form.
double eval_global_float(GlobalFn fn, const double* args, size_t count);

// Whether a call to `fn` with all-integer arguments produces an integer. Used
// to resolve a NUMERIC entry: `min(1, 2)` is an int and `min(1, 2.0)` is not.
GlobalFn resolve_numeric_form(const GlobalFunction& info, bool all_integer);

} // namespace gdscript
