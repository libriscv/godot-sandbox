#include "api.hpp"

extern "C" Variant test_math_sin(double val) {
	return Math::sin(val);
}
extern "C" Variant test_math_cos(double val) {
	return Math::cos(val);
}
extern "C" Variant test_math_tan(double val) {
	return Math::tan(val);
}
extern "C" Variant test_math_asin(double val) {
	return Math::asin(val);
}
extern "C" Variant test_math_acos(double val) {
	return Math::acos(val);
}
extern "C" Variant test_math_atan(double val) {
	return Math::atan(val);
}
extern "C" Variant test_math_atan2(double x, double y) {
	return Math::atan2(x, y);
}
extern "C" Variant test_math_pow(double x, double y) {
	return Math::pow(x, y);
}

extern "C" Variant test_math_lerp(double a, double b, double t) {
	return Math::lerp(a, b, t);
}
extern "C" Variant test_math_smoothstep(double a, double b, double t) {
	return Math::smoothstep(a, b, t);
}
extern "C" Variant test_math_clamp(double x, double a, double b) {
	return Math::clamp(x, a, b);
}
extern "C" Variant test_math_slerp(double a, double b, double t) {
	return Math::slerp(a, b, t);
}
