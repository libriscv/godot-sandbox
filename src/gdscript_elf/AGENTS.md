# GDScriptELF - Agent Documentation

## Module Overview

GDScriptELF is a script language implementation for the Godot Sandbox module that compiles GDScript source code to RISC-V ELF binaries for sandboxed execution. Unlike traditional GDScript which uses a virtual machine (VM) for bytecode execution, GDScriptELF compiles functions to native ELF binaries that run in the RISC-V sandbox, providing improved performance and security isolation.

**Key Technologies:**

- GDScript compilation pipeline (tokenizer, parser, analyzer, compiler)
- RISC-V ELF binary compilation
- GDExtension API (godot-cpp)
- Sandbox execution via libriscv
- VM fallback for unsupported opcodes

**Architecture:**

- GDScript source → Tokenizer → Parser → Analyzer → Compiler → Bytecode
- Bytecode → C Code Generation → RISC-V Cross-Compiler → ELF Binary
- ELF Binary → Sandbox Execution (with VM fallback)

## Module Structure

```
src/gdscript_elf/
├── AGENTS.md                    # This file
├── TODO.md                      # Remaining work and known issues
├── compilation/                 # GDScript compilation pipeline
│   ├── gdscript_tokenizer.*     # Tokenizes GDScript source
│   ├── gdscript_parser.*        # Parses tokens into AST
│   ├── gdscript_analyzer.*       # Analyzes and validates AST
│   ├── gdscript_compiler.*       # Compiles AST to bytecode
│   ├── gdscript_function.*       # Function representation
│   ├── gdscript.*                # GDScript class (read-only reference)
│   └── ...
├── elf/                         # ELF compilation components
│   ├── gdscript_bytecode_c_codegen.*      # Bytecode to C code
│   ├── gdscript_bytecode_elf_compiler.*    # C code to ELF binary
│   ├── gdscript_c_compiler.*               # RISC-V cross-compiler interface
│   └── ...
├── language/                    # GDScriptELF language implementation
│   ├── gdscript_elf.*           # Script resource (extends ScriptExtension)
│   ├── gdscript_elf_language.*  # Script language (extends ScriptLanguageExtension)
│   ├── gdscript_elf_instance.*  # Script instance (extends ScriptInstanceExtension)
│   ├── gdscript_elf_function.*  # Function execution wrapper
│   ├── resource_loader_gdscript_elf.*     # Resource loader for .gde files
│   ├── resource_saver_gdscript_elf.*       # Resource saver for .gde files
│   └── register_gdscript_elf.*  # Module registration
└── execution/                   # Execution components (future)
```

## Core Components

### GDScriptELF (Script Resource)

**File**: `language/gdscript_elf.h`, `language/gdscript_elf.cpp`

- Extends `ScriptExtension` (GDExtension API)
- Manages GDScript source code and compilation
- Stores compiled ELF binaries per function
- Handles property access and method information
- Integrates with GDScript compilation pipeline

**Key Methods:**

- `_compile_to_elf()` - Compiles script to ELF binaries
- `instance_create()` - Creates script instances
- `_get()`, `_set()` - Property access
- `get_property_default_value()` - Evaluates default property values

### GDScriptELFLanguage (Script Language)

**File**: `language/gdscript_elf_language.h`, `language/gdscript_elf_language.cpp`

- Extends `ScriptLanguageExtension` (GDExtension API)
- Registers `.gde` file extension
- Provides script validation and editor integration
- Manages language-level state

**Status**: ✅ **Completed** - Fully adapted to `ScriptLanguageExtension` interface with `_`-prefixed methods

### GDScriptELFInstance (Script Instance)

**File**: `language/gdscript_elf_instance.h`, `language/gdscript_elf_instance.cpp`

- Extends `ScriptInstanceExtension` (GDExtension API)
- Manages runtime state of script instances
- Handles property access and method calls
- Manages Sandbox instance for ELF execution

**Key Methods:**

- `callp()` - Executes function calls (dispatches to ELF or VM)
- `set()`, `get()` - Property access
- `_initialize_sandbox()` - Sets up sandbox for ELF execution

### GDScriptELFFunction (Function Execution)

