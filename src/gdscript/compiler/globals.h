#pragma once
// @GlobalScope functions (print, abs, sin, clamp, str, ...) in one table.
//
// Each row declares name, arity, result type and lowering kind:
//   PRINT       ECALL_PRINT, side-effecting
//   INT_OP      inline 64-bit integer arithmetic
//   FLOAT_OP    inline double arithmetic (single IEEE-754 primitives only)
//   SYSCALL     ECALL_UTILITY with doubles in fa0-fa7, result in fa0
//   SYSCALL_INT ECALL_UTILITY with int64s in a1-a3, result in a0
//   NUMERIC     int_form or float_form chosen at run time by argument type
//   CAST        inline when argument is numeric/bool, host otherwise
//   HOST        needs host Variant API (str, len, String)
//
// All except PRINT lower to a single IR opcode: GLOBAL_CALL with GlobalFn.
// Adding a function: one row here, one case in globals.cpp's evaluator,
// and for SYSCALL one case in the host's api_utility().
//
// Functions that mutate engine state may be marked unrestricted-only. The
// compiler then refuses them when building for a restricted Sandbox.
//
// Random draws are impure (DCE must not delete them) and refused by the
// IR interpreter — both the differential and optimizer-invariance tests
// require deterministic answers.
//
// Non-numeric arguments convert via Variant::operator double() (bool by
// value, everything else as zero) rather than raising. Vectors unsupported.
#include "variant_types.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace gdscript {

// ECALL_UTILITY op numbers, mirrored from src/syscalls.h.
// ABI-stable: existing values never change; new ops append.
// Duplicated because this file also compiles standalone inside the sandbox.
enum UtilityOp : int16_t {
	UTILITY_STR = 0,
	UTILITY_LEN = 1,

	// Unary (fa0)
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

	// Binary (fa0, fa1)
	UTILITY_ATAN2 = 28,
	UTILITY_POW = 29,
	UTILITY_FMOD = 30,
	UTILITY_FPOSMOD = 31,
	UTILITY_SNAPPED = 32,
	UTILITY_IS_EQUAL_APPROX = 33,
	UTILITY_ANGLE_DIFFERENCE = 34,
	UTILITY_PINGPONG = 35,

	// Ternary (fa0, fa1, fa2)
	UTILITY_LERP = 36,
	UTILITY_INVERSE_LERP = 37,
	UTILITY_SMOOTHSTEP = 38,
	UTILITY_MOVE_TOWARD = 39,
	UTILITY_LERP_ANGLE = 40,
	UTILITY_ROTATE_TOWARD = 41,
	UTILITY_WRAP = 42,

	// Quinary (fa0-fa4)
	UTILITY_REMAP = 43,
	UTILITY_CUBIC_INTERPOLATE = 44,
	UTILITY_BEZIER_INTERPOLATE = 45,
	UTILITY_BEZIER_DERIVATIVE = 46,

	// Variant-in, Variant-out (same shape as STR/LEN): type constructors.
	UTILITY_TO_INT = 47,
	UTILITY_TO_FLOAT = 48,
	UTILITY_TO_BOOL = 49,

	// RANDF family: doubles in fa0-fa1. RANDI family: int64s in a1-a2, result in a0.
	UTILITY_RANDF = 50,
	UTILITY_RANDF_RANGE = 51,
	UTILITY_RANDFN = 52,
	UTILITY_RANDI = 53,
	UTILITY_RANDI_RANGE = 54,

	// fa0-fa4 arithmetic. step_decimals() result fits in double.
	UTILITY_EASE = 55,
	UTILITY_STEP_DECIMALS = 56,

	// int64 via a1/a0.
	UTILITY_NEAREST_PO2 = 57,

	// Variant in, Variant out (same shape as STR/LEN).
	UTILITY_HASH = 58,
	UTILITY_VAR_TO_STR = 59,
	UTILITY_STR_TO_VAR = 60,
	UTILITY_VAR_TO_BYTES = 61,
	UTILITY_BYTES_TO_VAR = 62,
	UTILITY_TYPE_STRING = 63,
	UTILITY_TYPE_CONVERT = 64,
	UTILITY_ERROR_STRING = 65,
	UTILITY_IS_SAME = 66,
	UTILITY_IS_INSTANCE_VALID = 69,

	UTILITY_CHAR = 67,
	UTILITY_ORD = 68,

	// fa0-fa4. Angular form of CUBIC_INTERPOLATE.
	UTILITY_CUBIC_INTERPOLATE_ANGLE = 70,

