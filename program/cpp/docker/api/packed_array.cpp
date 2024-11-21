#include "variant.hpp"

#include "syscalls_fwd.hpp"

EXTERN_SYSCALL(void, sys_vcreate, Variant *, int, int, const void *);
EXTERN_SYSCALL(void, sys_vstore, unsigned *, Variant::Type, const void *, size_t);
EXTERN_SYSCALL(void, sys_vfetch, unsigned, void *, int);

template <>
PackedArray<uint8_t>::PackedArray(const std::vector<uint8_t> &data) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_BYTE_ARRAY, 0, &data);
	this->m_idx = v.get_internal_index();
}
template <>
PackedArray<uint8_t>::PackedArray(const uint8_t *data, size_t size) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_BYTE_ARRAY, size, data);
	this->m_idx = v.get_internal_index();
}
template <>
PackedArray<int32_t>::PackedArray(const std::vector<int32_t> &data) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_INT32_ARRAY, 0, &data);
	this->m_idx = v.get_internal_index();
}
template <>
PackedArray<int64_t>::PackedArray(const std::vector<int64_t> &data) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_INT64_ARRAY, 0, &data);
	this->m_idx = v.get_internal_index();
}
template <>
PackedArray<float>::PackedArray(const std::vector<float> &data) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_FLOAT32_ARRAY, 0, &data);
	this->m_idx = v.get_internal_index();
}
template <>
PackedArray<double>::PackedArray(const std::vector<double> &data) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_FLOAT64_ARRAY, 0, &data);
	this->m_idx = v.get_internal_index();
}
template <>
PackedArray<Vector2>::PackedArray(const std::vector<Vector2> &data) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_VECTOR2_ARRAY, 0, &data);
	this->m_idx = v.get_internal_index();
}
template <>
PackedArray<Vector3>::PackedArray(const std::vector<Vector3> &data) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_VECTOR3_ARRAY, 0, &data);
	this->m_idx = v.get_internal_index();
}
template <>
PackedArray<Vector4>::PackedArray(const std::vector<Vector4> &data) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_VECTOR4_ARRAY, 0, &data);
	this->m_idx = v.get_internal_index();
}
template <>
PackedArray<Color>::PackedArray(const std::vector<Color> &data) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_COLOR_ARRAY, 0, &data);
	this->m_idx = v.get_internal_index();
}
template <>
PackedArray<std::string>::PackedArray(const std::vector<std::string> &data) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_STRING_ARRAY, 0, &data);
	this->m_idx = v.get_internal_index();
}

template <>
std::vector<uint8_t> PackedArray<uint8_t>::fetch() const {
	std::vector<uint8_t> result;
	sys_vfetch(m_idx, &result, 0);
	return result;
}
template <>
std::vector<int32_t> PackedArray<int32_t>::fetch() const {
	std::vector<int32_t> result;
	sys_vfetch(m_idx, &result, 0);
	return result;
}
template <>
std::vector<int64_t> PackedArray<int64_t>::fetch() const {
	std::vector<int64_t> result;
	sys_vfetch(m_idx, &result, 0);
	return result;
}
template <>
std::vector<float> PackedArray<float>::fetch() const {
	std::vector<float> result;
	sys_vfetch(m_idx, &result, 0);
	return result;
}
template <>
std::vector<double> PackedArray<double>::fetch() const {
	std::vector<double> result;
	sys_vfetch(m_idx, &result, 0);
	return result;
}
template <>
std::vector<Vector2> PackedArray<Vector2>::fetch() const {
	std::vector<Vector2> result;
	sys_vfetch(m_idx, &result, 0);
	return result;
}
template <>
std::vector<Vector3> PackedArray<Vector3>::fetch() const {
	std::vector<Vector3> result;
	sys_vfetch(m_idx, &result, 0);
	return result;
}
template <>
std::vector<Vector4> PackedArray<Vector4>::fetch() const {
	std::vector<Vector4> result;
	sys_vfetch(m_idx, &result, 0);
	return result;
}
template <>
std::vector<Color> PackedArray<Color>::fetch() const {
	std::vector<Color> result;
	sys_vfetch(m_idx, &result, 0);
	return result;
}
template <>
std::vector<std::string> PackedArray<std::string>::fetch() const {
	std::vector<std::string> result;
	sys_vfetch(m_idx, &result, 0);
	return result;
}