**File**: `language/gdscript_elf_function.h`, `language/gdscript_elf_function.cpp`

- Wraps individual function execution
- Stores ELF binary and VM bytecode fallback
- Dispatches to ELF execution or VM fallback

**Key Methods:**

- `execute_elf()` - Executes function via ELF binary in sandbox
- `execute_vm_fallback()` - Executes function via VM bytecode
- `call()` - Main entry point for function execution

### Compilation Pipeline

**Files**: `compilation/gdscript_*.{h,cpp}`

The compilation pipeline follows this flow:

```
GDScript Source (.gde)
    ↓
Tokenizer (gdscript_tokenizer.*)
    ↓
Parser (gdscript_parser.*) → AST
    ↓
Analyzer (gdscript_analyzer.*) → Validated AST
    ↓
Compiler (gdscript_compiler.*) → Bytecode
    ↓
ELF Compiler (elf/gdscript_bytecode_elf_compiler.*) → ELF Binary
```

**Components:**

- **Tokenizer**: Converts GDScript source to tokens
- **Parser**: Builds Abstract Syntax Tree (AST) from tokens
- **Analyzer**: Validates and analyzes AST, resolves types
- **Compiler**: Generates bytecode from analyzed AST

**Note**: These files have been updated to use GDExtension includes (`godot_cpp/*`), but some internal APIs may still need adaptation. See `TODO.md` for remaining compilation issues.

### ELF Compilation

**Files**: `elf/gdscript_bytecode_*.{h,cpp}`

- **Bytecode to C**: Converts GDScript bytecode to C code
- **C to ELF**: Compiles C code to RISC-V ELF binary using cross-compiler
- **Function Wrappers**: Generates function wrappers for sandbox execution

## Implementation Status

### ✅ Completed

1. **GDScriptELF Class** (`language/gdscript_elf.h/cpp`)

   - Extends `ScriptExtension` (GDExtension API)
   - Core structure with ELF binary storage
   - Integration with GDScript compilation pipeline
   - Property access methods (`_get`, `_set`, `_get_property_list`)
   - Method information retrieval
   - Constant evaluation for default property values
   - ELF compilation during script loading (`_compile_to_elf()`)
   - Stores compiled ELF binaries per function in `function_elf_binaries`

2. **GDScriptELFInstance** (`language/gdscript_elf_instance.h/cpp`)

   - Extends `ScriptInstanceExtension` (GDExtension API)
   - Manages script state and properties
   - Handles function calls (dispatches to ELF or VM)
   - Sandbox integration for ELF execution
   - Property access and method calls

3. **GDScriptELFFunction** (`language/gdscript_elf_function.h/cpp`)

   - Function execution wrapper
   - ELF execution via sandbox (`execute_elf()`)
   - VM fallback structure (`execute_vm_fallback()`)
   - Function metadata and calling conventions
   - Stores ELF binary and bytecode fallback

4. **GDScriptELFLanguage** (`language/gdscript_elf_language.h/cpp`)

   - Extends `ScriptLanguageExtension` (GDExtension API)
   - Registers as new language "GDScriptELF" with `.gde` extension
   - Uses existing GDScript compilation pipeline
   - ✅ **Completed**: Fully adapted to `ScriptLanguageExtension` interface

5. **Resource Loader/Saver** (`language/resource_loader_gdscript_elf.*`, `resource_saver_gdscript_elf.*`)

   - Extends `ResourceFormatLoader`/`ResourceFormatSaver` (GDExtension API)
   - Uses `_`-prefixed methods (`_load()`, `_get_recognized_extensions()`, etc.)
   - Loads `.gde` files
   - Saves GDScriptELF resources
   - Recognizes `.gde` extension
   - ✅ **Completed**: Fully adapted to GDExtension API

6. **Registration System** (`language/register_gdscript_elf.h/cpp`)

   - GDExtension initialization using `GDExtensionInitializationLevel`
   - Registers language with Engine
   - Registers resource loaders/savers
   - Integrated into `register_types.cpp`
   - ✅ **Completed**: Uses GDExtension initialization API

7. **Build Integration**
   - Added to `CMakeLists.txt` for GDExtension builds
   - All source files included in build
   - ✅ **Completed**: GDExtension-only build support

