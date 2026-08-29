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
	// Compiler-only type name for Dictionary-backed structs (or Object classes).
	std::string class_name;

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
	std::string return_class_name;
	size_t required_arguments = 0;

	int32_t line = 0; // 1-based line of 'func' token; 0 when unknown
	std::string description; // '##' doc comment, lines joined by '\n'

	// Coroutine; MethodInfo return type forced to Variant.
	bool is_coroutine = false;
	bool is_static = false;
};

// One method exposed through Godot's high-level multiplayer API. The numeric
// values deliberately match MultiplayerAPI::RPCMode and
// MultiplayerPeer::TransferMode without making the standalone compiler depend
// on Godot headers.
struct RPCConfig {
	std::string name;
	int32_t rpc_mode = 2; // RPC_MODE_AUTHORITY
	int32_t transfer_mode = 2; // TRANSFER_MODE_RELIABLE
	bool call_local = false;
	int32_t channel = 0;
};

// One nested class with an engine base. The host makes a Script resource out of
// it and attaches an instance of that to the object the guest built.
struct ClassField {
	std::string name;
	int32_t type = FunctionParameter::ANY_TYPE;
	std::string class_name;
	std::string description;
};

// A lifted method of the class. Its parameters live in the FunctionSignature
// table under '@Class.name'; a static one has no synthetic self slot.
struct ClassMethod {
	std::string name;
	bool is_static = false;
};

struct ClassSignature {
	std::string name;
	// Declared parent class; empty when the class extends the engine directly.
	std::string base_name;
	// Engine class the declared chain bottoms out in.
	std::string native_base;
	// Inherited fields included, base's first, in declaration order.
	std::vector<ClassField> fields;
	// Declared here only; inherited ones are reached through base_name.
	std::vector<ClassMethod> methods;
	int32_t line = 0;
	bool is_struct = false;
	std::string description;
};

// Encoded as a single blob (one scoped variant) rather than Array of Dictionaries
// to stay under Sandbox::MAX_REFS. Little-endian; layout documented in
// function_signature.cpp.
std::vector<uint8_t> encode_function_signatures(const std::vector<FunctionSignature> &signatures);

// Returns false on truncated or invalid blobs; output left empty.
bool decode_function_signatures(const uint8_t *data, size_t size,
	std::vector<FunctionSignature> &out);

// A file-scope `const` or `enum`. Both are compiler-only -- they fold at their
// use sites and nothing reaches IR -- so an instance holds no storage the host
// can read. GDScript keeps its own in Script::constants, which is what makes
// `Autoload.SOME_ENUM.MEMBER` answer from another script; published here so a
// .sgd autoload answers the same way instead of null.
struct ScriptConstant {
	enum class Kind : uint8_t {
		INT,
		FLOAT,
		BOOL,
		STRING,
		ENUM, // members below; the host builds the Dictionary GDScript exposes
	};

	struct EnumMember {
		std::string name;
		int64_t value = 0;
	};

	std::string name;
	Kind kind = Kind::INT;
	std::variant<int64_t, double, std::string, bool> value;
	// Kind::ENUM only, in declaration order: the Dictionary is ordered, and
	// gen_enum_dictionary builds the guest's copy in that same order.
	std::vector<EnumMember> members;
};

// Separate blob, separate entry point: a section appended to the one above would
// fail to decode against every ELF built before it existed.
std::vector<uint8_t> encode_class_signatures(const std::vector<ClassSignature> &classes);
bool decode_class_signatures(const uint8_t *data, size_t size,
	std::vector<ClassSignature> &out);

// Own blob and own entry point, for the same reason as the class table above.
std::vector<uint8_t> encode_script_constants(const std::vector<ScriptConstant> &constants);
bool decode_script_constants(const uint8_t *data, size_t size,
	std::vector<ScriptConstant> &out);

// Own blob and compiler entry point, so older compiler ELFs simply publish no
// RPCs instead of making an existing metadata format undecodable.
std::vector<uint8_t> encode_rpc_configs(const std::vector<RPCConfig> &configs);
bool decode_rpc_configs(const uint8_t *data, size_t size,
	std::vector<RPCConfig> &out);

} // namespace gdscript
