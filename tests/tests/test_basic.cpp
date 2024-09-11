#include "api.hpp"

extern "C" Variant public_function() {
	return "Hello from the other side";
}

extern "C" Variant test_dictionary(Variant dict) {
	return Dictionary(dict)["1"];
}
