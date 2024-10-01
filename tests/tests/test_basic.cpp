#include "api.hpp"

// This works: it's being created during initialization
static Dictionary d = Dictionary::Create();

extern "C" Variant test_static_storage(Variant key, Variant val) {
	d[key] = val;
	return d;
}
extern "C" Variant test_failing_static_storage(Variant key, Variant val) {
	// This won't work: it's being created after initialization
	static Dictionary fd = Dictionary::Create();
	fd[key] = val;
	return fd;
}

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

extern "C" Variant test_vec3(Vector3 arg) {
	return arg;
}
extern "C" Variant test_vec3i(Vector3i arg) {
	return arg;
}

extern "C" Variant test_vec4(Vector4 arg) {
	return arg;
}
extern "C" Variant test_vec4i(Vector4i arg) {
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

extern "C" Variant test_callable(Callable callable) {
	return callable.call(1, 2, "3");
}

// clang-format off
extern "C" Variant test_create_callable() {
	Array array;
	array.push_back(1);
	array.push_back(2);
	array.push_back("3");
	return Callable::Create<Variant(Array, int, int, String)>([](Array array, int a, int b, String c) -> Variant {
		return a + b + std::stoi(c.utf8()) + int(array[0]) + int(array[1]) + std::stoi(array[2].as_string().utf8());
	}, array);
}
// clang-format on

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
extern "C" Variant test_pa_string(PackedArray<std::string> arr) {
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
extern "C" Variant test_create_pa_string() {
	PackedArray<std::string> arr({ "Hello", "from", "the", "other", "side" });
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

extern "C" Variant call_method(Variant v, Variant vmethod, Variant vargs) {
	std::string method = vmethod.as_std_string();
	Array args_array = vargs.as_array();
	std::vector<Variant> args = args_array.to_vector();
	Variant ret;
	v.callp(method, args.data(), args.size(), ret);
	return ret;
}

extern "C" Variant voidcall_method(Variant v, Variant vmethod, Variant vargs) {
	std::string method = vmethod.as_std_string();
	Array args_array = vargs.as_array();
	std::vector<Variant> args = args_array.to_vector();
	v.voidcallp(method, args.data(), args.size());
	return Nil;
}

extern "C" Variant access_a_parent(Node n) {
	Node p = n.get_parent();
	return Nil;
}

extern "C" Variant creates_a_node() {
	Node n = Node::create("test");
	return Nil;
}
