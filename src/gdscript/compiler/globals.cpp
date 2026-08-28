#include "globals.h"
#include "../../syscalls.h"
#include "compiler_exception.h"
#include <cmath>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace gdscript {

// From Godot's math_funcs.h.
static constexpr double MATH_PI = 3.1415926535897932384626433833;
static constexpr double MATH_TAU = 6.2831853071795864769252867666;
static constexpr double CMP_EPSILON = 0.00001;

// One row per global. Unused columns use the NO_OP / NO_FORM placeholders.

#define NO_OP (-1)
#define NO_FORM GlobalFn::PRINT

static const GlobalFunction GLOBAL_FUNCTIONS[] = {
	// name, fn, kind, min, max, result, utility_op, float_args, int_form, float_form

	// Side-effecting. Distinguished only by Print_Channel in utility_op.
	{ "print", GlobalFn::PRINT, GlobalKind::PRINT, 0, 63, GlobalResult::NIL, int16_t(Print_Channel::PRINT), 0, NO_FORM, NO_FORM },
	{ "prints", GlobalFn::PRINTS, GlobalKind::PRINT, 0, 63, GlobalResult::NIL, int16_t(Print_Channel::SPACED), 0, NO_FORM, NO_FORM },
	{ "printt", GlobalFn::PRINTT, GlobalKind::PRINT, 0, 63, GlobalResult::NIL, int16_t(Print_Channel::TABBED), 0, NO_FORM, NO_FORM },
	{ "printraw", GlobalFn::PRINTRAW, GlobalKind::PRINT, 0, 63, GlobalResult::NIL, int16_t(Print_Channel::RAW), 0, NO_FORM, NO_FORM },
	{ "print_rich", GlobalFn::PRINT_RICH, GlobalKind::PRINT, 0, 63, GlobalResult::NIL, int16_t(Print_Channel::RICH), 0, NO_FORM, NO_FORM },
	{ "printerr", GlobalFn::PRINTERR, GlobalKind::PRINT, 0, 63, GlobalResult::NIL, int16_t(Print_Channel::ERROR), 0, NO_FORM, NO_FORM },
	{ "print_verbose", GlobalFn::PRINT_VERBOSE, GlobalKind::PRINT, 0, 63, GlobalResult::NIL, int16_t(Print_Channel::VERBOSE), 0, NO_FORM, NO_FORM },
	{ "push_error", GlobalFn::PUSH_ERROR, GlobalKind::PRINT, 1, 63, GlobalResult::NIL, int16_t(Print_Channel::PUSH_ERROR), 0, NO_FORM, NO_FORM },
	{ "push_warning", GlobalFn::PUSH_WARNING, GlobalKind::PRINT, 1, 63, GlobalResult::NIL, int16_t(Print_Channel::PUSH_WARNING), 0, NO_FORM, NO_FORM },

	// Sign and magnitude
	{ "abs", GlobalFn::ABS, GlobalKind::NUMERIC, 1, 1, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::ABSI, GlobalFn::ABSF },
	{ "absi", GlobalFn::ABSI, GlobalKind::INT_OP, 1, 1, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "absf", GlobalFn::ABSF, GlobalKind::FLOAT_OP, 1, 1, GlobalResult::FLOAT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "sign", GlobalFn::SIGN, GlobalKind::NUMERIC, 1, 1, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::SIGNI, GlobalFn::SIGNF },
	{ "signi", GlobalFn::SIGNI, GlobalKind::INT_OP, 1, 1, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "signf", GlobalFn::SIGNF, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_SIGN, 1, NO_FORM, NO_FORM },

	// Rounding
	{ "floor", GlobalFn::FLOOR, GlobalKind::NUMERIC, 1, 1, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::INT_IDENTITY, GlobalFn::FLOORF },
	{ "floorf", GlobalFn::FLOORF, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_FLOOR, 1, NO_FORM, NO_FORM },
	{ "floori", GlobalFn::FLOORI, GlobalKind::SYSCALL, 1, 1, GlobalResult::INT, UTILITY_FLOOR, 1, NO_FORM, NO_FORM },
	{ "ceil", GlobalFn::CEIL, GlobalKind::NUMERIC, 1, 1, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::INT_IDENTITY, GlobalFn::CEILF },
	{ "ceilf", GlobalFn::CEILF, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_CEIL, 1, NO_FORM, NO_FORM },
	{ "ceili", GlobalFn::CEILI, GlobalKind::SYSCALL, 1, 1, GlobalResult::INT, UTILITY_CEIL, 1, NO_FORM, NO_FORM },
	{ "round", GlobalFn::ROUND, GlobalKind::NUMERIC, 1, 1, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::INT_IDENTITY, GlobalFn::ROUNDF },
	{ "roundf", GlobalFn::ROUNDF, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_ROUND, 1, NO_FORM, NO_FORM },
	{ "roundi", GlobalFn::ROUNDI, GlobalKind::SYSCALL, 1, 1, GlobalResult::INT, UTILITY_ROUND, 1, NO_FORM, NO_FORM },
	{ "snapped", GlobalFn::SNAPPED, GlobalKind::NUMERIC, 2, 2, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::SNAPPEDI, GlobalFn::SNAPPEDF },
	{ "snappedf", GlobalFn::SNAPPEDF, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_SNAPPED, 2, NO_FORM, NO_FORM },
	{ "snappedi", GlobalFn::SNAPPEDI, GlobalKind::SYSCALL, 2, 2, GlobalResult::INT, UTILITY_SNAPPED, 2, NO_FORM, NO_FORM },

	// Selection. Variadic min/max folded to pairwise by codegen.
	{ "min", GlobalFn::MIN, GlobalKind::NUMERIC, 2, 63, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::MINI, GlobalFn::MINF },
	{ "mini", GlobalFn::MINI, GlobalKind::INT_OP, 2, 2, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "minf", GlobalFn::MINF, GlobalKind::FLOAT_OP, 2, 2, GlobalResult::FLOAT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "max", GlobalFn::MAX, GlobalKind::NUMERIC, 2, 63, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::MAXI, GlobalFn::MAXF },
	{ "maxi", GlobalFn::MAXI, GlobalKind::INT_OP, 2, 2, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "maxf", GlobalFn::MAXF, GlobalKind::FLOAT_OP, 2, 2, GlobalResult::FLOAT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "clamp", GlobalFn::CLAMP, GlobalKind::NUMERIC, 3, 3, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::CLAMPI, GlobalFn::CLAMPF },
	{ "clampi", GlobalFn::CLAMPI, GlobalKind::INT_OP, 3, 3, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "clampf", GlobalFn::CLAMPF, GlobalKind::FLOAT_OP, 3, 3, GlobalResult::FLOAT, NO_OP, 0, NO_FORM, NO_FORM },

	// Modulo and wrapping
	{ "posmod", GlobalFn::POSMOD, GlobalKind::INT_OP, 2, 2, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "fmod", GlobalFn::FMOD, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_FMOD, 2, NO_FORM, NO_FORM },
	{ "fposmod", GlobalFn::FPOSMOD, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_FPOSMOD, 2, NO_FORM, NO_FORM },
	{ "wrap", GlobalFn::WRAP, GlobalKind::NUMERIC, 3, 3, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::WRAPI, GlobalFn::WRAPF },
	{ "wrapi", GlobalFn::WRAPI, GlobalKind::INT_OP, 3, 3, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "wrapf", GlobalFn::WRAPF, GlobalKind::SYSCALL, 3, 3, GlobalResult::FLOAT, UTILITY_WRAP, 3, NO_FORM, NO_FORM },

	// Powers, roots, logarithms
	{ "sqrt", GlobalFn::SQRT, GlobalKind::FLOAT_OP, 1, 1, GlobalResult::FLOAT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "pow", GlobalFn::POW, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_POW, 2, NO_FORM, NO_FORM },
	{ "exp", GlobalFn::EXP, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_EXP, 1, NO_FORM, NO_FORM },
	{ "log", GlobalFn::LOG, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_LOG, 1, NO_FORM, NO_FORM },

	// Trigonometry
	{ "sin", GlobalFn::SIN, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_SIN, 1, NO_FORM, NO_FORM },
	{ "cos", GlobalFn::COS, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_COS, 1, NO_FORM, NO_FORM },
	{ "tan", GlobalFn::TAN, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_TAN, 1, NO_FORM, NO_FORM },
	{ "asin", GlobalFn::ASIN, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_ASIN, 1, NO_FORM, NO_FORM },
	{ "acos", GlobalFn::ACOS, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_ACOS, 1, NO_FORM, NO_FORM },
	{ "atan", GlobalFn::ATAN, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_ATAN, 1, NO_FORM, NO_FORM },
	{ "atan2", GlobalFn::ATAN2, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_ATAN2, 2, NO_FORM, NO_FORM },
	{ "sinh", GlobalFn::SINH, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_SINH, 1, NO_FORM, NO_FORM },
	{ "cosh", GlobalFn::COSH, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_COSH, 1, NO_FORM, NO_FORM },
	{ "tanh", GlobalFn::TANH, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_TANH, 1, NO_FORM, NO_FORM },
	{ "asinh", GlobalFn::ASINH, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_ASINH, 1, NO_FORM, NO_FORM },
	{ "acosh", GlobalFn::ACOSH, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_ACOSH, 1, NO_FORM, NO_FORM },
	{ "atanh", GlobalFn::ATANH, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_ATANH, 1, NO_FORM, NO_FORM },
	{ "deg_to_rad", GlobalFn::DEG_TO_RAD, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_DEG_TO_RAD, 1, NO_FORM, NO_FORM },
	{ "rad_to_deg", GlobalFn::RAD_TO_DEG, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_RAD_TO_DEG, 1, NO_FORM, NO_FORM },
	{ "angle_difference", GlobalFn::ANGLE_DIFFERENCE, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_ANGLE_DIFFERENCE, 2, NO_FORM, NO_FORM },

	// Decibels
	{ "linear_to_db", GlobalFn::LINEAR_TO_DB, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_LINEAR_TO_DB, 1, NO_FORM, NO_FORM },
	{ "db_to_linear", GlobalFn::DB_TO_LINEAR, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_DB_TO_LINEAR, 1, NO_FORM, NO_FORM },

	// Interpolation
	{ "lerp", GlobalFn::LERP, GlobalKind::SYSCALL, 3, 3, GlobalResult::FLOAT, UTILITY_LERP, 3, NO_FORM, NO_FORM },
	{ "lerpf", GlobalFn::LERPF, GlobalKind::SYSCALL, 3, 3, GlobalResult::FLOAT, UTILITY_LERP, 3, NO_FORM, NO_FORM },
	{ "lerp_angle", GlobalFn::LERP_ANGLE, GlobalKind::SYSCALL, 3, 3, GlobalResult::FLOAT, UTILITY_LERP_ANGLE, 3, NO_FORM, NO_FORM },
	{ "inverse_lerp", GlobalFn::INVERSE_LERP, GlobalKind::SYSCALL, 3, 3, GlobalResult::FLOAT, UTILITY_INVERSE_LERP, 3, NO_FORM, NO_FORM },
	{ "remap", GlobalFn::REMAP, GlobalKind::SYSCALL, 5, 5, GlobalResult::FLOAT, UTILITY_REMAP, 5, NO_FORM, NO_FORM },
	{ "smoothstep", GlobalFn::SMOOTHSTEP, GlobalKind::SYSCALL, 3, 3, GlobalResult::FLOAT, UTILITY_SMOOTHSTEP, 3, NO_FORM, NO_FORM },
	{ "move_toward", GlobalFn::MOVE_TOWARD, GlobalKind::SYSCALL, 3, 3, GlobalResult::FLOAT, UTILITY_MOVE_TOWARD, 3, NO_FORM, NO_FORM },
	{ "rotate_toward", GlobalFn::ROTATE_TOWARD, GlobalKind::SYSCALL, 3, 3, GlobalResult::FLOAT, UTILITY_ROTATE_TOWARD, 3, NO_FORM, NO_FORM },
	{ "pingpong", GlobalFn::PINGPONG, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_PINGPONG, 2, NO_FORM, NO_FORM },
	{ "cubic_interpolate", GlobalFn::CUBIC_INTERPOLATE, GlobalKind::SYSCALL, 5, 5, GlobalResult::FLOAT, UTILITY_CUBIC_INTERPOLATE, 5, NO_FORM, NO_FORM },
	{ "cubic_interpolate_angle", GlobalFn::CUBIC_INTERPOLATE_ANGLE, GlobalKind::SYSCALL, 5, 5, GlobalResult::FLOAT, UTILITY_CUBIC_INTERPOLATE_ANGLE, 5, NO_FORM, NO_FORM },
	// Eight arguments: fa0-fa7, the widest ECALL_UTILITY form.
	{ "cubic_interpolate_in_time", GlobalFn::CUBIC_INTERPOLATE_IN_TIME, GlobalKind::SYSCALL, 8, 8, GlobalResult::FLOAT, UTILITY_CUBIC_INTERPOLATE_IN_TIME, 8, NO_FORM, NO_FORM },
	{ "cubic_interpolate_angle_in_time", GlobalFn::CUBIC_INTERPOLATE_ANGLE_IN_TIME, GlobalKind::SYSCALL, 8, 8, GlobalResult::FLOAT, UTILITY_CUBIC_INTERPOLATE_ANGLE_IN_TIME, 8, NO_FORM, NO_FORM },
	{ "bezier_interpolate", GlobalFn::BEZIER_INTERPOLATE, GlobalKind::SYSCALL, 5, 5, GlobalResult::FLOAT, UTILITY_BEZIER_INTERPOLATE, 5, NO_FORM, NO_FORM },
	{ "bezier_derivative", GlobalFn::BEZIER_DERIVATIVE, GlobalKind::SYSCALL, 5, 5, GlobalResult::FLOAT, UTILITY_BEZIER_DERIVATIVE, 5, NO_FORM, NO_FORM },

	// Predicates
	{ "is_nan", GlobalFn::IS_NAN, GlobalKind::SYSCALL, 1, 1, GlobalResult::BOOL, UTILITY_IS_NAN, 1, NO_FORM, NO_FORM },
	{ "is_inf", GlobalFn::IS_INF, GlobalKind::SYSCALL, 1, 1, GlobalResult::BOOL, UTILITY_IS_INF, 1, NO_FORM, NO_FORM },
	{ "is_finite", GlobalFn::IS_FINITE, GlobalKind::SYSCALL, 1, 1, GlobalResult::BOOL, UTILITY_IS_FINITE, 1, NO_FORM, NO_FORM },
	{ "is_zero_approx", GlobalFn::IS_ZERO_APPROX, GlobalKind::SYSCALL, 1, 1, GlobalResult::BOOL, UTILITY_IS_ZERO_APPROX, 1, NO_FORM, NO_FORM },
	{ "is_equal_approx", GlobalFn::IS_EQUAL_APPROX, GlobalKind::SYSCALL, 2, 2, GlobalResult::BOOL, UTILITY_IS_EQUAL_APPROX, 2, NO_FORM, NO_FORM },

	// Curves and step sizes
	{ "ease", GlobalFn::EASE, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_EASE, 2, NO_FORM, NO_FORM },
	{ "step_decimals", GlobalFn::STEP_DECIMALS, GlobalKind::SYSCALL, 1, 1, GlobalResult::INT, UTILITY_STEP_DECIMALS, 1, NO_FORM, NO_FORM },
	// int64 result via a0; exceeds double range.
	{ "nearest_po2", GlobalFn::NEAREST_PO2, GlobalKind::SYSCALL_INT, 1, 1, GlobalResult::INT, UTILITY_NEAREST_PO2, 0, NO_FORM, NO_FORM },

	// Variant queries
	{ "str", GlobalFn::STR, GlobalKind::HOST, 1, 63, GlobalResult::STRING, UTILITY_STR, 0, NO_FORM, NO_FORM },
	{ "len", GlobalFn::LEN, GlobalKind::HOST, 1, 1, GlobalResult::INT, UTILITY_LEN, 0, NO_FORM, NO_FORM },

	// Serialization and identity. Depend on host Variant encoding.
	{ "hash", GlobalFn::HASH, GlobalKind::HOST, 1, 1, GlobalResult::INT, UTILITY_HASH, 0, NO_FORM, NO_FORM },
	{ "var_to_str", GlobalFn::VAR_TO_STR, GlobalKind::HOST, 1, 1, GlobalResult::STRING, UTILITY_VAR_TO_STR, 0, NO_FORM, NO_FORM },
	{ "str_to_var", GlobalFn::STR_TO_VAR, GlobalKind::HOST, 1, 1, GlobalResult::VARIANT, UTILITY_STR_TO_VAR, 0, NO_FORM, NO_FORM },
	{ "var_to_bytes", GlobalFn::VAR_TO_BYTES, GlobalKind::HOST, 1, 1, GlobalResult::VARIANT, UTILITY_VAR_TO_BYTES, 0, NO_FORM, NO_FORM },
	{ "bytes_to_var", GlobalFn::BYTES_TO_VAR, GlobalKind::HOST, 1, 1, GlobalResult::VARIANT, UTILITY_BYTES_TO_VAR, 0, NO_FORM, NO_FORM },
	{ "type_string", GlobalFn::TYPE_STRING, GlobalKind::HOST, 1, 1, GlobalResult::STRING, UTILITY_TYPE_STRING, 0, NO_FORM, NO_FORM },
	{ "type_convert", GlobalFn::TYPE_CONVERT, GlobalKind::HOST, 2, 2, GlobalResult::VARIANT, UTILITY_TYPE_CONVERT, 0, NO_FORM, NO_FORM },
	{ "error_string", GlobalFn::ERROR_STRING, GlobalKind::HOST, 1, 1, GlobalResult::STRING, UTILITY_ERROR_STRING, 0, NO_FORM, NO_FORM },
	{ "is_same", GlobalFn::IS_SAME, GlobalKind::HOST, 2, 2, GlobalResult::BOOL, UTILITY_IS_SAME, 0, NO_FORM, NO_FORM },
	{ "is_instance_valid", GlobalFn::IS_INSTANCE_VALID, GlobalKind::HOST, 1, 1, GlobalResult::BOOL, UTILITY_IS_INSTANCE_VALID, 0, NO_FORM, NO_FORM },

	{ "char", GlobalFn::CHAR, GlobalKind::HOST, 1, 1, GlobalResult::STRING, UTILITY_CHAR, 0, NO_FORM, NO_FORM },
	{ "ord", GlobalFn::ORD, GlobalKind::HOST, 1, 1, GlobalResult::INT, UTILITY_ORD, 0, NO_FORM, NO_FORM },

	// Seeded draw. Not in the random family below: it reads no shared state
	// and writes none -- the seed goes in and the next one comes back in the
	// PackedInt64Array beside the number -- so it may be folded and hoisted
	// like any other pure call.
	{ "rand_from_seed", GlobalFn::RAND_FROM_SEED, GlobalKind::HOST, 1, 1, GlobalResult::VARIANT, UTILITY_RAND_FROM_SEED, 0, NO_FORM, NO_FORM },

	// Type constructors. Inline when argument is numeric/bool; host otherwise.
	{ "int", GlobalFn::TO_INT, GlobalKind::CAST, 1, 1, GlobalResult::INT, UTILITY_TO_INT, 0, GlobalFn::INT_IDENTITY, NO_FORM },
	{ "float", GlobalFn::TO_FLOAT, GlobalKind::CAST, 1, 1, GlobalResult::FLOAT, UTILITY_TO_FLOAT, 0, GlobalFn::FLOAT_IDENTITY, NO_FORM },
	{ "bool", GlobalFn::TO_BOOL, GlobalKind::CAST, 1, 1, GlobalResult::BOOL, UTILITY_TO_BOOL, 0, GlobalFn::BOOLEANIZE, NO_FORM },
	{ "String", GlobalFn::TO_STRING, GlobalKind::HOST, 0, 1, GlobalResult::STRING, UTILITY_STR, 0, NO_FORM, NO_FORM },

	// Randomness (impure). The mutation calls are available only when compiling
	// for an unrestricted Sandbox.
	{ "randf", GlobalFn::RANDF, GlobalKind::SYSCALL, 0, 0, GlobalResult::FLOAT, UTILITY_RANDF, 0, NO_FORM, NO_FORM, true },
	{ "randf_range", GlobalFn::RANDF_RANGE, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_RANDF_RANGE, 2, NO_FORM, NO_FORM, true },
	{ "randfn", GlobalFn::RANDFN, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_RANDFN, 2, NO_FORM, NO_FORM, true },
	{ "randi", GlobalFn::RANDI, GlobalKind::SYSCALL_INT, 0, 0, GlobalResult::INT, UTILITY_RANDI, 0, NO_FORM, NO_FORM, true },
	{ "randi_range", GlobalFn::RANDI_RANGE, GlobalKind::SYSCALL_INT, 2, 2, GlobalResult::INT, UTILITY_RANDI_RANGE, 0, NO_FORM, NO_FORM, true },
	{ "randomize", GlobalFn::RANDOMIZE, GlobalKind::HOST, 0, 0, GlobalResult::NIL, UTILITY_RANDOMIZE, 0, NO_FORM, NO_FORM, true, true },
	{ "seed", GlobalFn::SEED, GlobalKind::HOST, 1, 1, GlobalResult::NIL, UTILITY_SEED, 0, NO_FORM, NO_FORM, true, true },

	// Internal forms (not callable by name, hence invalid-identifier names).
	{ ".int_identity", GlobalFn::INT_IDENTITY, GlobalKind::INT_OP, 1, 1, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ ".float_identity", GlobalFn::FLOAT_IDENTITY, GlobalKind::FLOAT_OP, 1, 1, GlobalResult::FLOAT, NO_OP, 0, NO_FORM, NO_FORM },
	{ ".booleanize", GlobalFn::BOOLEANIZE, GlobalKind::FLOAT_OP, 1, 1, GlobalResult::BOOL, NO_OP, 0, NO_FORM, NO_FORM },
};

