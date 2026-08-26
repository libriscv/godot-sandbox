#include "api.hpp"

struct MyException : public std::exception {
	using std::exception::exception;
	const char *what() const noexcept override {
		return "This is a test exception";
	}
};

PUBLIC Variant test_exceptions() {
#ifdef ZIG_COMPILER
#warning "Zig does not support exceptions (yet)"
	return "This is a test exception";
#else
	try {
		throw MyException();
	} catch (const std::exception &e) {
		return e.what();
	}
	return "";
#endif
}

// This works: it's being created during initialization
static Dictionary d = Dictionary::Create();

PUBLIC Variant test_static_storage(Variant key, Variant val) {
	d[key] = val;
	return d;
}
PUBLIC Variant test_failing_static_storage(Variant key, Variant val) {
	// This works only once: it's being created after initialization
	static Dictionary fd = Dictionary::Create();
	fd[key] = val;
	return fd;
}
static Dictionary fd = Dictionary::Create();
PUBLIC Variant test_permanent_storage(Variant key, Variant val) {
	fd[key] = val;
	fd = Variant(fd).make_permanent();
	return fd;
}

static String ps = "Hello this is a permanent string";
PUBLIC Variant test_permanent_string(String input) {
	ps = input;
	return ps;
}

static Array pa = Array::Create();
PUBLIC Variant test_permanent_array(Array input) {
	pa = input;
	return pa;
}

static Dictionary pd = Dictionary::Create();
PUBLIC Variant test_permanent_dict(Dictionary input) {
	pd = input;
	return pd;
}

PUBLIC Variant test_check_if_permanent(String test) {
	if (test == "string") {
		printf("Checking if string %d is permanent\n", ps.get_variant_index());
		return ps.is_permanent();
	} else if (test == "array") {
		printf("Checking if array %d is permanent\n", pa.get_variant_index());
		return pa.is_permanent();
	} else if (test == "dict") {
		printf("Checking if dictionary %d is permanent\n", pd.get_variant_index());
		return pd.is_permanent();
	}
	return false;
}

PUBLIC Variant test_infinite_loop() {
	while (true)
		;
}

PUBLIC Variant test_recursive_calls(Node sandbox) {
	sandbox("vmcall", "test_recursive_calls", sandbox);
	return {};
}

PUBLIC Variant public_function() {
	return "Hello from the other side";
}

PUBLIC Variant test_ping_pong(Variant arg) {
	return arg;
}

PUBLIC Variant test_ping_move_pong(Variant arg) {
	Variant v = std::move(arg);
	return v;
}

PUBLIC Variant test_variant_eq(Variant arg1, Variant arg2) {
	return arg1 == arg2;
}

PUBLIC Variant test_variant_neq(Variant arg1, Variant arg2) {
	return (arg1 != arg2) == false;
}

PUBLIC Variant test_variant_lt(Variant arg1, Variant arg2) {
	return arg1 < arg2;
}

PUBLIC Variant test_bool(bool arg) {
	return arg;
}

PUBLIC Variant test_int(long arg) {
	return arg;
}

PUBLIC Variant test_float(double arg) {
	return arg;
}

PUBLIC Variant test_string(String arg) {
	return arg;
}

PUBLIC Variant test_fetch_string(String arg) {
	return std::string(arg.utf8());
}

PUBLIC Variant test_u32string(String arg) {
	std::u32string u32 = arg.utf32();
	return u32;
}

PUBLIC Variant test_nodepath(NodePath arg) {
	return arg;
}

PUBLIC Variant test_vec2(Vector2 arg) {
	Vector2 result = arg;
	return result;
}
PUBLIC Variant test_vec2i(Vector2i arg) {
	Vector2i result = arg;
	return result;
}

PUBLIC Variant test_vec3(Vector3 arg) {
	Vector3 result = arg;
	return result;
}
PUBLIC Variant test_vec3i(Vector3i arg) {
	Vector3i result = arg;
	return result;
}

PUBLIC Variant test_vec4(Vector4 arg) {
	Vector4 result = arg;
	return result;
}
PUBLIC Variant test_vec4i(Vector4i arg) {
	Vector4i result = arg;
	return result;
}

PUBLIC Variant test_color(Color arg) {
	Color result = arg;
	return result;
}

