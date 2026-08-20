#pragma once
#include "ir.h"
#include <string>

namespace gdscript {

// The IR verifier.
//
// The optimizer runs nine passes over each function, and until this existed
// nothing checked the IR between them: a pass that corrupted the IR was only
// noticed when the generated code misbehaved, and only if a test happened to
// exercise it. Both of the optimizer bugs in TASKS.md were exactly that.
//
// Everything here is derived from the opcode metadata table in ir_opcodes.def,
// so an opcode's operand roles are checked against the same declaration the
// passes read.
//
// What is checked:
//
//   - Arity and operand kinds match the opcode's signature.
//   - Every register read is defined on every path that reaches the read.
//   - Labels are defined exactly once, and every branch target exists.
//   - max_registers covers every register mentioned.
//   - Type hints are consistent with the opcode: a CONVERT declares what it
//     converts to, and a native-path hint on an arithmetic or comparison
//     opcode agrees with the types its operands are known to hold.
//   - The register a CALL defines is not also one of its arguments.
//
// Throws CompilerException describing the first problem found, naming the
// function, the instruction index and the pass that was running.

// Verify one function. `after_pass` names the pass that just ran, for the
// diagnostic; nullptr when the IR did not come out of a pass.
void ir_verify(const IRFunction& func, const char* after_pass = nullptr);

// Verify every function in a program, including the synthetic global
// initializer.
void ir_verify(const IRProgram& program, const char* after_pass = nullptr);

// Verification is on by default in debug builds and off in release builds,
// where it would run on every user compile. A test can turn it on regardless.
bool ir_verification_enabled();
void set_ir_verification_enabled(bool enabled);

} // namespace gdscript