#undef NO_OP
#undef NO_FORM

const GlobalFunction* find_global_function(const std::string& name) {
	static const std::unordered_map<std::string, const GlobalFunction*> by_name = [] {
		std::unordered_map<std::string, const GlobalFunction*> map;
		for (const GlobalFunction& entry : GLOBAL_FUNCTIONS) {
			map[entry.name] = &entry;
		}
		return map;
	}();

	auto it = by_name.find(name);
	return it == by_name.end() ? nullptr : it->second;
}

size_t global_function_count() {
	return std::size(GLOBAL_FUNCTIONS);
}

const char* global_function_name(size_t index) {
	const char* name = GLOBAL_FUNCTIONS[index].name;
	// Leading '.' marks internal lowering forms; not source-visible.
	return name[0] == '.' ? nullptr : name;
}

static const GlobalConstant GLOBAL_CONSTANTS[] = {
#define GDSC_INT_CONSTANT(name, value) { #name, false, static_cast<int64_t>(value), 0.0 },
#define GDSC_FLOAT_CONSTANT(name, value) { #name, true, 0, (value) },
#include "global_constants.def"
};

const GlobalConstant* find_global_constant(const std::string& name) {
	static const std::unordered_map<std::string, const GlobalConstant*> by_name = [] {
		std::unordered_map<std::string, const GlobalConstant*> map;
		for (const GlobalConstant& entry : GLOBAL_CONSTANTS) {
			map[entry.name] = &entry;
		}
		return map;
	}();

	auto it = by_name.find(name);
	return it == by_name.end() ? nullptr : it->second;
}