	// fa0-fa7: four values and the four times they sit at.
	UTILITY_CUBIC_INTERPOLATE_IN_TIME = 71,
	UTILITY_CUBIC_INTERPOLATE_ANGLE_IN_TIME = 72,

	// Variant in, Variant out (same shape as STR/LEN). Deterministic: the
	// seed comes in and the next one comes back, so the project's shared
	// generator is untouched and the call is pure.
	UTILITY_RAND_FROM_SEED = 73,

	// Shared project RNG mutation. Variant-shaped so void/NIL results and seed's
	// full int64 argument use the ordinary host Variant ABI.
	UTILITY_RANDOMIZE = 74,
	UTILITY_SEED = 75,
	UTILITY_IS_INSTANCE_OF = 76,

	UTILITY_OP_COUNT,
};

// Max float args (fa0-fa7).
static constexpr size_t UTILITY_MAX_FLOAT_ARGS = 8;

// Max integer args (a1-a3; a0 holds the op number).
static constexpr size_t UTILITY_MAX_INT_ARGS = 3;

// One global function — GDScript name + compilation metadata.
enum class GlobalFn : int16_t {
	PRINT,

	// Other output channels. PRINT + Print_Channel in utility_op.
	PRINTERR,
	PRINTRAW,
	PRINT_RICH,
	PRINT_VERBOSE,
	PRINTS,
	PRINTT,
	PUSH_ERROR,
	PUSH_WARNING,

	// NUMERIC dispatchers (type-preserving)
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

	// INT_OP forms
	ABSI,
	SIGNI,
	MINI,
	MAXI,
	CLAMPI,
	WRAPI,
	POSMOD,
	// floor/ceil/round of an integer: identity.
	INT_IDENTITY,

	// FLOAT_OP forms (inline). Only single IEEE-754 primitives — compound
	// expressions risk FMA contraction disagreement between C++ and RISC-V.
	ABSF,
	MINF,
	MAXF,
	CLAMPF,
	SQRT,

	// SYSCALL forms (host-performed)
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
	CUBIC_INTERPOLATE_ANGLE,
	CUBIC_INTERPOLATE_IN_TIME,
	CUBIC_INTERPOLATE_ANGLE_IN_TIME,
	BEZIER_INTERPOLATE,
	BEZIER_DERIVATIVE,
	IS_NAN,
	IS_INF,
	IS_FINITE,
	IS_ZERO_APPROX,
	IS_EQUAL_APPROX,
	EASE,
	STEP_DECIMALS,
	NEAREST_PO2,

	// HOST forms (Variant API)
	STR,
	LEN,
	HASH,
	VAR_TO_STR,
	STR_TO_VAR,
	VAR_TO_BYTES,
	BYTES_TO_VAR,
	TYPE_STRING,
	TYPE_CONVERT,
	ERROR_STRING,
	IS_SAME,
	IS_INSTANCE_VALID,
	IS_INSTANCE_OF,
	CHAR,
	ORD,
	// Deterministic, hence a HOST form rather than a random one.
	RAND_FROM_SEED,
	RANDOMIZE,
	SEED,

	// Randomness (SYSCALL / SYSCALL_INT). Impure: advances the project's shared RNG.
	RANDF,
	RANDF_RANGE,
	RANDFN,
	RANDI,
	RANDI_RANGE,

	// CAST forms (type constructors)
	TO_INT,
	TO_FLOAT,
	TO_BOOL,
	TO_STRING,

	// Inline forms for CAST when argument is numeric/bool.
	// int() reuses INT_IDENTITY — loading as int64 is the conversion.
	FLOAT_IDENTITY,
	BOOLEANIZE,
};

// How a call is performed.
enum class GlobalKind : uint8_t {
	PRINT,
	NUMERIC,
	INT_OP,
	FLOAT_OP,
	SYSCALL,
	SYSCALL_INT,
	CAST,
	HOST,
};

// Result Variant type.
enum class GlobalResult : uint8_t {
	NIL,
	BOOL,
	INT,
	FLOAT,
	STRING,
	// Follows chosen form (NUMERIC only).
	NUMERIC,
	// Untyped result; no type hint on the register.
	VARIANT,
};

struct GlobalFunction {
	const char* name;
	GlobalFn fn;
	GlobalKind kind;
	uint8_t min_args;
	uint8_t max_args;
	GlobalResult result;
	int16_t utility_op;  // SYSCALL: ECALL_UTILITY op number
	uint8_t float_args;  // SYSCALL: count of fa0-fa7 args
	// NUMERIC: forms for all-integer vs. mixed. CAST reuses int_form for
	// numeric/bool inline path; float_form unused for CAST.
	GlobalFn int_form;
	GlobalFn float_form;
	// Random family: must not be DCE'd or deduplicated.
	bool impure = false;
	// Mutates host/project state and is therefore refused under restrictions.
	bool unrestricted_only = false;
};