PUBLIC Variant test_plane(Plane arg) {
	Plane result = arg;
	return result;
}

PUBLIC Variant test_array(Array array) {
	array.push_back(2);
	array.push_back("4");
	array.push_back(6.0);
	if (array[0] != 2 || array[1] != "4" || array[2] != 6.0) {
		return "Fail";
	}
	if (!(array[0] == 2 && array[1] == "4" && array[2] == 6.0)) {
		return "Fail";
	}
	array[0] = 1;
	array[1] = "2";
	array[2] = 3.0;
	if (int(array[0]) != 1 || String(array[1]) != "2" || double(array[2]) != 3.0) {
		return "Fail";
	}
	if (int(array[0]) == 1 && String(array[1]) == "2" || double(array[2]) == 3.0) {
		return array;
	}
	return "Fail";
}

PUBLIC Variant test_array_assign(Array arr) {
	arr[0] = 42;
	arr[1] = "Hello";
	arr[2] = PackedArray<double> ({ 3.14, 2.71 });
	if (arr[0] != 42 || arr[1] != "Hello" || arr[2].get().get_type() != Variant::Type::PACKED_FLOAT64_ARRAY) {
		return "Fail";
	}

	Array arr2 = Array::Create();
	arr2.push_back(PackedArray<double> ({ 1.0, 2.0, 3.0 }));
	arr.push_back(arr2);
	arr[3] = arr2;

	PackedArray<double> pa = arr[2];
	if (pa.size() != 2 || pa.is_empty()) {
		return "Fail";
	}
	std::vector<double> vec = pa.fetch();
	if (vec[0] != 3.14 || vec[1] != 2.71 || vec.size() != 2) {
		return "Fail";
	}

	return arr;
}

PUBLIC Variant test_array_assign2(Array arr, const size_t idx) {

	std::vector<size_t> indices = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };

	arr.resize(indices.size());
	for (size_t i = 0; i < indices.size(); i++) {
		arr[i] = indices[i];
	}

	return arr;
}

PUBLIC Variant test_dict(Dictionary arg) {
	return arg;
}

PUBLIC Variant test_sub_dictionary(Dictionary dict) {
	return Dictionary(dict["1"].value());
}

PUBLIC Variant test_rid(RID rid) {
	return rid;
}

PUBLIC Variant test_object(Object arg) {
	Object result = arg;
	return result;
}

// The host caches method and property names keyed on the guest address they came from,
// so it has to notice when a guest reuses one buffer for a different name. Both names
// here are the same length, which leaves the text comparison as the only thing that can
// tell them apart.
PUBLIC Variant test_reused_name_buffer(Object obj) {
	Array results = Array::Create();
	char buffer[32];

	__builtin_memcpy(buffer, "get_class", 10);
	results.push_back(obj.callv(std::string_view(buffer, 9), false, nullptr, 0));
	__builtin_memcpy(buffer, "get_index", 10);
	results.push_back(obj.callv(std::string_view(buffer, 9), false, nullptr, 0));
	__builtin_memcpy(buffer, "get_class", 10);
	results.push_back(obj.callv(std::string_view(buffer, 9), false, nullptr, 0));

	// The same buffer again, now naming two properties instead of two methods.
	__builtin_memcpy(buffer, "editor_description", 19);
	obj.set(std::string_view(buffer, 18), "described");
	__builtin_memcpy(buffer, "name", 5);
	obj.set(std::string_view(buffer, 4), "renamed");
	results.push_back(obj.get(std::string_view(buffer, 4)));
	__builtin_memcpy(buffer, "editor_description", 19);
	results.push_back(obj.get(std::string_view(buffer, 18)));

	return results;
}

PUBLIC Variant test_basis(Basis basis) {
	Basis b = basis;
	return b;
}

PUBLIC Variant test_transform2d(Transform2D transform2d) {
	Transform2D t2d = transform2d;
	return t2d;
}

PUBLIC Variant test_transform3d(Transform3D transform3d) {
	Transform3D t3d = transform3d;
	return t3d;
}

PUBLIC Variant test_quaternion(Quaternion quaternion) {
	Quaternion q2 = quaternion;
	return q2;
}

PUBLIC Variant test_callable(Callable callable) {
	return callable.call(1, 2, "3");
}

