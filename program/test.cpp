#include "syscalls.h"
#include "variant.hpp"
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

template <typename T>
using remove_cvref = std::remove_cv_t<std::remove_reference_t<T>>;

/// --> Fucking ugly side <-- ///

extern "C" __attribute__((used, retain, noreturn)) void fast_exit() {
	asm(".insn i SYSTEM, 0, x0, x0, 0x7ff");
	__builtin_unreachable();
}

MAKE_SYSCALL(ECALL_PRINT, void, sys_print, const Variant *, size_t);

/// --> Nice side <-- ///

struct UtilityFunctions {
	template <typename... Args>
	static void print(Args &&...vars) {
		std::array<Variant, sizeof...(Args)> vptrs;
		int idx = 0;
		([&] {
			if constexpr (std::is_same_v<Variant, remove_cvref<Args>>)
				vptrs[idx++] = vars;
			else
				vptrs[idx++] = Variant(vars);
		}(),
				...);
		sys_print(vptrs.data(), vptrs.size());
	}
};

#include "variant.hpp"

int main() {
	UtilityFunctions::print("Hello, ", 55, " world!\n");

	// do shit here...

	fast_exit();
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