size_t global_constant_count() {
	return std::size(GLOBAL_CONSTANTS);
}

const char* global_constant_name(size_t index) {
	return GLOBAL_CONSTANTS[index].name;
}

static const BuiltinConstant BUILTIN_CONSTANTS[] = {
#define GDSC_BUILTIN_CONSTANT(type, name, c0, c1, c2, c3) { #type, #name, { c0, c1, c2, c3 } },
#include "builtin_constants.def"
};

static const GlobalEnumValue GLOBAL_ENUM_VALUES[] = {
#define GDSC_GLOBAL_ENUM_VALUE(enum_name, name, value) { enum_name, #name, (value) },
#include "global_enums.def"
};

const GlobalEnumValue* find_global_enum_value(const std::string& enum_name, const std::string& name) {
	// Keyed on "Enum.MEMBER": one map answers both the member lookup and, via
	// the name set below, the question of whether the enum exists at all.
	static const std::unordered_map<std::string, const GlobalEnumValue*> by_name = [] {
		std::unordered_map<std::string, const GlobalEnumValue*> map;
		for (const GlobalEnumValue& entry : GLOBAL_ENUM_VALUES) {
			map[std::string(entry.enum_name) + "." + entry.name] = &entry;
		}
		return map;
	}();

	auto it = by_name.find(enum_name + "." + name);
	return it == by_name.end() ? nullptr : it->second;
}

