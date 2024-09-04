#include "dictionary.hpp"

#include "syscalls.h"

EXTERN_SYSCALL(ECALL_VCREATE, void, sys_vcreate, Variant *, int, const void *);
MAKE_SYSCALL(ECALL_DICTIONARY_OPS, int, sys_dict_ops, Dictionary_Op, unsigned, const Variant *, Variant *);

void Dictionary::clear() {
	(void)sys_dict_ops(Dictionary_Op::CLEAR, m_idx, nullptr, nullptr);
}

void Dictionary::erase(const Variant &key) {
	(void)sys_dict_ops(Dictionary_Op::ERASE, m_idx, &key, nullptr);
}

bool Dictionary::has(const Variant &key) const {
	return sys_dict_ops(Dictionary_Op::HAS, m_idx, &key, nullptr);
}

int Dictionary::size() const {
	return sys_dict_ops(Dictionary_Op::GET_SIZE, m_idx, nullptr, nullptr);
}

Variant Dictionary::get(const Variant &key) const {
	Variant v;
	(void)sys_dict_ops(Dictionary_Op::GET, m_idx, &key, &v);
	return v;
}
void Dictionary::set(const Variant &key, const Variant &value) {
	(void)sys_dict_ops(Dictionary_Op::SET, m_idx, &key, (Variant *)&value);
}

void Dictionary::merge(const Dictionary &other) {
	Variant v(other);
	(void)sys_dict_ops(Dictionary_Op::MERGE, m_idx, &v, nullptr);
}

Dictionary::Dictionary() {
	Variant v;
	sys_vcreate(&v, Variant::DICTIONARY, nullptr);
	this->m_idx = v.get_internal_index();
}

Dictionary::Dictionary(const Dictionary &other) {
	m_idx = other.m_idx;
}

Dictionary::Dictionary(Dictionary &&other) {
	m_idx = other.m_idx;
	// Invalidate the other dictionary?
}

Dictionary::~Dictionary() {
	// Do nothing, as we don't own the dictionary?
}

Dictionary &Dictionary::operator=(const Dictionary &other) {
	m_idx = other.m_idx;
	return *this;
}

Dictionary &Dictionary::operator=(Dictionary &&other) {
	m_idx = other.m_idx;
	return *this;
}
