#include "api.hpp"
#include <cstdio>

int main() {
	UtilityFunctions::print("Hello, ", 55, " world!\n");

	// do shit here...

	halt(); // Prevent stdout,stderr closing etc.
}

extern "C" Variant empty_function() {
	return Variant();
}

extern "C" Variant my_function(Variant varg) {
	UtilityFunctions::print("Hello, ", 124.5, " world!\n");
	UtilityFunctions::print("Arg: ", varg);
	return 1234; //
}

extern "C" Variant function3(Variant x, Variant y, Variant text) {
	UtilityFunctions::print("x = ", x, " y = ", y, " text = ", text);
	return float(int(x) + float(y));
}

extern "C" Variant final_function() {
	UtilityFunctions::print("The function was called!!\n");
	return Variant();
}

static Variant copy;

extern "C" Variant trampoline_function(Variant callback) {
	UtilityFunctions::print("Trampoline is calling first argument...\n");
	callback.call(1, 2, 3);
	copy = callback;
	UtilityFunctions::print("After call...\n");
	return Variant();
}

extern "C" Variant failing_function() {
	copy.call(1, 2, 3);
	return Variant();
}

extern "C" Variant test_buffer(Variant var) {
	auto data = var.operator std::string_view();

	char buffer[256];
	snprintf(buffer, sizeof(buffer),
			"The buffer is not here! Or is it? T12345\n");

	UtilityFunctions::print(buffer);
	return Variant();
}
