#include "variant.hpp"

#include "syscalls.h"

MAKE_SYSCALL(ECALL_VCALL, void, sys_vcall, Variant *, const char *, size_t, const Variant *, size_t, Variant &);
MAKE_SYSCALL(ECALL_VEVAL, bool, sys_veval, int, const Variant *, const Variant *, Variant *);
MAKE_SYSCALL(ECALL_VFREE, void, sys_vfree, Variant *);

MAKE_SYSCALL(ECALL_VCREATE, void, sys_vcreate, Variant *, int, const void *);
MAKE_SYSCALL(ECALL_VFETCH, void, sys_vfetch, const Variant *, void *);
MAKE_SYSCALL(ECALL_VCLONE, void, sys_vclone, const Variant *, Variant *);
MAKE_SYSCALL(ECALL_VSTORE, void, sys_vstore, Variant *, const std::string *);

Variant Variant::new_array() {
	Variant v;
	sys_vcreate(&v, ARRAY, nullptr);
	return v;
}

Variant Variant::from_array(const std::vector<Variant> &values) {
	Variant v;
	sys_vcreate(&v, ARRAY, &values);
	return v;
}

Variant Variant::new_dictionary() {
	Variant v;
	sys_vcreate(&v, DICTIONARY, nullptr);
	return v;
}

void Variant::evaluate(const Operator &op, const Variant &a, const Variant &b, Variant &r_ret, bool &r_valid) {
	r_valid = sys_veval(op, &a, &b, &r_ret);
}

std::vector<uint8_t> Variant::as_byte_array() const {
	if (m_type == PACKED_BYTE_ARRAY) {
		std::vector<uint8_t> result;
		sys_vfetch(this, &result);
		return result;
	}
	api_throw("std::bad_cast", "Failed to cast Variant to PackedByteArray", this);
}

std::vector<float> Variant::as_float32_array() const {
	if (m_type == PACKED_FLOAT32_ARRAY) {
		std::vector<float> result;
		sys_vfetch(this, &result);
		return result;
	}
	api_throw("std::bad_cast", "Failed to cast Variant to PackedFloat32Array", this);
}

std::vector<double> Variant::as_float64_array() const {
	if (m_type == PACKED_FLOAT64_ARRAY) {
		std::vector<double> result;
		sys_vfetch(this, &result);
		return result;
	}
	api_throw("std::bad_cast", "Failed to cast Variant to PackedFloat64Array", this);
}

std::vector<int32_t> Variant::as_int32_array() const {
	if (m_type == PACKED_INT32_ARRAY) {
		std::vector<int32_t> result;
		sys_vfetch(this, &result);
		return result;
	}
	api_throw("std::bad_cast", "Failed to cast Variant to PackedInt32Array", this);
}

std::vector<int64_t> Variant::as_int64_array() const {
	if (m_type == PACKED_INT64_ARRAY) {
		std::vector<int64_t> result;
		sys_vfetch(this, &result);
		return result;
	}
	api_throw("std::bad_cast", "Failed to cast Variant to PackedInt64Array", this);
}

std::vector<Vector2> Variant::as_vector2_array() const {
	if (m_type == PACKED_VECTOR2_ARRAY) {
		std::vector<Vector2> result;
		sys_vfetch(this, &result);
		return result;
	}
	api_throw("std::bad_cast", "Failed to cast Variant to PackedVector2Array", this);
}

std::vector<Vector3> Variant::as_vector3_array() const {
	if (m_type == PACKED_VECTOR3_ARRAY) {
		std::vector<Vector3> result;
		sys_vfetch(this, &result);
		return result;
	}
	api_throw("std::bad_cast", "Failed to cast Variant to PackedVector3Array", this);
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