bool is_global_enum(const std::string& name) {
	static const std::unordered_set<std::string> names = [] {
		std::unordered_set<std::string> set;
		for (const GlobalEnumValue& entry : GLOBAL_ENUM_VALUES) {
			set.insert(entry.enum_name);
		}
		return set;
	}();

	return names.find(name) != names.end();
}

size_t global_enum_value_count() {
	return std::size(GLOBAL_ENUM_VALUES);
}

const char* global_enum_value_enum(size_t index) {
	return GLOBAL_ENUM_VALUES[index].enum_name;
}

const char* global_enum_value_name(size_t index) {
	return GLOBAL_ENUM_VALUES[index].name;
}

int64_t global_enum_value(size_t index) {
	return GLOBAL_ENUM_VALUES[index].value;
}

const BuiltinConstant* find_builtin_constant(const std::string& type, const std::string& name) {
	for (const BuiltinConstant& entry : BUILTIN_CONSTANTS) {
		if (type == entry.type && name == entry.name) {
			return &entry;
		}
	}
	return nullptr;
}

bool has_builtin_constants(const std::string& type) {
	for (const BuiltinConstant& entry : BUILTIN_CONSTANTS) {
		if (type == entry.type) {
			return true;
		}
	}
	return false;
}

// @GlobalScope names not yet lowered. Refused at compile time; the self-call
// fallback would be silently dropped. Removed when a GLOBAL_FUNCTIONS row is added.
static const struct { const char* name; const char* reason; } UNIMPLEMENTED_GLOBALS[] = {
	// No guest script location to attach.
	{ "print_debug", "there is no script location to attach; use print()" },

	// Object references are process-local.
	{ "var_to_bytes_with_objects", "object references cannot leave the sandbox" },
	{ "bytes_to_var_with_objects", "object references cannot enter the sandbox" },

	// Object lifetime: sandbox uses allowlist, not instance ids.
	{ "instance_from_id", "objects are reached through the sandbox allowlist, not by instance id" },
	{ "is_instance_id_valid", "objects are reached through the sandbox allowlist, not by instance id" },
	{ "weakref", "no host syscall for weak references yet" },

	{ "rid_allocate_id", "RIDs name engine resources the sandbox cannot reach" },
	{ "rid_from_int64", "RIDs name engine resources the sandbox cannot reach" },

};

