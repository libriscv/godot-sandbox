#pragma once

#include "variant.hpp"

struct Callable {
	constexpr Callable() {}
	template <typename F>
	Callable(F *f, const Variant &args = Nil);

	/// @brief Create a callable from a function pointer, which always returns a Variant.
	/// @tparam F The function type.
	/// @param f The function pointer.
	/// @param args The arguments to pass to the function.
	template <typename F>
	static Callable Create(F *f, const Variant &args = Nil);

	/// @brief Call the function with the given arguments.
	/// @tparam Args The argument types.
	/// @param args The arguments.
	template <typename... Args>
	Variant operator () (Args&&... args);

	/// @brief Call the function with the given arguments.
	/// @tparam Args The argument types.
	/// @param args The arguments.
	template <typename... Args>
	Variant call(Args&&... args);

	static Callable from_variant_index(unsigned idx) { Callable a; a.m_idx = idx; return a; }
	unsigned get_variant_index() const noexcept { return m_idx; }

private:
	unsigned m_idx = INT32_MIN;
};

inline Variant::Variant(const Callable &callable) {
	m_type = CALLABLE;
	v.i = callable.get_variant_index();
}

inline Callable Variant::as_callable() const {
	if (m_type != CALLABLE) {
		api_throw("std::bad_cast", "Failed to cast Variant to Callable", this);
	}
	return Callable::from_variant_index(v.i);
}

inline Variant::operator Callable() const {
	return as_callable();
}

template <typename... Args>
inline Variant Callable::operator () (Args&&... args) {
	return Variant(*this)(std::forward<Args>(args)...);
}

template <typename... Args>
inline Variant Callable::call(Args&&... args) {
	return Variant(*this).call(std::forward<Args>(args)...);
}

template <typename F>
inline Callable Callable::Create(F *f, const Variant &args) {
	unsigned idx = sys_callable_create((void (*)())f, &args, nullptr, 0);

	return Callable::from_variant_index(idx);
}

template <typename F>
inline Callable::Callable(F *f, const Variant &args)
	: m_idx(sys_callable_create((void (*)())f, &args, nullptr, 0)) {
}