// nullptr when `name` is not a known global (caller falls through to self-call).
const GlobalFunction* find_global_function(const std::string& name);

// Positional access for editor completion. Names only; host forward-declares
// to avoid variant_types.h vs godot::Variant ambiguity.
size_t global_function_count();
// nullptr for internal lowering forms (leading '.'); skip when enumerating.
const char* global_function_name(size_t index);
uint8_t global_function_min_args(size_t index);
uint8_t global_function_max_args(size_t index);

// @GlobalScope constant. Folds to immediate; no syscall.
struct GlobalConstant {
	const char* name;
	bool is_float;
	int64_t int_value;
	double float_value;
};

// nullptr if not a @GlobalScope constant.
const GlobalConstant* find_global_constant(const std::string& name);

size_t global_constant_count();
const char* global_constant_name(size_t index);
bool global_constant_is_float(size_t index);
int64_t global_constant_int_value(size_t index);
double global_constant_float_value(size_t index);

// @GlobalScope enumeration member (Side.SIDE_LEFT, Error.ERR_BUSY). The enum
// itself is compile-time only: GDScript's native enums are not Dictionaries and
// have no keys()/values(), and naming one on its own is a parse error there --
// so a member is an integer immediate and nothing else reaches IR.
struct GlobalEnumValue {
	const char* enum_name;
	const char* name;
	int64_t value;
};

// nullptr when `enum_name` has no such member (or is not a global enum).
const GlobalEnumValue* find_global_enum_value(const std::string& enum_name, const std::string& name);

// True if `name` is a @GlobalScope enum. Dotted names (Variant.Type) included.
bool is_global_enum(const std::string& name);

// Positional access for the editor, and for the table's own consistency test.
size_t global_enum_value_count();
const char* global_enum_value_enum(size_t index);
const char* global_enum_value_name(size_t index);
int64_t global_enum_value(size_t index);

// Built-in type constant (Vector2.ZERO, Color.RED). Folded into MAKE_*.
struct BuiltinConstant {
	const char* type;
	const char* name;
	double components[4];
};

// Built-in type constant too large for the guest's inline Variant payload.
// Components are the flattened constructor arguments from extension_api.json;
// codegen groups them into vectors before asking Godot to construct the value.
struct HostConstant {
	const char* type;
	const char* name;
	uint8_t component_count;
	double components[16];
};

// nullptr if no such constant exists for `type`.
const BuiltinConstant* find_builtin_constant(const std::string& type, const std::string& name);

// nullptr if no host-constructed constant has this qualified name.
const HostConstant* find_host_constant(const std::string& type, const std::string& name);

// True if `type` has any built-in constants (distinguishes typo from unknown type).
bool has_builtin_constants(const std::string& type);

// Positional access for editor. Rows not contiguous by type; filter on name.
size_t builtin_constant_count();
const char* builtin_constant_type(size_t index);
const char* builtin_constant_name(size_t index);

// True when an engine class itself declares an enum with this name. Values
// remain ClassDB constants; this table only resolves the declaring namespace.
bool engine_class_declares_enum(const std::string& class_name, const std::string& enum_name);

// Unimplemented @GlobalScope name -> reason string, or nullptr if not a global.
const char* unimplemented_global_reason(const std::string& name);

// Row for a GlobalFn. Every enum value has one, including internal forms.
const GlobalFunction& global_function(GlobalFn fn);

// Evaluation — shared by the IR interpreter and the differential harness's
// ECALL_UTILITY shim, so the two cannot disagree.

// SYSCALL op evaluation. Unused fa slots ignored.
double eval_utility_op(int16_t utility_op, const double args[UTILITY_MAX_FLOAT_ARGS]);

// INT_OP form evaluation.
int64_t eval_global_int(GlobalFn fn, const int64_t* args, size_t count);

// FLOAT_OP or SYSCALL form evaluation.
double eval_global_float(GlobalFn fn, const double* args, size_t count);

// SYSCALL_INT: always throws — the host answers these.
int64_t eval_global_int_syscall(GlobalFn fn, const int64_t* args, size_t count);

// NUMERIC dispatch: int_form when all_integer, float_form otherwise.
GlobalFn resolve_numeric_form(const GlobalFunction& info, bool all_integer);

// CAST dispatch: inline form when `hint` is INT/FLOAT/BOOL, host otherwise.
GlobalFn resolve_cast_form(const GlobalFunction& info, int hint);

} // namespace gdscript
