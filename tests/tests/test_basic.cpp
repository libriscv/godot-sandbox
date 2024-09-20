#include "api.hpp"

extern "C" Variant test_infinite_loop() {
	while (true)
		;
}

extern "C" Variant test_recursive_calls(Node sandbox) {
	sandbox("vmcall", "test_recursive_calls", sandbox);
	return {};
}

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

extern "C" Variant test_color(Color arg) {
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

extern "C" Variant test_callable(Variant callable) {
	return callable.call(1, 2, "3");
}

extern "C" Variant test_pa_u8(PackedArray<uint8_t> arr) {
	return arr;
}
extern "C" Variant test_pa_f32(PackedArray<float> arr) {
	return arr;
}
extern "C" Variant test_pa_f64(PackedArray<double> arr) {
	return arr;
}
extern "C" Variant test_pa_i32(PackedArray<int32_t> arr) {
	return arr;
}
extern "C" Variant test_pa_i64(PackedArray<int64_t> arr) {
	return arr;
}
extern "C" Variant test_pa_vec2(PackedArray<Vector2> arr) {
	return arr;
}
extern "C" Variant test_pa_vec3(PackedArray<Vector3> arr) {
	return arr;
}
extern "C" Variant test_pa_color(PackedArray<Color> arr) {
	return arr;
}

extern "C" Variant test_create_pa_u8() {
	PackedArray<uint8_t> arr({ 1, 2, 3, 4 });
	return arr;
}
extern "C" Variant test_create_pa_f32() {
	PackedArray<float> arr({ 1, 2, 3, 4 });
	return arr;
}
extern "C" Variant test_create_pa_f64() {
	PackedArray<double> arr({ 1, 2, 3, 4 });
	return arr;
}
extern "C" Variant test_create_pa_i32() {
	PackedArray<int32_t> arr({ 1, 2, 3, 4 });
	return arr;
}
extern "C" Variant test_create_pa_i64() {
	PackedArray<int64_t> arr({ 1, 2, 3, 4 });
	return arr;
}
extern "C" Variant test_create_pa_vec2() {
	PackedArray<Vector2> arr({ { 1, 1 }, { 2, 2 }, { 3, 3 } });
	return arr;
}
extern "C" Variant test_create_pa_vec3() {
	PackedArray<Vector3> arr({ { 1, 1, 1 }, { 2, 2, 2 }, { 3, 3, 3 } });
	return arr;
}
extern "C" Variant test_create_pa_color() {
	PackedArray<Color> arr({ { 0, 0, 0, 0 }, { 1, 1, 1, 1 } });
	return arr;
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
