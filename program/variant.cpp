#include "variant.hpp"

#include "syscalls.h"

MAKE_SYSCALL(ECALL_VCALL, void, sys_pcall, Variant*, const char*, size_t, const Variant**, size_t, Variant&);
MAKE_SYSCALL(ECALL_VEVAL, bool, sys_veval, int, const Variant*, const Variant*, Variant*);

void Variant::callp(const std::string &method, const Variant **args, int argcount, Variant &r_ret, int &r_error)
{
	if (m_type != OBJECT) {
		r_error = 1;
		return;
	}

	sys_pcall(this, method.c_str(), method.size(), args, argcount, r_ret);
}

void Variant::evaluate(const Operator &op, const Variant &a, const Variant &b, Variant &r_ret, bool &r_valid)
{
	r_valid = sys_veval(op, &a, &b, &r_ret);
}