size_t builtin_constant_count() {
	return std::size(BUILTIN_CONSTANTS);
}

const char* builtin_constant_type(size_t index) {
	return BUILTIN_CONSTANTS[index].type;
}

const char* builtin_constant_name(size_t index) {
	return BUILTIN_CONSTANTS[index].name;
}

const char* unimplemented_global_reason(const std::string& name) {
	static const std::unordered_map<std::string, const char*> by_name = [] {
		std::unordered_map<std::string, const char*> map;
		for (const auto& entry : UNIMPLEMENTED_GLOBALS) {
			map[entry.name] = entry.reason;
		}
		return map;
	}();

	auto it = by_name.find(name);
	return it == by_name.end() ? nullptr : it->second;
}

const GlobalFunction& global_function(GlobalFn fn) {
	static const std::unordered_map<int16_t, const GlobalFunction*> by_fn = [] {
		std::unordered_map<int16_t, const GlobalFunction*> map;
		for (const GlobalFunction& entry : GLOBAL_FUNCTIONS) {
			map[static_cast<int16_t>(entry.fn)] = &entry;
		}
		return map;
	}();

	auto it = by_fn.find(static_cast<int16_t>(fn));
	if (it == by_fn.end()) {
		throw CompilerException(ErrorType::CODEGEN_ERROR,
			"No table entry for global function id " + std::to_string(static_cast<int>(fn)));
	}
	return *it->second;
}

