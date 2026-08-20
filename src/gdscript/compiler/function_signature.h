#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace gdscript {

// What a caller outside the program has to know about a function it calls.
//
// The Sandbox ABI hands a guest function one Variant pointer per argument and
// no count, so the function cannot tell how many arguments it was given: an
// argument the caller left out is a null pointer, and reading a parameter out
// of it faults. The produced ELF says nothing about this -- its symbol table
// carries names alone -- so the compiler publishes the arity here, and whoever
// makes the call checks it and fills in the defaults.
//
// Deliberately free of the compiler's own headers: a guest program includes
// this through compiler.h, and there `Variant` is the sandbox API's Variant
// struct rather than the compiler's namespace of type constants.
struct FunctionParameter {
	// A declared type, as Variant::Type, or ANY_TYPE when the parameter is
	// untyped and may hold any Variant. A struct name is DICTIONARY: an
	// instance of one is an ordinary Dictionary.
	static constexpr int32_t ANY_TYPE = -1;

	std::string name;
	int32_t type = ANY_TYPE;

	// How the caller can produce the value when it leaves the argument out.
	// NONE covers both a parameter with no default and one whose default does
	// not fold to a constant: an expression can only be evaluated by running
	// guest code, and the callee cannot run it either, having no way to tell
	// whether it was given the argument. Such a parameter stays required.
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
	// Fewest arguments a caller must supply: every parameter up to the first
	// one it is allowed to leave out.
	size_t required_arguments = 0;
};

// -= The wire format =-
//
// The table crosses the sandbox boundary as one blob rather than as an Array of
// Dictionaries, because everything a guest hands out that is not inlined in a
// Variant becomes a scoped variant, and a Sandbox caps those (Sandbox::MAX_REFS
// is 100). A table built out of containers costs several of them per function
// and blows the cap on a script of any size; a blob costs one, whatever the
// script.
//
// Little-endian throughout, which is the only byte order either side runs on:
//
//   u32  function count
//   per function:
//     str  name                 (u32 byte count, then the bytes, no terminator)
//     i32  return type          (Variant::Type, or ANY_TYPE)
//     u32  required argument count
//     u32  parameter count
//     per parameter:
//       str  name
//       i32  type
//       u8   DefaultKind
//       the default, whose width the kind fixes: i64 for INT, f64 for FLOAT,
//       u8 for BOOL, str for STRING, nothing for the rest
//
// Types are written out rather than derived, so a float default of 3.0 comes
// back a float and not an integer -- which is exactly what a self-describing
// text format would get wrong.
std::vector<uint8_t> encode_function_signatures(const std::vector<FunctionSignature> &signatures);

// Parse what encode_function_signatures() produced. Returns false on a blob
// that runs out early or names a kind that does not exist, leaving the output
// empty: it arrives from a guest program, so it is not trusted to be well
// formed.
bool decode_function_signatures(const uint8_t *data, size_t size,
	std::vector<FunctionSignature> &out);

} // namespace gdscript
