#include "variant.hpp"

#include "syscalls.h"

MAKE_SYSCALL(ECALL_VCALL, void, sys_vcall, Variant *, const char *, size_t, const Variant *, size_t, Variant &);
MAKE_SYSCALL(ECALL_VEVAL, bool, sys_veval, int, const Variant *, const Variant *, Variant *);
MAKE_SYSCALL(ECALL_VFREE, void, sys_vfree, Variant *);

void Variant::evaluate(const Operator &op, const Variant &a, const Variant &b, Variant &r_ret, bool &r_valid) {
	r_valid = sys_veval(op, &a, &b, &r_ret);
}

Variant::~Variant() {
	sys_vfree(this);
}
