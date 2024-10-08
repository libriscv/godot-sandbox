#include "transform2d.hpp"

#include "syscalls.h"

MAKE_SYSCALL(ECALL_TRANSFORM_2D_OPS, void, sys_transform2d_ops, unsigned, Transform2D_Op, ...);

Transform2D Transform2D::identity() {
	Transform2D t;
	sys_transform2d_ops(0, Transform2D_Op::IDENTITY, &t);
	return t;
}
