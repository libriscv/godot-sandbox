#include <api.hpp>

extern "C" Variant my_function(Vector4 v) {
	print("Arg: ", v);
	return 123;
}

extern "C" Variant my_function2(String s, Array a) {
	print("Args: ", s, a);
	return {};
}

SANDBOX_API({
	.name = "my_function",
	.address = (void*)my_function,
	.description = "Takes a Vector4",
	.return_type = "int",
	.arguments = "Vector4 v",
}, {
	.name = "my_function2",
	.address = (void*)my_function2,
	.description = "Takes a String and an Array",
	.return_type = "void",
	.arguments = "String s, Array a",
});
