#include <api.hpp>
#include <fmt/format.h>

extern "C" Variant test_function() {
	fmt::print("Hello, World!\n");
	return 42;
}