GlobalFn resolve_numeric_form(const GlobalFunction& info, bool all_integer) {
	if (info.kind != GlobalKind::NUMERIC) {
		return info.fn;
	}
	return all_integer ? info.int_form : info.float_form;
}

GlobalFn resolve_cast_form(const GlobalFunction& info, int hint) {
	if (info.kind != GlobalKind::CAST) {
		return info.fn;
	}
	// Numeric/bool: inline (the load is the conversion). Anything else
	// (e.g. int("42") == 42) needs the host.
	switch (hint) {
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::BOOL:
			return info.int_form;
		default:
			return info.fn;
	}
}

// Godot's math_funcs.h transcribed in double (not real_t) for build-invariance.

static double eval_sign(double x) {
	// SIGN(): zero and NaN are neither positive nor negative.
	return (x < 0.0) ? -1.0 : ((x > 0.0) ? 1.0 : 0.0);
}

static bool eval_is_equal_approx(double a, double b) {
	// Exact equality first: infinities must compare equal.
	if (a == b) {
		return true;
	}
	double tolerance = CMP_EPSILON * std::fabs(a);
	if (tolerance < CMP_EPSILON) {
		tolerance = CMP_EPSILON;
	}
	return std::fabs(a - b) < tolerance;
}

static bool eval_is_zero_approx(double x) {
	return std::fabs(x) < CMP_EPSILON;
}

static double eval_fposmod(double x, double y) {
	double value = std::fmod(x, y);
	if ((value < 0 && y > 0) || (value > 0 && y < 0)) {
		value += y;
	}
	value += 0.0;
	return value;
}

static double eval_lerp(double from, double to, double weight) {
	return from + weight * (to - from);
}

static double eval_inverse_lerp(double from, double to, double value) {
	return (value - from) / (to - from);
}

static double eval_angle_difference(double from, double to) {
	const double difference = std::fmod(to - from, MATH_TAU);
	return std::fmod(2.0 * difference, MATH_TAU) - difference;
}

// Math::cubic_interpolate() and the three forms built on it. Transcribed
// rather than derived: the host mirrors these in utility_math_op(), and the
// differential test compares the two.
static double eval_cubic_interpolate(double from, double to, double pre, double post, double weight) {
	return 0.5 *
		((from * 2.0) +
			(-pre + to) * weight +
			(2.0 * pre - 5.0 * from + 4.0 * to - post) * (weight * weight) +
			(-pre + 3.0 * from - 3.0 * to + post) * (weight * weight * weight));
}

// Barry-Goldman: the four values sit at four times rather than at even spacing.
static double eval_cubic_interpolate_in_time(double from, double to, double pre, double post,
	double weight, double to_t, double pre_t, double post_t)
{
	const double t = eval_lerp(0.0, to_t, weight);
	const double a1 = eval_lerp(pre, from, (pre_t == 0.0) ? 0.0 : (t - pre_t) / -pre_t);
	const double a2 = eval_lerp(from, to, (to_t == 0.0) ? 0.5 : t / to_t);
	const double a3 = eval_lerp(to, post, (post_t - to_t == 0.0) ? 1.0 : (t - to_t) / (post_t - to_t));
	const double b1 = eval_lerp(a1, a2, (to_t - pre_t == 0.0) ? 0.0 : (t - pre_t) / (to_t - pre_t));
	const double b2 = eval_lerp(a2, a3, (post_t == 0.0) ? 1.0 : t / post_t);
	return eval_lerp(b1, b2, (to_t == 0.0) ? 0.5 : t / to_t);
}

// The angular forms first bring all four control values onto one branch of the
// circle, then interpolate them as ordinary numbers.
struct CubicAngles {
	double from, to, pre, post;
};

static CubicAngles eval_cubic_angles(double from, double to, double pre, double post) {
	const double from_rot = std::fmod(from, MATH_TAU);
	const double pre_diff = std::fmod(pre - from_rot, MATH_TAU);
	const double pre_rot = from_rot + std::fmod(2.0 * pre_diff, MATH_TAU) - pre_diff;
	const double to_diff = std::fmod(to - from_rot, MATH_TAU);
	const double to_rot = from_rot + std::fmod(2.0 * to_diff, MATH_TAU) - to_diff;
	const double post_diff = std::fmod(post - to_rot, MATH_TAU);
	const double post_rot = to_rot + std::fmod(2.0 * post_diff, MATH_TAU) - post_diff;
	return { from_rot, to_rot, pre_rot, post_rot };
}

static double eval_snapped(double value, double step) {
	if (step != 0) {
		value = std::floor(value / step + 0.5) * step;
	}
	return value;
}

static double eval_wrapf(double value, double min, double max) {
	const double range = max - min;
	if (eval_is_zero_approx(range)) {
		return min;
	}
	const double result = value - (range * std::floor((value - min) / range));
	if (eval_is_equal_approx(result, max)) {
		return min;
	}
	return result;
}

