#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace gdscript {

// Published beside the ELF; the ELF symbol table carries no arity or defaults.
// Free of compiler-internal headers so guest programs can include it.
struct FunctionParameter {
	static constexpr int32_t ANY_TYPE = -1;

	std::string name;
	int32_t type = ANY_TYPE;

	// NONE: no default or default does not fold to a constant (parameter stays required).
	enum class DefaultKind : uint8_t {
		NONE,
		NIL,
		INT,
		FLOAT,
		BOOL,
		STRING,
		EMPTY_ARRAY,
		EMPTY_DICT,
	};
	DefaultKind default_kind = DefaultKind::NONE;
	std::variant<int64_t, double, std::string, bool> default_value;

	bool optional() const { return default_kind != DefaultKind::NONE; }
};

struct FunctionSignature {
	std::string name;
	std::vector<FunctionParameter> parameters;
	int32_t return_type = FunctionParameter::ANY_TYPE;
	size_t required_arguments = 0;

	int32_t line = 0; // 1-based line of 'func' token; 0 when unknown
	std::string description; // '##' doc comment, lines joined by '\n'
};

// Encoded as a single blob (one scoped variant) rather than Array of Dictionaries
// to stay under Sandbox::MAX_REFS. Little-endian; layout documented in
// function_signature.cpp.
std::vector<uint8_t> encode_function_signatures(const std::vector<FunctionSignature> &signatures);

// Returns false on truncated or invalid blobs; output left empty.
bool decode_function_signatures(const uint8_t *data, size_t size,
	std::vector<FunctionSignature> &out);

} // namespace gdscript
