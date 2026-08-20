#pragma once
#include "api.hpp"
#include <compiler.h>
#include <function_signature.h>
#include <vector>

// The signatures of the last compile, on their way to the .sgd script language.
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