double eval_utility_op(int16_t utility_op, const double args[UTILITY_MAX_FLOAT_ARGS]) {
	const double a = args[0];
	const double b = args[1];
	const double c = args[2];
	const double d = args[3];
	const double e = args[4];
	const double f = args[5];
	const double g = args[6];
	const double h = args[7];

	switch (utility_op) {
		case UTILITY_FLOOR: return std::floor(a);
		case UTILITY_CEIL: return std::ceil(a);
		// Godot rounds half away from zero via floor(), not ::round().
		case UTILITY_ROUND: return (a >= 0) ? std::floor(a + 0.5) : -std::floor(-a + 0.5);
		case UTILITY_SIGN: return eval_sign(a);
		case UTILITY_SIN: return std::sin(a);
		case UTILITY_COS: return std::cos(a);
		case UTILITY_TAN: return std::tan(a);
		case UTILITY_ASIN: return std::asin(a);
		case UTILITY_ACOS: return std::acos(a);
		case UTILITY_ATAN: return std::atan(a);
		case UTILITY_SINH: return std::sinh(a);
		case UTILITY_COSH: return std::cosh(a);
		case UTILITY_TANH: return std::tanh(a);
		case UTILITY_ASINH: return std::asinh(a);
		case UTILITY_ACOSH: return std::acosh(a);
		case UTILITY_ATANH: return std::atanh(a);
		case UTILITY_EXP: return std::exp(a);
		case UTILITY_LOG: return std::log(a);
		case UTILITY_DEG_TO_RAD: return a * MATH_PI / 180.0;
		case UTILITY_RAD_TO_DEG: return a * 180.0 / MATH_PI;
		case UTILITY_LINEAR_TO_DB: return std::log(a) * 8.6858896380650365530225783783321;
		case UTILITY_DB_TO_LINEAR: return std::exp(a * 0.11512925464970228420089957273422);
		case UTILITY_IS_NAN: return std::isnan(a) ? 1.0 : 0.0;
		case UTILITY_IS_INF: return std::isinf(a) ? 1.0 : 0.0;
		case UTILITY_IS_FINITE: return std::isfinite(a) ? 1.0 : 0.0;
		case UTILITY_IS_ZERO_APPROX: return eval_is_zero_approx(a) ? 1.0 : 0.0;

		// Transcribed from Math::ease() / Math::step_decimals(); must match host utility_math_op().
		case UTILITY_EASE: {
			const double x = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
			if (b > 0.0) {
				return (b < 1.0) ? 1.0 - std::pow(1.0 - x, 1.0 / b) : std::pow(x, b);
			}
			if (b < 0.0) {
				return (x < 0.5)
						? std::pow(x * 2.0, -b) * 0.5
						: (1.0 - std::pow(1.0 - (x - 0.5) * 2.0, -b)) * 0.5 + 0.5;
			}
			return 0.0;
		}
		case UTILITY_STEP_DECIMALS: {
			static const double sd[] = { 0.9999, 0.09999, 0.009999, 0.0009999, 0.00009999,
				0.000009999, 0.0000009999, 0.00000009999, 0.000000009999, 0.0000000009999 };
			const double magnitude = std::fabs(a);
			const double decimals = magnitude - std::floor(magnitude);
			for (int i = 0; i < 10; i++) {
				if (decimals >= sd[i]) {
					return double(i);
				}
			}
			return 0.0;
		}

		case UTILITY_ATAN2: return std::atan2(a, b);
		case UTILITY_POW: return std::pow(a, b);
		case UTILITY_FMOD: return std::fmod(a, b);
		case UTILITY_FPOSMOD: return eval_fposmod(a, b);
		case UTILITY_SNAPPED: return eval_snapped(a, b);
		case UTILITY_IS_EQUAL_APPROX: return eval_is_equal_approx(a, b) ? 1.0 : 0.0;
		case UTILITY_ANGLE_DIFFERENCE: return eval_angle_difference(a, b);
		case UTILITY_PINGPONG: {
			if (b == 0.0) {
				return 0.0;
			}
			const double x = (a - b) / (b * 2.0);
			const double fract = x - std::floor(x);
			return std::fabs(fract * b * 2.0 - b);
		}

		case UTILITY_LERP: return eval_lerp(a, b, c);
		case UTILITY_INVERSE_LERP: return eval_inverse_lerp(a, b, c);
		case UTILITY_SMOOTHSTEP: {
			if (eval_is_equal_approx(a, b)) {
				return a;
			}
			double x = eval_inverse_lerp(a, b, c);
			x = (x < 0.0) ? 0.0 : ((x > 1.0) ? 1.0 : x);
			return x * x * (3.0 - 2.0 * x);
		}
		case UTILITY_MOVE_TOWARD:
			return std::fabs(b - a) <= c ? b : a + eval_sign(b - a) * c;
		case UTILITY_LERP_ANGLE:
			return a + eval_angle_difference(a, b) * c;
		case UTILITY_ROTATE_TOWARD: {
			const double difference = eval_angle_difference(a, b);
			const double abs_difference = std::fabs(difference);
			double delta = c;
			const double lower = abs_difference - MATH_PI;
			delta = (delta < lower) ? lower : ((delta > abs_difference) ? abs_difference : delta);
			return a + delta * ((difference >= 0.0) ? 1.0 : -1.0);
		}
		case UTILITY_WRAP: return eval_wrapf(a, b, c);

		case UTILITY_REMAP:
			return eval_lerp(d, e, eval_inverse_lerp(b, c, a));
		case UTILITY_CUBIC_INTERPOLATE:
			return eval_cubic_interpolate(a, b, c, d, e);
		case UTILITY_CUBIC_INTERPOLATE_ANGLE: {
			const CubicAngles rot = eval_cubic_angles(a, b, c, d);
			return eval_cubic_interpolate(rot.from, rot.to, rot.pre, rot.post, e);
		}
		case UTILITY_CUBIC_INTERPOLATE_IN_TIME:
			return eval_cubic_interpolate_in_time(a, b, c, d, e, f, g, h);
		case UTILITY_CUBIC_INTERPOLATE_ANGLE_IN_TIME: {
			const CubicAngles rot = eval_cubic_angles(a, b, c, d);
			return eval_cubic_interpolate_in_time(rot.from, rot.to, rot.pre, rot.post, e, f, g, h);
		}
		case UTILITY_BEZIER_INTERPOLATE: {
			const double omt = 1.0 - e;
			const double omt2 = omt * omt;
			const double omt3 = omt2 * omt;
			const double t2 = e * e;
			const double t3 = t2 * e;
			return a * omt3 + b * omt2 * e * 3.0 + c * omt * t2 * 3.0 + d * t3;
		}
		case UTILITY_BEZIER_DERIVATIVE: {
			const double omt = 1.0 - e;
			const double omt2 = omt * omt;
			const double t2 = e * e;
			return (b - a) * 3.0 * omt2 + (c - b) * 6.0 * omt * e + (d - c) * 3.0 * t2;
		}

		case UTILITY_STR:
		case UTILITY_LEN:
		case UTILITY_TO_INT:
		case UTILITY_TO_FLOAT:
		case UTILITY_TO_BOOL:
		case UTILITY_RAND_FROM_SEED:
			throw CompilerException(ErrorType::CODEGEN_ERROR,
				"str(), len() and the type constructors need the host's Variant API"
				" and cannot be evaluated here");

		case UTILITY_RANDF:
		case UTILITY_RANDF_RANGE:
		case UTILITY_RANDFN:
		case UTILITY_RANDI:
		case UTILITY_RANDI_RANGE:
			throw CompilerException(ErrorType::CODEGEN_ERROR,
				"The random functions need the host's random number generator"
				" and cannot be evaluated here");
		default:
			throw CompilerException(ErrorType::CODEGEN_ERROR,
				"Unknown utility op " + std::to_string(utility_op));
	}
}