// Stack-per-level probes. A nested call must run on a stack of its own, not on
// what the call it interrupted had left over.
PUBLIC Variant stack_probe() {
	volatile int local = 0;
	return int64_t(uintptr_t(&local));
}

// Reports this frame's stack address and the one the re-entrant call ran on.
PUBLIC Variant nested_stack_probe(Callable reenter) {
	volatile int local = 0;
	Array result = Array::Create();
	result.push_back(int64_t(uintptr_t(&local)));
	result.push_back(reenter.call());
	return result;
}

// clang-format off
PUBLIC Variant test_create_callable() {
	Array array = Array::Create();
	array.push_back(1);
	array.push_back(2);
	array.push_back("3");
	return Callable::Create<Variant(Array, int, int, String)>([](Array array, int a, int b, String c) -> Variant {
		return a + b + std::stoi(c.utf8()) + int(array.at(0)) + int(array.at(1)) + std::stoi(array.at(2).as_string().utf8());
	}, array);
}
// clang-format on

PUBLIC Variant test_pa_u8(PackedByteArray arr) {
	return PackedByteArray (arr.fetch());
}
PUBLIC Variant test_pa_f32(PackedArray<float> arr) {
	return PackedArray<float> (arr.fetch());
}
PUBLIC Variant test_pa_f64(PackedArray<double> arr) {
	return PackedArray<double> (arr.fetch());
}
PUBLIC Variant test_pa_i32(PackedArray<int32_t> arr) {
	return PackedArray<int32_t> (arr.fetch());
}
PUBLIC Variant test_pa_i64(PackedArray<int64_t> arr) {
	return PackedArray<int64_t> (arr.fetch());
}
PUBLIC Variant test_pa_vec2(PackedArray<Vector2> arr) {
	return PackedArray<Vector2> (arr.fetch());
}
PUBLIC Variant test_pa_vec3(PackedArray<Vector3> arr) {
	return PackedArray<Vector3> (arr.fetch());
}
PUBLIC Variant test_pa_vec4(PackedArray<Vector4> arr) {
	return PackedArray<Vector4> (arr.fetch());
}
PUBLIC Variant test_pa_color(PackedArray<Color> arr) {
	return PackedArray<Color> (arr.fetch());
}
PUBLIC Variant test_pa_string(PackedArray<std::string> arr) {
	return PackedArray<std::string> (arr.fetch());
}

PUBLIC Variant test_create_pa_u8() {
	PackedByteArray arr({ 1, 2, 3, 4 });
	return arr;
}
static const uint8_t pa_u8_data[] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
PUBLIC Variant test_create_pa_u8_ptr() {
	return PackedByteArray(pa_u8_data, sizeof(pa_u8_data) / sizeof(uint8_t));
}
PUBLIC Variant test_create_pa_f32() {
	PackedArray<float> arr({ 1, 2, 3, 4 });
	return arr;
}
PUBLIC Variant test_create_pa_f64() {
	PackedArray<double> arr({ 1, 2, 3, 4 });
	return arr;
}
PUBLIC Variant test_create_pa_i32() {
	PackedArray<int32_t> arr({ 1, 2, 3, 4 });
	return arr;
}
PUBLIC Variant test_create_pa_i64() {
	PackedArray<int64_t> arr({ 1, 2, 3, 4 });
	return arr;
}
PUBLIC Variant test_create_pa_vec2() {
	PackedArray<Vector2> arr({ { 1, 1 }, { 2, 2 }, { 3, 3 } });
	return arr;
}
PUBLIC Variant test_create_pa_vec3() {
	PackedArray<Vector3> arr({ { 1, 1, 1 }, { 2, 2, 2 }, { 3, 3, 3 } });
	return arr;
}
PUBLIC Variant test_create_pa_vec4() {
	PackedArray<Vector4> arr({ { 1, 1, 1, 1 }, { 2, 2, 2, 2 }, { 3, 3, 3, 3 } });
	return arr;
}
PUBLIC Variant test_create_pa_color() {
	PackedArray<Color> arr({ { 0, 0, 0, 0 }, { 1, 1, 1, 1 } });
	return arr;
}
PUBLIC Variant test_create_pa_string() {
	PackedArray<std::string> arr({ "Hello", "from", "the", "other", "side" });
	return arr;
}