### 🚧 In Progress

1. **Compilation Errors** ⚠️ **Blocking**

   - Missing GDExtension API methods in `gdscript_function.h`:
     - `Variant::get_validated_object_with_check()`
     - `Object::get_class_name()`
     - `Object::get_script_instance()`
   - Missing Variant internal types (ValidatedOperatorEvaluator, ValidatedSetter, etc.)
   - Incomplete `Script` type in some contexts
   - **See**: `TODO.md` for detailed list and solutions

2. **VM Fallback Implementation**

   - Basic structure exists in `execute_vm_fallback()`
   - Needs full VM bytecode interpreter implementation
   - Should handle all GDScript opcodes not supported by ELF compilation

3. **CoreConstants and Math:: Usage**

   - Some compilation pipeline files still use `CoreConstants` internally:
     - `gdscript_analyzer.cpp`
     - `gdscript_editor.cpp`
     - `gdscript.cpp`
     - `gdscript_parser.cpp`
   - Some files still use `Math::`:
     - `gdscript_utility_functions.cpp`
     - `gdscript_parser.cpp`
   - **See**: `TODO.md` for tracking

4. **Base Class Inheritance**

   - Base class handling in compilation pipeline
   - Inheritance chain resolution
   - Method/property inheritance

### ⏳ Future Work

1. **Full Feature Support**

   - Yield/coroutines
   - Signals and connections
   - RPC/network features
   - All GDScript opcodes

2. **Performance Optimizations**

   - ELF binary caching
   - Binary translation support
   - JIT compilation improvements
   - Memory management optimizations

3. **Testing and Documentation**

   - Comprehensive unit tests
   - Integration tests
   - Performance benchmarks
   - Complete API documentation

4. **Editor Integration**
   - Syntax highlighting for `.gde` files
   - Code completion
   - Error reporting
   - Debugging support

## Development Guidelines

### Code Style

Follow the same conventions as the main project (see root `AGENTS.md`):

- **Classes**: PascalCase (e.g., `GDScriptELF`, `GDScriptELFInstance`)
- **Files**: snake_case matching class name
- **Methods**: snake_case (e.g., `_compile_to_elf()`, `execute_elf()`)
- **GDExtension**: Use `godot::` namespace, extend `ScriptExtension`/`ScriptLanguageExtension`

### GDExtension Builds

**Important**: GDScriptELF is now fully migrated to GDExtension builds:

- **GDExtension Build**: Uses godot-cpp includes (`<godot_cpp/classes/script_language_extension.hpp>`, etc.)
- All language interface files use GDExtension API
- Compilation pipeline files (`compilation/`) have been updated to use GDExtension includes
- Module build compatibility has been dropped in favor of GDExtension-only support

### Adding New Features

1. **New Function Opcodes**: Add to `gdscript_bytecode_c_codegen.cpp` to generate C code
2. **New Language Features**: Extend parser/analyzer in `compilation/` directory
3. **Performance Improvements**: Optimize ELF compilation or execution paths
4. **VM Fallback**: Extend `execute_vm_fallback()` in `gdscript_elf_function.cpp`

### Testing

**Unit Tests:**

```gdscript
extends GutTest

func test_gdscript_elf_compilation():
    var script = GDScriptELF.new()
    script.set_source_code("func test(): return 42")
    var err = script.reload()
    assert_eq(err, OK)
    assert_true(script.is_valid())
```

**ELF Execution Tests:**

- Test function execution via ELF binary
- Test VM fallback for unsupported opcodes
- Test property access and method calls
- Test sandbox integration

### Debugging

**Compilation Issues:**

- Check `_compile_to_elf()` for compilation errors
- Verify RISC-V cross-compiler is available
- Check ELF binary generation in `gdscript_bytecode_elf_compiler.cpp`

**Execution Issues:**

- Enable sandbox debug logging
- Check function address resolution in `execute_elf()`
- Verify argument marshaling
- Check VM fallback execution

**Common Issues:**

- **Missing ELF Binary**: Function may not be compilable, check `can_compile_function()`
- **Symbol Not Found**: Function name mismatch between C code and ELF symbol
- **VM Fallback Not Working**: `execute_vm_fallback()` needs full implementation

