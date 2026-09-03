#pragma once

#include "export_hints.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace gdscript {

enum class PropertyDefaultKind : uint8_t {
	NONE,
	NIL,
	INT,
	FLOAT,
	BOOL,
	STRING,
	EMPTY_ARRAY,
	EMPTY_DICTIONARY,
};

using PropertyDefaultValue = std::variant<int64_t, double, bool, std::string>;

struct PropertySignature {
	std::string name;
	int32_t type = -1;
	std::string class_name;
	uint32_t hint = 0;
	std::string hint_string;
	uint32_t usage = 0;
	uint32_t declaration_line = 0;
	ExportSection section;
	bool is_member = false;
	bool is_static = false;
	PropertyDefaultKind default_kind = PropertyDefaultKind::NONE;
	PropertyDefaultValue default_value = int64_t(0);
};

std::vector<uint8_t> encode_property_signatures(const std::vector<PropertySignature> &properties);

// Decodes transactionally. On malformed or unsupported input, returns false and
// leaves `out` empty.
bool decode_property_signatures(const uint8_t *data, size_t size,
		std::vector<PropertySignature> &out);

} // namespace gdscript