PUBLIC Variant test_string_append(String s) {
	s.append(String(" from"));
	s.append(std::string_view(" the other side"));
	return s;
}

PUBLIC Variant test_nodepath_append(NodePath p) {
	p.append(String("/Child"));
	return p;
}

static String permanent_appendable = "perm";
PUBLIC Variant test_permanent_string_append() {
	permanent_appendable.append(String("+"));
	return permanent_appendable;
}

PUBLIC Variant test_pa_store_reuse() {
	PackedArray<int64_t> pa({ 0, 0, 0 });
	const unsigned idx = pa.get_variant_index();
	for (int64_t i = 0; i < 500; i++) {
		pa.store(std::vector<int64_t>{ i, i + 1, i + 2 });
		if (pa.get_variant_index() != idx) {
			return "Fail: store() did not re-use the Variant slot";
		}
	}
	return pa;
}

PUBLIC Variant test_transform2d_rotate_reuse() {
	Transform2D t = Transform2D::identity();
	const unsigned idx = t.get_variant_index();
	for (int i = 0; i < 500; i++) {
		t.rotate(0.001);
		if (t.get_variant_index() != idx) {
			return "Fail: rotate() did not re-use the Variant slot";
		}
	}
	return "OK";
}

PUBLIC Variant test_transform2d_identity() {
	return Transform2D::identity();
}
PUBLIC Variant test_transform3d_identity() {
	return Transform3D::identity();
}
PUBLIC Variant test_basis_identity() {
	return Basis::identity();
}
PUBLIC Variant test_quaternion_identity() {
	return Quaternion::identity();
}

PUBLIC Variant test_assign_pa_to_array(PackedArray<int64_t> pa) {
	Array arr = Array::Create();
	arr.push_back(pa);
	arr.push_back(pa);
	return arr;
}

PUBLIC Variant test_assign_pa_to_dict(PackedArray<int64_t> arr) {
	Dictionary d = Dictionary::Create();
	d["a1"] = arr;
	d["a2"] = arr;
	return d;
}

PUBLIC Variant test_construct_pa_from_array_at(Array arr, int idx) {
	PackedArray<int64_t> pa(arr.at(idx));
	return pa;
}

PUBLIC Variant test_exception() {
	asm volatile("unimp");
	return "This should not be reached";
}

static bool timer_got_called = false;
PUBLIC Variant test_timers() {
	long val1 = 11;
	float val2 = 22.0f;
	return CallbackTimer::periodic(0.01, [=](Node timer) -> Variant {
		print("Timer with values: ", val1, val2);
		timer.queue_free();
		timer_got_called = true;
		return {};
	});
}
PUBLIC Variant verify_timers() {
	return timer_got_called;
}

PUBLIC Variant call_method(Variant v, Variant vmethod, Variant vargs) {
	std::string method = vmethod.as_std_string();
	Array args_array = vargs.as_array();
	std::vector<Variant> args = args_array.to_vector();
	Variant ret;
	v.callp(method, args.data(), args.size(), ret);
	return ret;
}

PUBLIC Variant voidcall_method(Variant v, Variant vmethod, Variant vargs) {
	std::string method = vmethod.as_std_string();
	Array args_array = vargs.as_array();
	std::vector<Variant> args = args_array.to_vector();
	v.voidcallp(method, args.data(), args.size());
	return Nil;
}

// A RefCounted reaching the guest as the return value of a call is owned by nothing but
// the temporary Variant that carried it across the API boundary. Touching the handle
// afterwards only works if the sandbox took a reference of its own while it was there.
PUBLIC Variant test_refcounted_result(Object maker) {
	Variant made = maker.call("make_ref");
	Array results = Array::Create();
	results.push_back(made.method_call("get_class"));
	results.push_back(made.method_call("get_meta", "marker"));
	return results;
}

// The same object, but reached through an Array rather than directly, so that it is the
// element and not the call result that has to stay alive.
PUBLIC Variant test_refcounted_in_array(Object maker) {
	Array made = maker.call("make_ref_array").as_array();
	Variant first = made[0];
	return first.method_call("get_meta", "marker");
}

static Object stored_object{ uint64_t(0) };

