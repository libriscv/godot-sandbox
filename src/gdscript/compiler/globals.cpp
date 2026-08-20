#include "globals.h"
#include "compiler_exception.h"
#include <cmath>
#include <unordered_map>

namespace gdscript {

// Godot's constants, spelled the way math_funcs.h spells them.
static constexpr double MATH_PI = 3.1415926535897932384626433833;
static constexpr double MATH_TAU = 6.2831853071795864769252867666;
static constexpr double CMP_EPSILON = 0.00001;

// -= The table =-
//
// One row per global. The columns after the result are only read by the kinds
// that need them: utility_op / float_args by SYSCALL, int_form / float_form by
// NUMERIC. A row that does not use a column leaves it at the placeholder.

#define NO_OP (-1)
#define NO_FORM GlobalFn::PRINT

static const GlobalFunction GLOBAL_FUNCTIONS[] = {
	// name, fn, kind, min, max, result, utility_op, float_args, int_form, float_form

	// -= The one global with a side effect =-
	{ "print", GlobalFn::PRINT, GlobalKind::PRINT, 0, 63, GlobalResult::NIL, NO_OP, 0, NO_FORM, NO_FORM },

	// -= Sign and magnitude =-
	{ "abs", GlobalFn::ABS, GlobalKind::NUMERIC, 1, 1, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::ABSI, GlobalFn::ABSF },
	{ "absi", GlobalFn::ABSI, GlobalKind::INT_OP, 1, 1, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "absf", GlobalFn::ABSF, GlobalKind::FLOAT_OP, 1, 1, GlobalResult::FLOAT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "sign", GlobalFn::SIGN, GlobalKind::NUMERIC, 1, 1, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::SIGNI, GlobalFn::SIGNF },
	{ "signi", GlobalFn::SIGNI, GlobalKind::INT_OP, 1, 1, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "signf", GlobalFn::SIGNF, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_SIGN, 1, NO_FORM, NO_FORM },

	// -= Rounding =-
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

	// -= Selection =-
	//
	// min() and max() are variadic in GDScript. The code generator folds the
	// tail into a chain of two-argument calls, so the forms below take two.
	{ "min", GlobalFn::MIN, GlobalKind::NUMERIC, 2, 63, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::MINI, GlobalFn::MINF },
	{ "mini", GlobalFn::MINI, GlobalKind::INT_OP, 2, 2, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "minf", GlobalFn::MINF, GlobalKind::FLOAT_OP, 2, 2, GlobalResult::FLOAT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "max", GlobalFn::MAX, GlobalKind::NUMERIC, 2, 63, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::MAXI, GlobalFn::MAXF },
	{ "maxi", GlobalFn::MAXI, GlobalKind::INT_OP, 2, 2, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "maxf", GlobalFn::MAXF, GlobalKind::FLOAT_OP, 2, 2, GlobalResult::FLOAT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "clamp", GlobalFn::CLAMP, GlobalKind::NUMERIC, 3, 3, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::CLAMPI, GlobalFn::CLAMPF },
	{ "clampi", GlobalFn::CLAMPI, GlobalKind::INT_OP, 3, 3, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "clampf", GlobalFn::CLAMPF, GlobalKind::FLOAT_OP, 3, 3, GlobalResult::FLOAT, NO_OP, 0, NO_FORM, NO_FORM },

	// -= Modulo and wrapping =-
	{ "posmod", GlobalFn::POSMOD, GlobalKind::INT_OP, 2, 2, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "fmod", GlobalFn::FMOD, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_FMOD, 2, NO_FORM, NO_FORM },
	{ "fposmod", GlobalFn::FPOSMOD, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_FPOSMOD, 2, NO_FORM, NO_FORM },
	{ "wrap", GlobalFn::WRAP, GlobalKind::NUMERIC, 3, 3, GlobalResult::NUMERIC, NO_OP, 0, GlobalFn::WRAPI, GlobalFn::WRAPF },
	{ "wrapi", GlobalFn::WRAPI, GlobalKind::INT_OP, 3, 3, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "wrapf", GlobalFn::WRAPF, GlobalKind::SYSCALL, 3, 3, GlobalResult::FLOAT, UTILITY_WRAP, 3, NO_FORM, NO_FORM },

	// -= Powers, roots and logarithms =-
	{ "sqrt", GlobalFn::SQRT, GlobalKind::FLOAT_OP, 1, 1, GlobalResult::FLOAT, NO_OP, 0, NO_FORM, NO_FORM },
	{ "pow", GlobalFn::POW, GlobalKind::SYSCALL, 2, 2, GlobalResult::FLOAT, UTILITY_POW, 2, NO_FORM, NO_FORM },
	{ "exp", GlobalFn::EXP, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_EXP, 1, NO_FORM, NO_FORM },
	{ "log", GlobalFn::LOG, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_LOG, 1, NO_FORM, NO_FORM },

	// -= Trigonometry =-
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

	// -= Decibels =-
	{ "linear_to_db", GlobalFn::LINEAR_TO_DB, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_LINEAR_TO_DB, 1, NO_FORM, NO_FORM },
	{ "db_to_linear", GlobalFn::DB_TO_LINEAR, GlobalKind::SYSCALL, 1, 1, GlobalResult::FLOAT, UTILITY_DB_TO_LINEAR, 1, NO_FORM, NO_FORM },

	// -= Interpolation =-
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
	{ "bezier_interpolate", GlobalFn::BEZIER_INTERPOLATE, GlobalKind::SYSCALL, 5, 5, GlobalResult::FLOAT, UTILITY_BEZIER_INTERPOLATE, 5, NO_FORM, NO_FORM },
	{ "bezier_derivative", GlobalFn::BEZIER_DERIVATIVE, GlobalKind::SYSCALL, 5, 5, GlobalResult::FLOAT, UTILITY_BEZIER_DERIVATIVE, 5, NO_FORM, NO_FORM },

	// -= Predicates =-
	{ "is_nan", GlobalFn::IS_NAN, GlobalKind::SYSCALL, 1, 1, GlobalResult::BOOL, UTILITY_IS_NAN, 1, NO_FORM, NO_FORM },
	{ "is_inf", GlobalFn::IS_INF, GlobalKind::SYSCALL, 1, 1, GlobalResult::BOOL, UTILITY_IS_INF, 1, NO_FORM, NO_FORM },
	{ "is_finite", GlobalFn::IS_FINITE, GlobalKind::SYSCALL, 1, 1, GlobalResult::BOOL, UTILITY_IS_FINITE, 1, NO_FORM, NO_FORM },
	{ "is_zero_approx", GlobalFn::IS_ZERO_APPROX, GlobalKind::SYSCALL, 1, 1, GlobalResult::BOOL, UTILITY_IS_ZERO_APPROX, 1, NO_FORM, NO_FORM },
	{ "is_equal_approx", GlobalFn::IS_EQUAL_APPROX, GlobalKind::SYSCALL, 2, 2, GlobalResult::BOOL, UTILITY_IS_EQUAL_APPROX, 2, NO_FORM, NO_FORM },

	// -= Variant queries =-
	{ "str", GlobalFn::STR, GlobalKind::HOST, 1, 63, GlobalResult::STRING, UTILITY_STR, 0, NO_FORM, NO_FORM },
	{ "len", GlobalFn::LEN, GlobalKind::HOST, 1, 1, GlobalResult::INT, UTILITY_LEN, 0, NO_FORM, NO_FORM },

	// -= Forms with no GDScript name of their own =-
	//
	// floor(), ceil() and round() of an integer are that integer. The
	// dispatchers above name this as their integer form; nothing can call it
	// directly, which is why the name is not a valid identifier.
	{ ".int_identity", GlobalFn::INT_IDENTITY, GlobalKind::INT_OP, 1, 1, GlobalResult::INT, NO_OP, 0, NO_FORM, NO_FORM },
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
		// Every GlobalFn has a row; a missing one is a table that was not
		// updated alongside the enum.
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

// -= Evaluation =-
//
// Godot's math_funcs.h, transcribed. Where Godot's version is written against
// real_t -- smoothstep's near-equality test, wrapf's -- this uses double, so
// that a program means the same thing in a single- and a double-precision
// build.

static double eval_sign(double x) {
	// Godot's SIGN(): zero, and NaN, are neither positive nor negative.
	return (x < 0.0) ? -1.0 : ((x > 0.0) ? 1.0 : 0.0);
}

static bool eval_is_equal_approx(double a, double b) {
	// Exact equality first, so that infinities compare equal.
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

	switch (utility_op) {
		case UTILITY_FLOOR: return std::floor(a);
		case UTILITY_CEIL: return std::ceil(a);
		// Godot rounds half away from zero through floor(), not through
		// ::round(), and the two differ for values where adding 0.5 is not
		// exact.
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
		case UTILITY_CUBIC_INTERPOLATE: {
			// cubic_interpolate(from, to, pre, post, weight)
			const double from = a, to = b, pre = c, post = d, weight = e;
			return 0.5 *
				((from * 2.0) +
					(-pre + to) * weight +
					(2.0 * pre - 5.0 * from + 4.0 * to - post) * (weight * weight) +
					(-pre + 3.0 * from - 3.0 * to + post) * (weight * weight * weight));
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
			throw CompilerException(ErrorType::CODEGEN_ERROR,
				"str() and len() need the host's Variant API and cannot be evaluated here");
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
			// The sign-mask form rather than std::abs(): the emitted RISC-V
			// wraps on INT64_MIN, and std::abs() of INT64_MIN is undefined.
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
			// Godot leaves a zero divisor to the C++ `%`, which traps. The
			// backend cannot trap usefully inside the sandbox, so both sides
			// answer zero -- the same thing integer division by zero does here.
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

double eval_global_float(GlobalFn fn, const double* args, size_t count) {
	const GlobalFunction& info = global_function(fn);

	if (info.kind == GlobalKind::SYSCALL) {
		double packed[UTILITY_MAX_FLOAT_ARGS] = { 0, 0, 0, 0, 0 };
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
		default:
			throw CompilerException(ErrorType::CODEGEN_ERROR,
				std::string(info.name) + " is not a floating-point operation");
	}
}

} // namespace gdscript