int64_t eval_global_int(GlobalFn fn, const int64_t* args, size_t count) {
	const auto need = [&](size_t n) {
		if (count < n) {
			throw CompilerException(ErrorType::CODEGEN_ERROR,
				std::string(global_function(fn).name) + " needs " + std::to_string(n) + " arguments");
		}
	};

	switch (fn) {
		case GlobalFn::INT_IDENTITY:
			need(1);
			return args[0];
		case GlobalFn::ABSI: {
			need(1);
			// Sign-mask form: wraps on INT64_MIN (matching RISC-V); std::abs(INT64_MIN) is UB.
			const int64_t mask = args[0] >> 63;
			return static_cast<int64_t>((static_cast<uint64_t>(args[0]) ^ static_cast<uint64_t>(mask)) - static_cast<uint64_t>(mask));
		}
		case GlobalFn::SIGNI:
			need(1);
			return (args[0] > 0) ? 1 : ((args[0] < 0) ? -1 : 0);
		case GlobalFn::MINI:
			need(2);
			return args[0] < args[1] ? args[0] : args[1];
		case GlobalFn::MAXI:
			need(2);
			return args[0] > args[1] ? args[0] : args[1];
		case GlobalFn::CLAMPI:
			need(3);
			if (args[0] < args[1]) return args[1];
			if (args[0] > args[2]) return args[2];
			return args[0];
		case GlobalFn::POSMOD: {
			need(2);
			// Zero divisor → 0 (C++ % traps; sandbox cannot trap usefully).
			if (args[1] == 0) {
				return 0;
			}
			int64_t value = args[0] % args[1];
			if ((value < 0 && args[1] > 0) || (value > 0 && args[1] < 0)) {
				value += args[1];
			}
			return value;
		}
		case GlobalFn::WRAPI: {
			need(3);
			const int64_t range = args[2] - args[1];
			if (range == 0) {
				return args[1];
			}
			return args[1] + (((args[0] - args[1]) % range + range) % range);
		}
		default:
			throw CompilerException(ErrorType::CODEGEN_ERROR,
				std::string(global_function(fn).name) + " is not an integer operation");
	}
}

int64_t eval_global_int_syscall(GlobalFn fn, const int64_t* args, size_t count) {
	(void)args;
	(void)count;
	// OPTIMIZER_ERROR, not CODEGEN_ERROR: the harnesses skip a program on this,
	// the way they do for the other globals only the host can answer.
	throw CompilerException(ErrorType::OPTIMIZER_ERROR,
		std::string(global_function(fn).name) + "() is answered by the host"
		" and cannot be evaluated here");
}

double eval_global_float(GlobalFn fn, const double* args, size_t count) {
	const GlobalFunction& info = global_function(fn);

	if (info.kind == GlobalKind::SYSCALL) {
		double packed[UTILITY_MAX_FLOAT_ARGS] = {};
		for (size_t i = 0; i < count && i < UTILITY_MAX_FLOAT_ARGS; i++) {
			packed[i] = args[i];
		}
		return eval_utility_op(info.utility_op, packed);
	}

	const auto need = [&](size_t n) {
		if (count < n) {
			throw CompilerException(ErrorType::CODEGEN_ERROR,
				std::string(info.name) + " needs " + std::to_string(n) + " arguments");
		}
	};

	switch (fn) {
		case GlobalFn::ABSF:
			need(1);
			return std::fabs(args[0]);
		case GlobalFn::SQRT:
			need(1);
			return std::sqrt(args[0]);
		case GlobalFn::MINF:
			need(2);
			return args[0] < args[1] ? args[0] : args[1];
		case GlobalFn::MAXF:
			need(2);
			return args[0] > args[1] ? args[0] : args[1];
		case GlobalFn::CLAMPF:
			need(3);
			if (args[0] < args[1]) return args[1];
			if (args[0] > args[2]) return args[2];
			return args[0];
		case GlobalFn::FLOAT_IDENTITY:
			need(1);
			return args[0];
		case GlobalFn::BOOLEANIZE:
			// Variant::booleanize(): NaN is true.
			need(1);
			return (args[0] != 0.0) ? 1.0 : 0.0;
		default:
			throw CompilerException(ErrorType::CODEGEN_ERROR,
				std::string(info.name) + " is not a floating-point operation");
	}
}

} // namespace gdscript