PUBLIC Variant store_object(Object obj) {
	stored_object = obj;
	return Nil;
}

PUBLIC Variant use_stored_object() {
	return stored_object.get_class();
}

PUBLIC Variant granted_mid_call(Callable grant) {
	Object obj = grant.call();
	stored_object = obj;
	return obj.get_class();
}

PUBLIC Variant access_a_parent(Node n) {
	Node p = n.get_parent();
	return p;
}

PUBLIC Variant creates_a_node() {
	return Node::Create("test");
}

PUBLIC Variant free_self() {
	get_node()("free");
	return Nil;
}

PUBLIC Variant access_an_invalid_child_node() {
	Node n = Node::Create("test");
	Node c = Node::Create("child");
	n.add_child(c);
	c("free");
	c.set_name("child2");
	return c;
}

PUBLIC Variant access_an_invalid_child_resource(String path) {
	Variant resource = loadv(path.utf8());
	return resource.method_call("get_class");
}

PUBLIC Variant disable_restrictions() {
	get_node().call("disable_restrictions");
	return Nil;
}

PUBLIC Variant test_property_proxy() {
	Node node = Node::Create("Fail 1");
	node.name() = "Fail 1.5";
	node.set_name("Fail 2");
	if (node.get_name() == "Fail 2") {
		node.set("name", "Fail 3");
		if (node.get("name") == "Fail 3") {
			node.name() = "TestOK";
			if (node.name() != "TestOK") {
				return "Fail 4";
			}
		}
	}
	return node.get_name();
}

// This tests the higher limit for boxed arguments with up to 16 arguments
// We will pass in 10 integers and 6 strings, which we add up and return
PUBLIC Variant test_many_arguments(Variant a1, Variant a2, Variant a3, Variant a4, Variant a5, Variant a6, Variant a7, Variant a8, Variant a9, Variant a10, Variant a11, Variant a12, Variant a13, Variant a14, Variant a15, Variant a16) {
	return int(a1) + int(a2) + int(a3) + int(a4) + int(a5) + int(a6) + int(a7) + int(a8) + int(a9) + int(a10) + a11.as_string().to_int() + a12.as_string().to_int() + a13.as_string().to_int() + a14.as_string().to_int() + a15.as_string().to_int() + a16.as_string().to_int();
}

PUBLIC Variant test_many_arguments2(Variant a1, Variant a2, Variant a3, Variant a4, Variant a5, Variant a6, Variant a7, Variant a8) {
	return int(a1) + int(a2) + int(a3) + int(a4) + int(a5) + int(a6) + int(a7) + a8.as_string().to_int();
}

PUBLIC Variant test_many_unboxed_arguments(int a1, int a2, int a3, int a4, int a5, int a6, int a7, double f1, double f2, double f3, double f4) {
	return int(a1) + int(a2) + int(a3) + int(a4) + int(a5) + int(a6) + int(a7) + int(f1) + int(f2) + int(f3) + int(f4);
}

PUBLIC Variant test_many_unboxed_arguments2(int a1, int a2, int a3, int a4, int a5, int a6, int a7, Vector2 v1, Vector2 v2, Vector2 v3, Vector2 v4) {
	return int(a1) + int(a2) + int(a3) + int(a4) + int(a5) + int(a6) + int(a7) + int(v1.x) + int(v1.y) + int(v2.x) + int(v2.y) + int(v3.x) + int(v3.y) + int(v4.x) + int(v4.y);
}

PUBLIC Variant get_tree_base_parent() {
	return get_parent();
}

// -= Coroutines, written out by hand =-
// What the compiler will emit for an `await`, spelled as C++ so the host side can be
// tested on its own: a frame of Variant slots, an ECALL_AWAIT that hands it over, and a
// resume entry that asks for it back and dispatches on the state it is given.
MAKE_SYSCALL(ECALL_AWAIT, long, sys_await, const Variant *, void *, unsigned, int, void *, int);
MAKE_SYSCALL(ECALL_AWAIT_RESTORE, long, sys_await_restore, void *, unsigned);

extern "C" Variant await_probe_resume();