## Dependencies

### Internal Dependencies

- **GDScript Compilation Pipeline** (`compilation/`): Tokenizer, parser, analyzer, compiler
- **ELF Compilation** (`elf/`): Bytecode to C, C to ELF compilation
- **Sandbox** (`../../sandbox.h`): RISC-V emulator for ELF execution
- **GDExtension** (`ext/godot-cpp/`): GDExtension C++ bindings

### External Dependencies

- **RISC-V Cross-Compiler**: Required for ELF compilation
  - `riscv64-unknown-elf-gcc` (preferred)
  - `riscv64-linux-gnu-gcc` (alternative)
- **Godot Engine**: Version 4.3+ (for module builds)
- **CMake**: For building (GDExtension builds)

## Build Integration

### CMake (GDExtension)

GDScriptELF files are included in `CMakeLists.txt`:

```cmake
src/gdscript_elf/language/gdscript_elf.cpp
src/gdscript_elf/language/gdscript_elf_function.cpp
src/gdscript_elf/language/gdscript_elf_instance.cpp
src/gdscript_elf/language/gdscript_elf_language.cpp
src/gdscript_elf/language/register_gdscript_elf.cpp
src/gdscript_elf/language/resource_loader_gdscript_elf.cpp
src/gdscript_elf/language/resource_saver_gdscript_elf.cpp
```

### SCons (Module)

**Note**: Module build support has been dropped. GDScriptELF now only supports GDExtension builds via CMake.

## Known Issues

See `TODO.md` for a comprehensive list of remaining issues. Key items:

1. **Compilation Errors**: Missing GDExtension API methods and internal types in compilation pipeline (see `TODO.md` Critical Issues)
2. **VM Fallback**: Basic structure exists but needs full bytecode interpreter implementation
3. **CoreConstants/Math:: Usage**: Some compilation pipeline files still use these internally - may need wrapper functions or alternative implementations
4. **Editor-Only Features**: Some includes and features may not be fully compatible with GDExtension builds

**Note**: The language interface layer (`language/`) is fully migrated to GDExtension. Remaining issues are primarily in the compilation pipeline (`compilation/`) which needs further refactoring for full GDExtension compatibility.

## Compilation Flow

The complete compilation and execution flow:

```
GDScript Source (.gde file)
    ↓
ResourceLoaderGDScriptELF
    ↓
GDScriptELF::reload()
    ↓
GDScriptELF::_compile_to_elf()
    ├─→ GDScriptParser::parse() → AST
    ├─→ GDScriptAnalyzer::analyze() → Validated AST
    ├─→ GDScriptCompiler::compile() → Bytecode
    └─→ GDScriptBytecodeELFCompiler::compile_function_to_elf()
        ├─→ GDScriptBytecodeCCodeGenerator → C Code
        └─→ GDScriptCCompiler → ELF Binary
    ↓
Store ELF binaries in function_elf_binaries
    ↓
GDScriptELFInstance::callp()
    ↓
GDScriptELFFunction::call()
    ├─→ execute_elf() (if ELF available)
    │   ├─→ Sandbox::load_buffer()
    │   ├─→ Sandbox::address_of()
    │   └─→ Sandbox::vmcall_address()
    └─→ execute_vm_fallback() (if ELF unavailable)
```

## References

- **Main Project AGENTS.md**: `/AGENTS.md` - Overall project documentation
- **TODO.md**: `TODO.md` - Remaining work and known issues tracking
- **Godot GDScript Module**: Reference implementation for compilation pipeline
- **GDExtension Documentation**: https://docs.godotengine.org/en/stable/tutorials/scripting/gdextension/
- **RISC-V Specification**: https://riscv.org/technical/specifications/
- **agents.md Standard**: https://agents.md

## Contributing

When contributing to GDScriptELF:

1. Follow the main project's commit message format: `[gdscript_elf] Description`
2. Ensure GDExtension builds work (module builds are no longer supported)
3. Add tests for new functionality
4. Update this documentation and `TODO.md` for significant changes
5. Consider VM fallback for unsupported features
6. Test with and without RISC-V cross-compiler available
7. Check `TODO.md` for known issues before starting work
