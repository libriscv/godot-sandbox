#pragma once

#include "variant.hpp"
#include <string>

/**
 * @brief String wrapper for Godot String.
 * Implemented by referencing and mutating a host-side String Variant.
 */
union String {
	constexpr String() {} // DON'T TOUCH
	String(std::string_view value);
	template <size_t N>
	String(const char (&value)[N]);
	String(const std::string &value);

	String &operator =(std::string_view value);
	String &operator =(const String &value);

	// String operations
	void append(const String &value);
	void append(std::string_view value);
	void erase(int idx, int count = 1);
	void insert(int idx, const String &value);
	int find(const String &value) const;
	bool contains(std::string_view value) const { return find(value) != -1; }
	bool empty() const { return size() == 0; }

	// Call methods on the String
	template <typename... Args>
	Variant operator () (std::string_view method, Args&&... args);

	String &operator +=(const String &value) { append(value); return *this; }
	String &operator +=(std::string_view value) { append(value); return *this; }

	// String access
	String operator[](int idx) const;
	String at(int idx) const { return (*this)[idx]; }

	operator std::string() const { return utf8(); }
	operator std::u32string() const { return utf32(); }
	std::string utf8() const;
	std::u32string utf32() const;

	bool operator ==(const String &other) const;
	bool operator ==(const char *other) const;

	// String size
	int size() const;
	bool is_empty() const { return size() == 0; }

	METHOD(begins_with);
	METHOD(bigrams);
	METHOD(bin_to_int);
	METHOD(c_escape);
	METHOD(c_unescape);
	METHOD(capitalize);
	METHOD(casecmp_to);
	METHOD(chr);
	METHOD(containsn);
	METHOD(count);
	METHOD(countn);
	METHOD(dedent);
	METHOD(ends_with);
	METHOD(filecasecmp_to);
	METHOD(filenocasecmp_to);
	METHOD(findn);
	METHOD(format);
	METHOD(get_base_dir);
	METHOD(get_basename);
	METHOD(get_extension);
	METHOD(get_file);
	METHOD(get_slice);
	METHOD(get_slice_count);
	METHOD(get_slicec);
	METHOD(hash);
	METHOD(hex_decode);
	METHOD(hex_to_int);
	METHOD(humanize_size);
	METHOD(indent);
	METHOD(is_absolute_path);
	METHOD(is_relative_path);
	METHOD(is_subsequence_of);
	METHOD(is_subsequence_ofn);
	METHOD(is_valid_filename);
	METHOD(is_valid_float);
	METHOD(is_valid_hex_number);
	METHOD(is_valid_html_color);
	METHOD(is_valid_identifier);
	METHOD(is_valid_int);
	METHOD(is_valid_ip_address);
	METHOD(join);
	METHOD(json_escape);
	METHOD(left);
	METHOD(length);
	METHOD(lpad);
	METHOD(lstrip);
	METHOD(match);
	METHOD(matchn);
	METHOD(md5_buffer);
	METHOD(md5_text);
	METHOD(naturalcasecmp_to);
	METHOD(naturalnocasecmp_to);
	METHOD(nocasecmp_to);
	METHOD(num);
	METHOD(num_int64);
	METHOD(num_scientific);
	METHOD(num_uint64);
	METHOD(pad_decimals);
	METHOD(pad_zeros);
	METHOD(path_join);
	METHOD(repeat);
	METHOD(replace);
	METHOD(replacen);
	METHOD(reverse);
	METHOD(rfind);
	METHOD(rfindn);
	METHOD(right);
	METHOD(rpad);
	METHOD(rsplit);
	METHOD(rstrip);
	METHOD(sha1_buffer);
	METHOD(sha1_text);
	METHOD(sha256_buffer);
	METHOD(sha256_text);
	METHOD(similarity);
	METHOD(simplify_path);
	METHOD(split);
	METHOD(split_floats);
	METHOD(strip_edges);
	METHOD(strip_escapes);
	METHOD(substr);
	METHOD(to_ascii_buffer);
	METHOD(to_camel_case);
	METHOD(to_float);
	METHOD(to_int);
	METHOD(to_lower);
	METHOD(to_pascal_case);
	METHOD(to_snake_case);
	METHOD(to_upper);
	METHOD(to_utf8_buffer);
	METHOD(to_utf16_buffer);
	METHOD(to_utf32_buffer);
	METHOD(to_wchar_buffer);
	METHOD(trim_prefix);
	METHOD(trim_suffix);
	METHOD(unicode_at);
	METHOD(uri_decode);
	METHOD(uri_encode);
	METHOD(validate_filename);
	METHOD(validate_node_name);
	METHOD(xml_escape);
	METHOD(xml_unescape);

	static String from_variant_index(unsigned idx) { String a {}; a.m_idx = idx; return a; }
	unsigned get_variant_index() const noexcept { return m_idx; }
	bool is_permanent() const { return Variant::is_permanent_index(m_idx); }
	static unsigned Create(const char *data, size_t size);

private:
	unsigned m_idx = INT32_MIN;
};
using NodePath = String; // NodePath is compatible with String

inline Variant::Variant(const String &s) {
	m_type = Variant::STRING;
	v.i = s.get_variant_index();
}

inline Variant::operator String() const {
	return as_string();
}

inline String Variant::as_string() const {
	if (m_type != STRING && m_type != STRING_NAME && m_type != NODE_PATH) {
		api_throw("std::bad_cast", "Failed to cast Variant to String", this);
	}
	return String::from_variant_index(v.i);
}

inline String::String(std::string_view value)
	: m_idx(Create(value.data(), value.size())) {}
inline String &String::operator =(std::string_view value) {
	m_idx = Create(value.data(), value.size());
	return *this;
}
template <size_t N>
inline String::String(const char (&value)[N])
	: m_idx(Create(value, N - 1)) {}

inline String::String(const std::string &value)
	: m_idx(Create(value.data(), value.size())) {}

template <typename... Args>
inline Variant String::operator () (std::string_view method, Args&&... args) {
	return Variant(*this).method_call(method, std::forward<Args>(args)...);
}
