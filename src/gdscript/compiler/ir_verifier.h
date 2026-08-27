#pragma once
#include "ir.h"
#include <string>

namespace gdscript {

// Checks IR well-formedness between optimizer passes, derived from ir_opcodes.def.
// Verifies: signature arity/kinds, register definedness, label uniqueness,
// max_registers coverage, type-hint consistency, CALL DST/arg disjointness.
// Throws CompilerException naming function, instruction index and pass.
// Without a string table, names in diagnostics render as their interned id.
void ir_verify(const IRFunction& func, const char* after_pass = nullptr,
	const IRStringTable* strings = nullptr);

void ir_verify(const IRProgram& program, const char* after_pass = nullptr);

// On in debug, off in release. Tests can override.
bool ir_verification_enabled();
void set_ir_verification_enabled(bool enabled);

} // namespace gdscript
