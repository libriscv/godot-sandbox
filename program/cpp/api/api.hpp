#pragma once
#include <cstddef>
#include "node2d.hpp"
#include "syscalls_fwd.hpp"

template <typename T>
using remove_cvref = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename... Args>
inline void print(Args &&...vars) {
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

inline void halt() {
	fast_exit();
}
