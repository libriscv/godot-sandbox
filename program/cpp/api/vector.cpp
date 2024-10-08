#include <vector> // std::hash

#include "syscalls.h"
#include "vector.hpp"

MAKE_SYSCALL(ECALL_VEC3_OPS, void, sys_vec3_ops, Vector3 *v, Vector3 *other, Vec3_Op op); // NOLINT

float Vector3::length() const noexcept {
	register const Vector3 *vptr asm("a0") = this;
	register int op asm("a2") = int(Vec3_Op::LENGTH);
	register float length asm("fa0");
	register int syscall asm("a7") = ECALL_VEC3_OPS;

	__asm__ volatile("ecall"
					 : "=f"(length)
					 : "r"(op), "r"(vptr), "m"(*vptr), "r"(syscall));
	return length;
}

Vector3 Vector3::normalized() const noexcept {
	Vector3 result;

	register const Vector3 *vptr asm("a0") = this;
	register Vector3 *resptr asm("a1") = &result;
	register int op asm("a2") = int(Vec3_Op::NORMALIZE);
	register int syscall asm("a7") = ECALL_VEC3_OPS;

	__asm__ volatile("ecall"
					 : "=m"(*resptr)
					 : "r"(op), "r"(vptr), "m"(*vptr), "r"(resptr), "r"(syscall));
	return result;
}

float Vector3::dot(const Vector3 &other) const noexcept {
	register const Vector3 *vptr asm("a0") = this;
	register const Vector3 *otherptr asm("a1") = &other;
	register int op asm("a2") = int(Vec3_Op::DOT);
	register float dot asm("fa0");
	register int syscall asm("a7") = ECALL_VEC3_OPS;

	__asm__ volatile("ecall"
					 : "=f"(dot)
					 : "r"(op), "r"(vptr), "m"(*vptr), "r"(otherptr), "m"(*otherptr), "r"(syscall));
	return dot;
}

Vector3 Vector3::cross(const Vector3 &other) const noexcept {
	Vector3 result;

	register const Vector3 *vptr asm("a0") = this;
	register const Vector3 *otherptr asm("a1") = &other;
	register int op asm("a2") = int(Vec3_Op::CROSS);
	register Vector3 *resptr asm("a3") = &result;
	register int syscall asm("a7") = ECALL_VEC3_OPS;

	__asm__ volatile("ecall"
					 : "=m"(*resptr)
					 : "r"(op), "r"(vptr), "m"(*vptr), "r"(otherptr), "m"(*otherptr), "r"(resptr), "r"(syscall));
	return result;
}

float Vector3::distance_to(const Vector3 &other) const noexcept {
	register const Vector3 *vptr asm("a0") = this;
	register const Vector3 *otherptr asm("a1") = &other;
	register int op asm("a2") = int(Vec3_Op::DISTANCE_TO);
	register float distance asm("fa0");
	register int syscall asm("a7") = ECALL_VEC3_OPS;

	__asm__ volatile("ecall"
					 : "=f"(distance)
					 : "r"(op), "r"(vptr), "m"(*vptr), "r"(otherptr), "m"(*otherptr), "r"(syscall));
	return distance;
}

float Vector3::distance_squared_to(const Vector3 &other) const noexcept {
	register const Vector3 *vptr asm("a0") = this;
	register const Vector3 *otherptr asm("a1") = &other;
	register int op asm("a2") = int(Vec3_Op::DISTANCE_SQ_TO);
	register float distance asm("fa0");
	register int syscall asm("a7") = ECALL_VEC3_OPS;

	__asm__ volatile("ecall"
					 : "=f"(distance)
					 : "r"(op), "r"(vptr), "m"(*vptr), "r"(otherptr), "m"(*otherptr), "r"(syscall));
	return float(distance);
}

float Vector3::angle_to(const Vector3 &other) const noexcept {
	register const Vector3 *vptr asm("a0") = this;
	register const Vector3 *otherptr asm("a1") = &other;
	register int op asm("a2") = int(Vec3_Op::ANGLE_TO);
	register float angle asm("fa0");
	register int syscall asm("a7") = ECALL_VEC3_OPS;

	__asm__ volatile("ecall"
					 : "=f"(angle)
					 : "r"(op), "r"(vptr), "m"(*vptr), "r"(otherptr), "m"(*otherptr), "r"(syscall));
	return angle;
}

static_assert(sizeof(Vector3) == 12, "Vector3 size mismatch");
static_assert(alignof(Vector3) == 4, "Vector3 alignment mismatch");
