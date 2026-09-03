#include "property_signature.h"

#include <cstring>
#include <limits>
#include <type_traits>

namespace gdscript {
namespace {

constexpr uint32_t MAGIC = 0x50524753u; // "SGRP" in little endian.
constexpr uint16_t VERSION_MAJOR = 1;
constexpr uint16_t VERSION_MINOR = 1;
constexpr size_t MAX_BLOB_SIZE = 16u * 1024u * 1024u;
constexpr uint32_t MAX_RECORDS = 100000u;
constexpr uint32_t MAX_STRING_SIZE = 1024u * 1024u;

template <typename T>
void write_scalar(std::vector<uint8_t> &out, T value) {
	static_assert(std::is_trivially_copyable<T>::value, "raw bytes only");
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
	out.insert(out.end(), bytes, bytes + sizeof(T));
}

void write_string(std::vector<uint8_t> &out, const std::string &value) {
	write_scalar<uint32_t>(out, uint32_t(value.size()));
	out.insert(out.end(), value.begin(), value.end());
}

struct Reader {
	const uint8_t *data = nullptr;
	size_t size = 0;
	size_t offset = 0;
	bool ok = true;

	template <typename T>
	T scalar() {
		T value{};
		if (!ok || offset > size || sizeof(T) > size - offset) {
			ok = false;
			return value;
		}
		std::memcpy(&value, data + offset, sizeof(T));
		offset += sizeof(T);
		return value;
	}

	std::string string() {
		const uint32_t length = scalar<uint32_t>();
		if (!ok || length > MAX_STRING_SIZE || offset > size || length > size - offset) {
			ok = false;
			return {};
		}
		std::string value(reinterpret_cast<const char *>(data + offset), length);
		offset += length;
		return value;
	}
};

} // namespace

std::vector<uint8_t> encode_property_signatures(const std::vector<PropertySignature> &properties) {
	std::vector<uint8_t> out;
	write_scalar<uint32_t>(out, MAGIC);
	write_scalar<uint16_t>(out, VERSION_MAJOR);
	write_scalar<uint16_t>(out, VERSION_MINOR);
	write_scalar<uint32_t>(out, uint32_t(properties.size()));

	for (const PropertySignature &property : properties) {
		write_string(out, property.name);
		write_scalar<int32_t>(out, property.type);
		write_string(out, property.class_name);
		write_scalar<uint32_t>(out, property.hint);
		write_string(out, property.hint_string);
		write_scalar<uint32_t>(out, property.usage);
		write_scalar<uint32_t>(out, property.declaration_line);
		write_string(out, property.section.category);
		write_string(out, property.section.group);
		write_string(out, property.section.group_prefix);
		write_string(out, property.section.subgroup);
		write_string(out, property.section.subgroup_prefix);
		write_scalar<uint8_t>(out, property.is_member ? 1 : 0);
		write_scalar<uint8_t>(out, property.is_static ? 1 : 0);
		write_scalar<uint8_t>(out, uint8_t(property.default_kind));
		switch (property.default_kind) {
			case PropertyDefaultKind::INT:
				write_scalar<int64_t>(out, std::get<int64_t>(property.default_value));
				break;
			case PropertyDefaultKind::FLOAT:
				write_scalar<double>(out, std::get<double>(property.default_value));
				break;
			case PropertyDefaultKind::BOOL:
				write_scalar<uint8_t>(out, std::get<bool>(property.default_value) ? 1 : 0);
				break;
			case PropertyDefaultKind::STRING:
				write_string(out, std::get<std::string>(property.default_value));
				break;
			case PropertyDefaultKind::NONE:
			case PropertyDefaultKind::NIL:
			case PropertyDefaultKind::EMPTY_ARRAY:
			case PropertyDefaultKind::EMPTY_DICTIONARY:
				break;
		}
	}
	return out;
}

bool decode_property_signatures(const uint8_t *data, size_t size,
		std::vector<PropertySignature> &out) {
	out.clear();
	if (data == nullptr || size > MAX_BLOB_SIZE) {
		return false;
	}
	Reader reader{data, size};
	if (reader.scalar<uint32_t>() != MAGIC ||
			reader.scalar<uint16_t>() != VERSION_MAJOR) {
		return false;
	}
	const uint16_t minor = reader.scalar<uint16_t>();
	if (!reader.ok || minor > VERSION_MINOR) {
		return false;
	}
	const uint32_t count = reader.scalar<uint32_t>();
	if (!reader.ok || count > MAX_RECORDS || count > size) {
		return false;
	}

	std::vector<PropertySignature> staged;
	staged.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		PropertySignature property;
		property.name = reader.string();
		property.type = reader.scalar<int32_t>();
		property.class_name = reader.string();
		property.hint = reader.scalar<uint32_t>();
		property.hint_string = reader.string();
		property.usage = reader.scalar<uint32_t>();
		property.declaration_line = reader.scalar<uint32_t>();
		if (minor >= 1) {
			property.section.category = reader.string();
			property.section.group = reader.string();
			property.section.group_prefix = reader.string();
			property.section.subgroup = reader.string();
			property.section.subgroup_prefix = reader.string();
		}
		const uint8_t member = reader.scalar<uint8_t>();
		const uint8_t is_static = reader.scalar<uint8_t>();
		const uint8_t kind = reader.scalar<uint8_t>();
		if (!reader.ok || property.name.empty() || member > 1 || is_static > 1 ||
				kind > uint8_t(PropertyDefaultKind::EMPTY_DICTIONARY) ||
				property.type < -1 || property.type > 64) {
			return false;
		}
		property.is_member = member != 0;
		property.is_static = is_static != 0;
		property.default_kind = PropertyDefaultKind(kind);
		switch (property.default_kind) {
			case PropertyDefaultKind::INT:
				property.default_value = reader.scalar<int64_t>();
				break;
			case PropertyDefaultKind::FLOAT:
				property.default_value = reader.scalar<double>();
				break;
			case PropertyDefaultKind::BOOL: {
				const uint8_t value = reader.scalar<uint8_t>();
				if (value > 1) {
					return false;
				}
				property.default_value = value != 0;
				break;
			}
			case PropertyDefaultKind::STRING:
				property.default_value = reader.string();
				break;
			case PropertyDefaultKind::NONE:
			case PropertyDefaultKind::NIL:
			case PropertyDefaultKind::EMPTY_ARRAY:
			case PropertyDefaultKind::EMPTY_DICTIONARY:
				break;
		}
		if (!reader.ok) {
			return false;
		}
		staged.push_back(std::move(property));
	}
	if (!reader.ok || reader.offset != size) {
		return false;
	}
	out = std::move(staged);
	return true;
}

} // namespace gdscript
