#include "variant.hpp"

#include "syscalls.h"

MAKE_SYSCALL(ECALL_VCALL, void, sys_vcall, Variant *, const char *, size_t, const Variant *, size_t, Variant &);
MAKE_SYSCALL(ECALL_VEVAL, bool, sys_veval, int, const Variant *, const Variant *, Variant *);
MAKE_SYSCALL(ECALL_VFREE, void, sys_vfree, Variant *);

MAKE_SYSCALL(ECALL_VCREATE, void, sys_vcreate, Variant *, int, const std::string *);
MAKE_SYSCALL(ECALL_VFETCH, void, sys_vfetch, const Variant *, std::string *);
MAKE_SYSCALL(ECALL_VCLONE, void, sys_vclone, const Variant *, Variant *);
MAKE_SYSCALL(ECALL_VSTORE, void, sys_vstore, Variant *, const std::string *);

void Variant::evaluate(const Operator &op, const Variant &a, const Variant &b, Variant &r_ret, bool &r_valid) {
	r_valid = sys_veval(op, &a, &b, &r_ret);
}

void Variant::internal_create_string(Type type, const std::string &value) {
	sys_vcreate(this, type, &value);
}

std::string Variant::internal_fetch_string() const {
	std::string result;
	sys_vfetch(this, &result);
	return result;
}

void Variant::internal_clone(const Variant &other) {
	sys_vclone(&other, this);
}

Variant::~Variant() {
	register Variant *v asm("a0") = this;
	register int syscall asm("a7") = ECALL_VFREE;

	asm volatile(
			"ecall"
			:
			: "r"(v), "m"(*v), "r"(syscall));
}
