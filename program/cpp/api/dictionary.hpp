#pragma once

#include "variant.hpp"
struct DictAccessor;

struct Dictionary {
	Dictionary();
	Dictionary(const Dictionary &other);
	Dictionary(Dictionary &&other);
	~Dictionary();

	Dictionary &operator=(const Dictionary &other);
	Dictionary &operator=(Dictionary &&other);

	operator Variant() const;

	void clear();
	void erase(const Variant &key);
	bool has(const Variant &key) const;
	void merge(const Dictionary &other);
	bool is_empty() const { return size() == 0; }
	int size() const;

	Variant get(const Variant &key) const;
	void set(const Variant &key, const Variant &value);

	DictAccessor operator[](const Variant &key);

	static Dictionary from_variant_index(unsigned idx) { Dictionary d; d.m_idx = idx; return d; }
	unsigned get_variant_index() const noexcept { return m_idx; }

private:
	unsigned m_idx = -1;
};

inline Dictionary::operator Variant() const {
	return Variant(*this);
}

inline Dictionary Variant::as_dictionary() const {
	if (m_type != DICTIONARY) {
		api_throw("std::bad_cast", "Failed to cast Variant to Dictionary", this);
	}
	return Dictionary::from_variant_index(v.i);
}

inline Variant::Variant(const Dictionary &d) {
	m_type = DICTIONARY;
	v.i = d.get_variant_index();
}

struct DictAccessor {
	DictAccessor(const Dictionary &dict, const Variant &key) : m_dict(dict), m_key(key) {}

	operator Variant() const { return m_dict.get(m_key); }
	Variant operator ->() const { return m_dict.get(m_key); }

	void operator=(const Variant &value) { m_dict.set(m_key, value); }

private:
	Dictionary m_dict;
	Variant m_key;
};

inline DictAccessor Dictionary::operator[](const Variant &key) {
	return DictAccessor(*this, key);
}
