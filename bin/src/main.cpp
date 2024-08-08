#include "api.hpp"
#include <cstdio>

int main() {
	UtilityFunctions::print("main()");
	halt();
}

extern "C" Variant my_function(Variant variant) {
	UtilityFunctions::print("Arg: ", variant);
	return 123;
}

extern "C" Variant my_function2(Variant var1, Variant var2) {
	UtilityFunctions::print("Args: ", var1, var2);
	return 456;
}
