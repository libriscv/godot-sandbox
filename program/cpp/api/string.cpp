#include "string.hpp"

#include "syscalls.h"

MAKE_SYSCALL(ECALL_STRING_CREATE, unsigned, sys_string_create, const char *, size_t);
MAKE_SYSCALL(ECALL_STRING_OPS, int, sys_string_ops, String_Op, unsigned, int, Variant *);
MAKE_SYSCALL(ECALL_STRING_AT, unsigned, sys_string_at, unsigned, int);
MAKE_SYSCALL(ECALL_STRING_SIZE, int, sys_string_size, unsigned);
MAKE_SYSCALL(ECALL_STRING_APPEND, void, sys_string_append, unsigned, const char *, size_t);
EXTERN_SYSCALL(unsigned, sys_vassign, unsigned, unsigned);

String &String::operator=(const String &value) {
	this->m_idx = sys_vassign(m_idx, value.m_idx);
	return *this;
}

void String::append(const String &value) {
	(void)sys_string_ops(String_Op::APPEND, m_idx, 0, (Variant *)&value);
}

void String::insert(int idx, const String &value) {
	(void)sys_string_ops(String_Op::INSERT, m_idx, 0, (Variant *)&value);
}

void String::append(std::string_view value) {
	(void)sys_string_append(m_idx, value.data(), value.size());
}

void String::erase(int idx, int count) {
	sys_string_ops(String_Op::ERASE, m_idx, idx, (Variant *)(uintptr_t)count);
}

int String::find(const String &value) const {
	return sys_string_ops(String_Op::FIND, m_idx, 0, (Variant *)&value);
}

String String::operator[](int idx) const {
	unsigned new_stridx = sys_string_at(m_idx, idx);
	return String::from_variant_index(new_stridx);
}

int String::size() const {
	return sys_string_size(m_idx);
}

unsigned String::Create(const char *data, size_t size) {
	return sys_string_create(data, size);
}

std::string String::utf8() const {
	std::string str;
	sys_string_ops(String_Op::TO_STD_STRING, m_idx, 0, (Variant *)&str);
	return str;
}

std::u32string String::utf32() const {
	std::u32string str;
	sys_string_ops(String_Op::TO_STD_STRING, m_idx, 2, (Variant *)&str);
	return str;
}

bool String::operator==(const String &other) const {
	return sys_string_ops(String_Op::COMPARE, m_idx, 0, (Variant *)&other);
}

bool String::operator==(const char *other) const {
	return sys_string_ops(String_Op::COMPARE_CSTR, m_idx, 0, (Variant *)other);
}
