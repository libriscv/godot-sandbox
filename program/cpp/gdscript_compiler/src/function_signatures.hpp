#pragma once
#include "api.hpp"
#include <compiler.h>
#include <function_signature.h>
#include <line_table.h>
#include <string>
#include <utility>
#include <vector>

// Signatures and line table from the last compile, cached for the .sgd host.
//
// A call from Godot lands on the exported guest function directly, and the
// Sandbox ABI gives that function one Variant pointer per argument and no
// count: an argument the caller left out is a null pointer, and reading a
// parameter out of it faults. Godot can only refuse such a call if it knows the
// arity, and the produced ELF does not say -- its symbol table carries names
// alone -- so the compiler hands the table over beside the ELF it just built.
//
// It travels as one PackedByteArray. Everything a guest hands out that is not
// inlined in a Variant costs a scoped variant, and a Sandbox caps those, so an
// Array of Dictionaries would run a script of any size into the cap. The format
// is in function_signature.h, which both sides of the boundary share.

// The plain C++ record, not a Variant, is what is kept between calls: anything
// built out of the sandbox API is scoped to the call that made it.
inline std::vector<gdscript::FunctionSignature> &gdscript_last_signatures() {
	static std::vector<gdscript::FunctionSignature> signatures;
	return signatures;
}

inline void gdscript_remember_signatures(const gdscript::Compiler &compiler) {
	gdscript_last_signatures() = compiler.get_function_signatures();
}

inline Variant gdscript_signatures_to_variant() {
	return PackedByteArray(gdscript::encode_function_signatures(gdscript_last_signatures()));
}

// Declared signals. Encoded as FunctionSignatures in a separate blob.
inline std::vector<gdscript::FunctionSignature> &gdscript_last_signals() {
	static std::vector<gdscript::FunctionSignature> signals;
	return signals;
}

inline void gdscript_remember_signals(const gdscript::Compiler &compiler) {
	gdscript_last_signals() = compiler.get_signal_signatures();
}

inline Variant gdscript_signals_to_variant() {
	return PackedByteArray(gdscript::encode_function_signatures(gdscript_last_signals()));
}

// @rpc methods. Kept in an independent blob so old hosts and compiler ELFs
// remain compatible in both directions.
inline std::vector<gdscript::RPCConfig> &gdscript_last_rpc_configs() {
	static std::vector<gdscript::RPCConfig> configs;
	return configs;
}

inline void gdscript_remember_rpc_configs(const gdscript::Compiler &compiler) {
	gdscript_last_rpc_configs() = compiler.get_rpc_configs();
}

inline Variant gdscript_rpc_configs_to_variant() {
	return PackedByteArray(gdscript::encode_rpc_configs(gdscript_last_rpc_configs()));
}

// Nested classes with an engine base. Own blob, own entry point: appending a
// section to the signature blob above would fail to decode for every host built
// against an older format.
inline std::vector<gdscript::ClassSignature> &gdscript_last_classes() {
	static std::vector<gdscript::ClassSignature> classes;
	return classes;
}

inline void gdscript_remember_classes(const gdscript::Compiler &compiler) {
	gdscript_last_classes() = compiler.get_class_signatures();
}

inline Variant gdscript_classes_to_variant() {
	return PackedByteArray(gdscript::encode_class_signatures(gdscript_last_classes()));
}

// File-scope `const` and `enum`. Own blob, own entry point, same reason as above.
inline std::vector<gdscript::ScriptConstant> &gdscript_last_constants() {
	static std::vector<gdscript::ScriptConstant> constants;
	return constants;
}

inline void gdscript_remember_constants(const gdscript::Compiler &compiler) {
	gdscript_last_constants() = compiler.get_script_constants();
}

inline Variant gdscript_constants_to_variant() {
	return PackedByteArray(gdscript::encode_script_constants(gdscript_last_constants()));
}

// Address-to-line table. Metadata only; every compile produces one.
inline gdscript::LineTable &gdscript_last_line_table() {
	static gdscript::LineTable table;
	return table;
}

inline void gdscript_remember_line_table(const gdscript::Compiler &compiler) {
	gdscript_last_line_table() = compiler.get_line_table();
}

inline Variant gdscript_line_table_to_variant() {
	return PackedByteArray(gdscript::encode_line_table(gdscript_last_line_table()));
}

inline bool &gdscript_last_is_tool() {
	static bool is_tool = false;
	return is_tool;
}

inline void gdscript_remember_is_tool(const gdscript::Compiler &compiler) {
	gdscript_last_is_tool() = compiler.is_tool();
}

// Subset of requested breakpoint lines that got a stop emitted.
// Reset by every compile; a no-breakpoint build reports none.
inline std::vector<uint32_t> &gdscript_last_breakpoints() {
	static std::vector<uint32_t> lines;
	return lines;
}

inline void gdscript_remember_breakpoints(const gdscript::Compiler &compiler) {
	gdscript_last_breakpoints() = compiler.get_installed_breakpoints();
}

inline Variant gdscript_breakpoints_to_variant() {
	const std::vector<uint32_t> &lines = gdscript_last_breakpoints();
	std::vector<int32_t> out;
	out.reserve(lines.size());
	for (uint32_t line : lines) {
		out.push_back(int32_t(line));
	}
	return PackedInt32Array(out);
}

inline std::string &gdscript_last_class_name() {
	static std::string class_name;
	return class_name;
}

inline std::string &gdscript_last_base_class() {
	static std::string base_class;
	return base_class;
}

inline bool &gdscript_last_base_is_path() {
	static bool is_path = false;
	return is_path;
}

inline std::string &gdscript_last_native_base_class() {
	static std::string native_base;
	return native_base;
}

inline bool &gdscript_last_native_base_is_path() {
	static bool is_path = false;
	return is_path;
}

inline void gdscript_remember_script_class(const gdscript::Compiler &compiler) {
	gdscript_last_class_name() = compiler.get_class_name();
	gdscript_last_base_class() = compiler.get_base_class();
	gdscript_last_base_is_path() = compiler.base_is_path();
	gdscript_last_native_base_class() = compiler.get_native_base_class();
	gdscript_last_native_base_is_path() = compiler.native_base_is_path();
}

inline bool &gdscript_restricted() {
	static bool restricted = false;
	return restricted;
}

inline std::string &gdscript_source_path() {
	static std::string path;
	return path;
}

inline std::vector<std::string> &gdscript_autoloads() {
	static std::vector<std::string> autoloads;
	return autoloads;
}

inline std::vector<std::pair<std::string, std::string>> &gdscript_global_classes() {
	static std::vector<std::pair<std::string, std::string>> classes;
	return classes;
}

inline std::vector<gdscript::CompilerOptions::BaseSource> &gdscript_base_sources() {
	static std::vector<gdscript::CompilerOptions::BaseSource> sources;
	return sources;
}

inline void gdscript_set_base_sources(const std::vector<std::string> &triples) {
	std::vector<gdscript::CompilerOptions::BaseSource> sources;
	for (size_t i = 0; i + 2 < triples.size(); i += 3) {
		sources.push_back(gdscript::CompilerOptions::BaseSource{
				triples[i], triples[i + 1], triples[i + 2] });
	}
	gdscript_base_sources() = std::move(sources);
}

inline void gdscript_apply_restrictions(gdscript::CompilerOptions &options) {
	options.restricted = gdscript_restricted();
	options.source_path = gdscript_source_path();
	options.autoloads = gdscript_autoloads();
	options.global_script_classes = gdscript_global_classes();
	options.base_sources = gdscript_base_sources();
}
