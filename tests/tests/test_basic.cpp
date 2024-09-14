#include "api.hpp"

extern "C" Variant public_function() {
	return "Hello from the other side";
}

extern "C" Variant test_ping_pong(Variant arg) {
	return arg;
}

extern "C" Variant test_bool(bool arg) {
	return arg;
}

extern "C" Variant test_int(long arg) {
	return arg;
}

extern "C" Variant test_float(double arg) {
	return arg;
}

extern "C" Variant test_string(String arg) {
	return arg;
}

extern "C" Variant test_nodepath(NodePath arg) {
	return arg;
}

extern "C" Variant test_vec2(Vector2 arg) {
	return arg;
}

extern "C" Variant test_vec2i(Vector2i arg) {
	return arg;
}

extern "C" Variant test_array(Array arg) {
	return arg;
}

extern "C" Variant test_dict(Dictionary arg) {
	return arg;
}

extern "C" Variant test_sub_dictionary(Dictionary dict) {
	return Dictionary(dict)["1"];
}

extern "C" Variant test_object(Object arg) {
	return arg;
}

extern "C" Variant test_exception() {
	asm("unimp");
	__builtin_unreachable();
}

static bool timer_got_called = false;
extern "C" Variant test_timers() {
	long val1 = 11;
	float val2 = 22.0f;
	return Timer::native_periodic(0.01, [=](Node timer) -> Variant {
		print("Timer with values: ", val1, val2);
		timer.queue_free();
		timer_got_called = true;
		return {};
	});
}
extern "C" Variant verify_timers() {
	return timer_got_called;
}
