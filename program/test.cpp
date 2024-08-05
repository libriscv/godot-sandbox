#include "api.hpp"
#include <cstdio>
#include <string_view>

int main() {
	UtilityFunctions::print("Hello, ", 55, " world!\n");

	// do shit here...

	halt(); // Prevent stdout,stderr closing etc.
}

extern "C" void empty_function() {
}

extern "C" void my_function(Variant varg) {
	UtilityFunctions::print("Hello, ", 124.5, " world!\n");
	UtilityFunctions::print("Arg: ", varg);
}

extern "C" void function3(Variant x, Variant y, Variant text) {
	UtilityFunctions::print("x = ", x, " y = ", y, " text = ", text);
}

extern "C" void final_function() {
	UtilityFunctions::print("The function was called!!\n");
}

static Variant copy;

extern "C" void trampoline_function(Variant callback) {
	UtilityFunctions::print("Trampoline is calling first argument...\n");
	callback.call(1, 2, 3);
	copy = callback;
	UtilityFunctions::print("After call...\n");
}

extern "C" void failing_function() {
	copy.call(1, 2, 3);
}

extern "C" void test_buffer(Variant var) {
	auto data = var.operator std::string_view();

	char buffer[256];
	for (size_t i = 0; i < data.size(); i++)
		snprintf(buffer + i * 3, 4, "%02x ", data[i]);
	buffer[192] = '\n';

	UtilityFunctions::print(buffer);
}