// Awaits its argument and answers 100 more than the value it was resumed with. The
// frame is three slots: the parameter, the await's result, and one spare.
PUBLIC Variant await_probe(Variant awaited) {
	Variant frame[3];
	frame[0] = awaited; // A coroutine copies its parameters in before it can suspend.
	if (sys_await(&frame[0], frame, sizeof(frame), 1,
				(void *)&await_probe_resume, 1 * sizeof(Variant))) {
		return Variant(); // Suspend epilogue: the host answers the caller, not this.
	}
	// Not awaitable, so the result slot is already written and nothing suspended.
	return int64_t(frame[1]) + 100;
}

// The resume entry supplies its own frame; the host checks the length against the one
// the suspension recorded and does the copy itself.
extern "C" PUBLIC Variant await_probe_resume() {
	Variant frame[3];
	const long state = sys_await_restore(frame, sizeof(frame));
	switch (state) {
		case 1:
			return int64_t(frame[1]) + 100;
		default:
			return Variant();
	}
}

// Same shape, but suspends twice, to pin that a frame surviving several resumes keeps
// working and that what it held between them comes back.
extern "C" Variant await_twice_resume();

PUBLIC Variant await_twice(Variant awaited) {
	Variant frame[4];
	frame[0] = awaited;
	frame[2] = Variant(1000); // Carried across both suspensions.
	if (sys_await(&frame[0], frame, sizeof(frame), 1,
				(void *)&await_twice_resume, 1 * sizeof(Variant))) {
		return Variant();
	}
	return int64_t(frame[1]) + int64_t(frame[2]);
}

extern "C" PUBLIC Variant await_twice_resume() {
	Variant frame[4];
	const long state = sys_await_restore(frame, sizeof(frame));
	if (state == 1) {
		// Accumulate and go around again on the same signal.
		frame[2] = int64_t(frame[2]) + int64_t(frame[1]);
		if (sys_await(&frame[0], frame, sizeof(frame), 2,
					(void *)&await_twice_resume, 1 * sizeof(Variant))) {
			return Variant();
		}
	}
	return int64_t(frame[1]) + int64_t(frame[2]);
}

// A resume that re-enters the Sandbox through the host: the Callable it is handed calls
// another exported coroutine on this same Sandbox. That nested invocation must get a
// frame of its own -- adopting the one being resumed would overwrite it mid-flight.
extern "C" Variant await_nested_resume();

PUBLIC Variant await_nested(Variant awaited, Variant callback) {
	Variant frame[4];
	frame[0] = awaited;
	frame[2] = callback;
	frame[3] = Variant(1000); // Must still read back as 1000 after the nested call.
	if (sys_await(&frame[0], frame, sizeof(frame), 1,
				(void *)&await_nested_resume, 1 * sizeof(Variant))) {
		return Variant();
	}
	return int64_t(frame[1]) + int64_t(frame[3]);
}

extern "C" PUBLIC Variant await_nested_resume() {
	Variant frame[4];
	const long state = sys_await_restore(frame, sizeof(frame));
	if (state == 1) {
		frame[2].as_callable().call();
		return int64_t(frame[1]) + int64_t(frame[3]);
	}
	return Variant();
}

// A frame holding a String: a non-inlined Variant is an index into the *call's* scoped
// variants, so this only survives a suspension if the host promoted it.
extern "C" Variant await_handle_resume();

PUBLIC Variant await_handle(Variant awaited, Variant text) {
	Variant frame[3];
	frame[0] = awaited;
	frame[2] = text;
	if (sys_await(&frame[0], frame, sizeof(frame), 1,
				(void *)&await_handle_resume, 1 * sizeof(Variant))) {
		return Variant();
	}
	return frame[2];
}

extern "C" PUBLIC Variant await_handle_resume() {
	Variant frame[3];
	const long state = sys_await_restore(frame, sizeof(frame));
	if (state == 1) {
		String text = frame[2].as_string();
		text += frame[1].as_string();
		return text;
	}
	return Variant();
}

// Mislabelled tag, real scoped container: the host must deny on what it resolved.
PUBLIC Variant readonly_tag_spoof(Variant container, Variant method) {
	Variant spoofed;
	__builtin_memcpy(&spoofed, &container, sizeof(Variant));
	const int32_t tag = int32_t(Variant::STRING);
	__builtin_memcpy(&spoofed, &tag, sizeof(tag));
	spoofed.method_call(method.operator std::string());
	return "the mutation was not denied";
}
