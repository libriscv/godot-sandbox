# GDScriptELF Implementation Status

## Overview

This document tracks the implementation of GDScriptELF as described in `GDSCRIPT_EXTENSION_ANALYSIS.md`. GDScriptELF is a new script language that compiles GDScript source code to ELF binaries instead of VM bytecode, enabling sandboxed execution.

## Implementation Status

### ✅ Completed

1. **GDScriptELFLanguage Class** (`language/gdscript_elf_language.h/cpp`)
   - Extends `ScriptLanguage` interface
   - Registers as new language "GDScriptELF" with `.gde` extension
   - Implements core ScriptLanguage methods
   - Uses existing GDScript compilation pipeline (tokenizer, parser, analyzer)

2. **GDScriptELF Class** (`language/gdscript_elf.h`)
   - Extends `Script` interface
   - Header file with structure for ELF compilation
   - Stores compiled ELF binaries per function
   - Integrates with existing GDScript compilation infrastructure

3. **Resource Loader/Saver** (`language/resource_loader_gdscript_elf.h/cpp`, `resource_saver_gdscript_elf.h/cpp`)
   - Loads `.gde` files
   - Saves GDScriptELF resources
   - Recognizes `.gde` extension

4. **Registration System** (`language/register_gdscript_elf.h/cpp`)
   - Module initialization code
   - GDExtension initialization function
   - Registers language with ScriptServer
   - Registers resource loaders/savers

### 🚧 In Progress / TODO

1. **GDScriptELF Implementation** (`language/gdscript_elf.cpp`)
   - Full implementation of Script interface methods
   - Integration with compilation pipeline
   - ELF compilation during script loading
   - Instance creation

2. **GDScriptELFFunction** (`language/gdscript_elf_function.h/cpp`)
   - Function execution using ELF binaries
   - Sandbox integration for execution
   - Fallback to VM if ELF compilation fails
   - Function metadata and calling conventions

3. **GDScriptELFInstance** (Not yet created)
   - ScriptInstance implementation
   - Manages script state and properties
   - Handles function calls
   - Integrates with sandbox execution

4. **ELF Compilation Integration**
   - Modify compiler to generate ELF instead of VM bytecode
   - Integrate `GDScriptBytecodeELFCompiler` into compilation pipeline
   - Store ELF binaries in GDScriptELF
   - Handle compilation errors and fallbacks

5. **Sandbox Integration**
   - Create sandbox instances for script execution
   - Map function calls to ELF execution
   - Handle syscalls and API calls
   - Memory management

## Architecture

### File Structure

```
src/gdscript_elf/
├── language/                    # New GDScriptELF language implementation
│   ├── gdscript_elf_language.h/cpp    # ScriptLanguage implementation
│   ├── gdscript_elf.h                 # Script implementation (header)
│   ├── gdscript_elf.cpp               # Script implementation (TODO)
│   ├── gdscript_elf_function.h        # Function execution (header)
│   ├── gdscript_elf_function.cpp      # Function execution (TODO)
│   ├── resource_loader_gdscript_elf.h/cpp
│   ├── resource_saver_gdscript_elf.h/cpp
│   └── register_gdscript_elf.h/cpp
├── compilation/                 # Existing GDScript compilation pipeline
│   ├── gdscript_tokenizer.*
│   ├── gdscript_parser.*
│   ├── gdscript_analyzer.*
│   ├── gdscript_compiler.*
│   └── ...
└── elf/                        # ELF compilation infrastructure
    ├── gdscript_bytecode_elf_compiler.*
    ├── gdscript_bytecode_c_codegen.*
    └── gdscript_c_compiler.*
```

### Compilation Flow

```
GDScript Source (.gde)
    ↓
Tokenizer (existing)
    ↓
Parser (existing)
    ↓
Analyzer (existing)
    ↓
Compiler (existing, modified)
    ↓
ELF Bytecode Generator → ELF Binary
    ↓
GDScriptELFFunction (sandbox execution)
```

## Integration Points

### 1. Module Registration

To integrate GDScriptELF into the module system, add to `src/register_types.cpp`:

```cpp
#include "gdscript_elf/language/register_gdscript_elf.h"

// In initialize_riscv_module:
initialize_gdscript_elf_language(MODULE_INITIALIZATION_LEVEL_SERVERS);
```

### 2. Compilation Pipeline

The compilation pipeline needs to be modified to:
1. After bytecode generation, compile to ELF using `GDScriptBytecodeELFCompiler`
2. Store ELF binaries in `GDScriptELF::function_elf_binaries`
3. Create `GDScriptELFFunction` instances with ELF binaries

### 3. Execution

Function execution should:
1. Check if ELF binary exists for function
2. If yes, execute in sandbox using ELF
3. If no, fallback to VM execution (or fail if required)

## Next Steps

1. **Complete GDScriptELF Implementation**
   - Implement all Script interface methods
   - Integrate with compilation pipeline
   - Handle script loading and reloading

2. **Complete GDScriptELFFunction**
   - Implement ELF execution
   - Integrate with sandbox
   - Handle function calls and returns

3. **Create GDScriptELFInstance**
   - Implement ScriptInstance interface
   - Manage script state
   - Handle property access

4. **Integrate ELF Compilation**
   - Modify compiler to call ELF compiler
   - Store results in GDScriptELF
   - Handle compilation errors

5. **Testing**
   - Create test scripts
   - Verify compilation
   - Verify execution
   - Test fallback scenarios

## Notes

- The implementation follows the analysis document's recommendation for a full copy approach
- Uses existing GDScript compilation pipeline (tokenizer, parser, analyzer)
- Only bytecode generation is replaced with ELF compilation
- Maintains compatibility with GDScript syntax
- Uses `.gde` extension to distinguish from standard `.gd` files

## References

- `GDSCRIPT_EXTENSION_ANALYSIS.md` - Original analysis document
- Existing ELF compilation infrastructure in `elf/` directory
- Existing GDScript compilation pipeline in `compilation/` directory