template <>
void PackedArray<uint8_t>::store(const std::vector<uint8_t> &data) {
	sys_vstore(&m_idx, Variant::PACKED_BYTE_ARRAY, data.data(), data.size());
}
template <>
void PackedArray<int32_t>::store(const std::vector<int32_t> &data) {
	sys_vstore(&m_idx, Variant::PACKED_INT32_ARRAY, data.data(), data.size());
}
template <>
void PackedArray<int64_t>::store(const std::vector<int64_t> &data) {
	sys_vstore(&m_idx, Variant::PACKED_INT64_ARRAY, data.data(), data.size());
}
template <>
void PackedArray<float>::store(const std::vector<float> &data) {
	sys_vstore(&m_idx, Variant::PACKED_FLOAT32_ARRAY, data.data(), data.size());
}
template <>
void PackedArray<double>::store(const std::vector<double> &data) {
	sys_vstore(&m_idx, Variant::PACKED_FLOAT64_ARRAY, data.data(), data.size());
}
template <>
void PackedArray<Vector2>::store(const std::vector<Vector2> &data) {
	sys_vstore(&m_idx, Variant::PACKED_VECTOR2_ARRAY, data.data(), data.size());
}
template <>
void PackedArray<Vector3>::store(const std::vector<Vector3> &data) {
	sys_vstore(&m_idx, Variant::PACKED_VECTOR3_ARRAY, data.data(), data.size());
}
template <>
void PackedArray<Vector4>::store(const std::vector<Vector4> &data) {
	sys_vstore(&m_idx, Variant::PACKED_VECTOR4_ARRAY, data.data(), data.size());
}
template <>
void PackedArray<Color>::store(const std::vector<Color> &data) {
	sys_vstore(&m_idx, Variant::PACKED_COLOR_ARRAY, data.data(), data.size());
}
template <>
void PackedArray<std::string>::store(const std::vector<std::string> &data) {
	sys_vstore(&m_idx, Variant::PACKED_STRING_ARRAY, data.data(), data.size());
}

template <>
void PackedArray<uint8_t>::store(const uint8_t *data, size_t size) {
	sys_vstore(&m_idx, Variant::PACKED_BYTE_ARRAY, data, size);
}
template <>
void PackedArray<int32_t>::store(const int32_t *data, size_t size) {
	sys_vstore(&m_idx, Variant::PACKED_INT32_ARRAY, data, size);
}
template <>
void PackedArray<int64_t>::store(const int64_t *data, size_t size) {
	sys_vstore(&m_idx, Variant::PACKED_INT64_ARRAY, data, size);
}
template <>
void PackedArray<float>::store(const float *data, size_t size) {
	sys_vstore(&m_idx, Variant::PACKED_FLOAT32_ARRAY, data, size);
}
template <>
void PackedArray<double>::store(const double *data, size_t size) {
	sys_vstore(&m_idx, Variant::PACKED_FLOAT64_ARRAY, data, size);
}
template <>
void PackedArray<Vector2>::store(const Vector2 *data, size_t size) {
	sys_vstore(&m_idx, Variant::PACKED_VECTOR2_ARRAY, data, size);
}
template <>
void PackedArray<Vector3>::store(const Vector3 *data, size_t size) {
	sys_vstore(&m_idx, Variant::PACKED_VECTOR3_ARRAY, data, size);
}
template <>
void PackedArray<Vector4>::store(const Vector4 *data, size_t size) {
	sys_vstore(&m_idx, Variant::PACKED_VECTOR4_ARRAY, data, size);
}
template <>
void PackedArray<Color>::store(const Color *data, size_t size) {
	sys_vstore(&m_idx, Variant::PACKED_COLOR_ARRAY, data, size);
}
template <>
void PackedArray<std::string>::store(const std::string *data, size_t size) {
	sys_vstore(&m_idx, Variant::PACKED_STRING_ARRAY, data, size);
}
