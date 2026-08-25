#pragma once
#include "api.hpp"
#include <compiler.h>
#include <function_signature.h>
#include <line_table.h>
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
