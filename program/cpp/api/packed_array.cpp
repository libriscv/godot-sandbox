#include "variant.hpp"

#include "syscalls.h"

EXTERN_SYSCALL(ECALL_VCREATE, void, sys_vcreate, Variant *, int, int, const void *);
EXTERN_SYSCALL(ECALL_VSTORE, void, sys_vstore, unsigned, const void *, size_t);
EXTERN_SYSCALL(ECALL_VFETCH, void, sys_vfetch, unsigned, void *, int);

template <>
PackedArray<uint8_t>::PackedArray(const std::vector<uint8_t> &data) {
	Variant v;
	sys_vcreate(&v, Variant::PACKED_BYTE_ARRAY, 0, &data);
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
void PackedArray<uint8_t>::store(const std::vector<uint8_t> &data) {
	sys_vstore(m_idx, data.data(), data.size());
}
template <>
void PackedArray<int32_t>::store(const std::vector<int32_t> &data) {
	sys_vstore(m_idx, data.data(), data.size());
}
template <>
void PackedArray<int64_t>::store(const std::vector<int64_t> &data) {
	sys_vstore(m_idx, data.data(), data.size());
}
template <>
void PackedArray<float>::store(const std::vector<float> &data) {
	sys_vstore(m_idx, data.data(), data.size());
}
template <>
void PackedArray<double>::store(const std::vector<double> &data) {
	sys_vstore(m_idx, data.data(), data.size());
}
