#include "api.hpp"
#include <cstdio>

int main() {
	UtilityFunctions::print("main()");
	halt();
}

extern "C" void my_function(Variant variant) {
	UtilityFunctions::print("Arg: ", variant);
}

extern "C" void my_function2(Variant variant) {
	UtilityFunctions::print("Arg: ", variant);
}
