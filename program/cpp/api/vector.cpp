#include <vector> // std::hash

#include "syscalls.h"
#include "vector.hpp"

MAKE_SYSCALL(ECALL_VEC3_OPS, void, ecall_vec3_ops, int op, Vector3 *v);

float Vector3::length() const noexcept {
	register int op asm("a0") = int(Vec3_Op::LENGTH);
	register const Vector3 *vptr asm("a1") = this;
	register float length asm("fa0");
	register int syscall asm("a7") = ECALL_VEC3_OPS;

	__asm__ volatile("ecall"
					 : "=f"(length)
					 : "r"(op), "r"(vptr), "m"(*vptr), "r"(syscall));
	return length;
}

Vector3 Vector3::normalized() const noexcept {
	Vector3 result = *this;

	register int op asm("a0") = int(Vec3_Op::NORMALIZE);
	register Vector3 *vptr asm("a1") = &result;
	register int syscall asm("a7") = ECALL_VEC3_OPS;

	__asm__ volatile("ecall"
					 : "+m"(*vptr)
					 : "r"(op), "r"(vptr), "r"(syscall));
	return result;
}
