#pragma once
#include "function_signature.h"
#include "line_table.h"
#include <cstdint>
#include <string>
#include <vector>

namespace gdscript {

struct ScriptMetadata {
	bool double_precision = false;
	bool is_tool = false;
	bool base_is_path = false;

	std::string class_name;
	std::string base_class;

	std::vector<FunctionSignature> functions;
	std::vector<FunctionSignature> signals;
	LineTable line_table;
};

constexpr const char *GDSMETA_SECTION = ".gdsmeta";

std::vector<uint8_t> encode_script_metadata(const ScriptMetadata &meta);

bool decode_script_metadata(const uint8_t *data, size_t size, ScriptMetadata &out);

} // namespace gdscript
