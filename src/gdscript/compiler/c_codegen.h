#pragma once
#include "ir.h"
#include <string>

namespace gdscript {
// Emits C99 using c_abi.h with local copy propagation and scalar promotion.
// Does not use sandbox ownership passes or a machine-specific register allocator.
// gj_entry function indices follow IRProgram::functions; -1 initializes globals.
// Throws a diagnostic for features requiring ScriptLanguage integration.
class CCodeGenerator {
public:
    std::string generate(const IRProgram &program, bool debug_info = false);
};
}
