#include "api.hpp"
#include <cstdio>

int main() {
	UtilityFunctions::print("Hello, ", 55, " world!\n");

	// do shit here...

	halt(); // Prevent stdout,stderr closing etc.
}

extern "C" void my_function3(Variant varg, Variant varg2) {
	UtilityFunctions::print("Hello, ", 124.5, " world!\n");
	UtilityFunctions::print("Arg: ", varg);
}
