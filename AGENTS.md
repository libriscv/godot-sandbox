# Godot Sandbox - Agent Documentation

## Project Overview

Godot Sandbox is a Godot engine module that provides safe, low-latency, and fast sandboxed execution for multiple programming languages. It enables Godot creators to implement safe modding support, allowing programs built by other players to be executed in a restricted environment that cannot harm other players or the host system.

**Key Technologies:**

- C++ (Godot engine module)
- RISC-V emulation (libriscv)
- Multiple script languages: C++, Rust, Zig, ELF, and GDScriptELF
- GDExtension API (godot-cpp)
- SCons build system
- CMake (for unit tests and program compilation)

**Architecture:**

- Sandboxed execution via RISC-V emulator
- Multiple script language implementations
- Resource loaders/savers for each language
- ELF binary compilation and execution
- Syscall-based API access with restrictions

**Main Components:**

- `Sandbox` - Core sandbox node for executing programs
- `ELFScript` / `ELFScriptLanguage` - ELF script resource and language
- `CPPScript` / `CPPScriptLanguage` - C++ script resource and language
- `RustScript` / `RustScriptLanguage` - Rust script resource and language
- `ZigScript` / `ZigScriptLanguage` - Zig script resource and language
- `GDScriptELF` / `GDScriptELFLanguage` - GDScript-to-ELF compilation language

## Development Environment Setup

### Prerequisites

- **Godot Engine**: Version 4.3+ (source code required for module build)
- **SCons**: Build system for Godot modules
- **Python 3**: Required for SCons
- **C++ Compiler**: C++17 support required
- **RISC-V Cross-Compiler**: For compiling programs to ELF
  - `riscv64-unknown-elf-gcc` (preferred)
  - `riscv64-linux-gnu-gcc` (alternative)
  - `riscv64-elf-gcc` (alternative)
- **CMake**: For building unit tests and programs
- **Ninja**: Optional, for faster CMake builds

### Module Structure

```
godot-sandbox/
├── AGENTS.md                    # This file
├── README.md                    # Project documentation
├── SCsub                        # SCons build configuration
├── SConstruct                   # SCons main file
├── config.py                    # Module configuration
├── build.sh                     # Build script (CMake/Ninja)
├── src/                         # Source files
│   ├── sandbox.*                # Core Sandbox class
│   ├── elf/                     # ELF script language
│   ├── cpp/                     # C++ script language
│   ├── rust/                    # Rust script language
│   ├── zig/                     # Zig script language
│   ├── gdscript_elf/            # GDScriptELF language
│   └── ...
├── ext/                         # External dependencies
│   └── godot-cpp/               # GDExtension C++ bindings
├── bin/                         # Built libraries
├── program/                     # Program compilation tools
├── tests/                       # Unit tests
└── doc_classes/                 # Documentation classes
```

### Initial Setup

1. **Clone the repository:**

   ```bash
   git clone https://github.com/libriscv/godot-sandbox.git
   cd godot-sandbox
   ```

2. **Initialize submodules:**

   ```bash
   git submodule update --init --recursive
   ```

3. **Add to Godot engine:**
   ```bash
   # In Godot engine repository
   git submodule add https://github.com/libriscv/godot-sandbox modules/sandbox
   cd modules/sandbox
   git submodule update --init --recursive
   ```

## Build and Test Commands

### Building the Module

**As a Godot Module (SCons):**

```bash
# Build Godot with sandbox module
# From Godot engine root directory
scons target=template_debug platform=linuxbsd

# Module is automatically included when building Godot
```

**As an Addon (CMake/Ninja):**

```bash
# Build the addon library
./build.sh

# Or manually:
mkdir -p .build
cd .build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja
```

**Platform-Specific Builds:**

```bash
# Android
./build_android.sh

# MinGW (Windows)
./build_mingw.sh
```

### Running Tests

**Unit Tests (GUT Framework):**

```bash
# Run unit tests
cd tests
./run_unittests.sh

# Requires:
# - Godot binary (4.4.1+)
# - RISC-V cross-compiler in PATH
# - CMake for building test ELF files
```

**Test Structure:**

- Unit tests use GUT (Godot Unit Test) framework
- Tests located in `tests/tests/` directory
- Test ELF files compiled via CMake
- Requires RISC-V cross-compiler for ELF compilation

### Code Formatting

**Format Code:**

```bash
# Format C++ code
./scripts/clang-format.sh

# Or manually:
clang-format -i src/**/*.{cpp,h,hpp}
```

**Linting:**

```bash
# Run clang-tidy
./scripts/clang-tidy.sh
```

## Code Style and Conventions

### Naming Conventions

- **Classes**: PascalCase (e.g., `Sandbox`, `ELFScript`)
- **Files**: snake_case matching class name (e.g., `sandbox.h`, `script_elf.cpp`)
- **Methods**: snake_case (e.g., `load_buffer()`, `vmcall_address()`)
- **Constants**: UPPER_SNAKE_CASE (e.g., `ECALL_OBJ_PROP_GET`)
- **Private members**: Leading underscore (e.g., `_internal_state`)

### Code Organization

- Header files (`.h`, `.hpp`) in `src/` directory
- Implementation files (`.cpp`) in `src/` directory
- Language-specific code in subdirectories (`elf/`, `cpp/`, `rust/`, `zig/`, `gdscript_elf/`)
- Test files in `tests/` directory
- Documentation in `doc_classes/` (XML format)

### Godot Conventions

- Use `ERR_FAIL_*` macros for error handling
- Use `memnew()` / `memdelete()` for memory management
- Follow Godot's coding style (see `.clang-format`)
- Use `GDCLASS()` macro for Godot classes
- Use `GDREGISTER_CLASS()` for class registration
- Use `Ref<T>` for reference-counted objects

### C++ Standards

- **C++ Standard**: C++17
- **Formatting**: LLVM style (see `.clang-format`)
- **Indentation**: Tabs (4 spaces equivalent)
- **Line Length**: No limit (ColumnLimit: 0)
- **Access Modifiers**: -4 offset

### GDExtension Conventions

- Use `godot::` namespace for GDExtension classes
- Extend `ScriptLanguageExtension` for script languages
- Extend `ScriptExtension` for script resources
- Use `_bind_methods()` for method binding
- Follow godot-cpp API patterns

## Testing Guidelines

### Test Framework

- **Framework**: GUT (Godot Unit Test) via `addons/gut/`
- **Structure**: GDScript test files in `tests/tests/`
- **ELF Tests**: C++ test programs compiled to ELF
- **Pattern**: Test files named `test_*.gd`

### Test Categories

- **Unit Tests**: Individual component testing
- **Integration Tests**: End-to-end functionality
- **ELF Execution Tests**: Sandbox execution verification
- **Language Tests**: Script language-specific tests
- **Performance Tests**: Execution speed benchmarks

### Writing Tests

**GDScript Tests:**

```gdscript
extends GutTest

func test_sandbox_creation():
    var sandbox = Sandbox.new()
    assert_not_null(sandbox)
    sandbox.free()
```

**ELF Test Programs:**

```cpp
// tests/tests/test_program.cpp
#include "api.hpp"

static Variant test_function() {
    return 42;
}

int main() {
    ADD_API_FUNCTION(test_function, "int");
}
```

### Test Requirements

- RISC-V cross-compiler must be available for ELF tests
- Tests should gracefully skip when compiler is not found
- Integration tests require sandbox module dependency
- Some tests may require creating minimal script instances

## Security Considerations

### Sandbox Isolation

- **RISC-V Emulation**: All code runs in isolated RISC-V emulator
- **Syscall Restrictions**: API access via restricted syscalls
- **Memory Isolation**: Sandbox memory separate from host
- **Resource Restrictions**: Configurable access to Godot resources
- **No Direct System Access**: Programs cannot access host filesystem directly

### Input Validation

- Validate all user inputs before execution
- Sanitize data before passing to sandbox
- Validate ELF binaries before loading
- Check function signatures before execution

### Error Handling

- All errors return safe defaults
- No exceptions thrown to host system
- Comprehensive error logging
- Graceful fallback mechanisms

### Best Practices

- Never execute untrusted code without sandbox
- Always validate ELF binaries
- Use restrictions API to limit resource access
- Monitor sandbox execution for anomalies
- Keep RISC-V emulator updated

## Pull Request Guidelines

### Commit Messages

Follow conventional commit format:

```
[component] Brief description

Detailed explanation if needed. Reference issue numbers
if applicable. Keep first line under 50 characters.

Fixes #12345
```

**Examples:**

```
[sandbox] Add memory limit configuration

Implements configurable memory limits for sandbox instances
to prevent resource exhaustion attacks.

Fixes #12345
```

```
[gdscript_elf] Integrate ELF compilation into compiler

Modifies GDScriptELF compiler to generate ELF binaries
during script compilation phase.

Related to #67890
```

### Code Review Checklist

- [ ] Follows Godot coding style (clang-format)
- [ ] No breaking changes to public API
- [ ] Tests added/updated for new functionality
- [ ] Documentation updated if needed
- [ ] Build system (`SCsub`) updated if adding new files
- [ ] Error handling implemented
- [ ] Memory management correct (`memnew`/`memdelete`)
- [ ] Security considerations addressed
- [ ] Cross-platform compatibility verified

### Pull Request Process

1. **Create Feature Branch**: `git checkout -b feature/description`
2. **Make Changes**: Follow coding conventions
3. **Run Tests**: Ensure all tests pass
4. **Format Code**: Run `clang-format`
5. **Update Documentation**: Update relevant docs
6. **Create PR**: Include description and checklist
7. **Address Review**: Respond to feedback
8. **Merge**: After approval and CI passes

## Development Workflow

### Adding a New Script Language

1. Create language directory in `src/` (e.g., `src/newlang/`)
2. Implement `ScriptLanguageExtension` subclass
3. Implement `ScriptExtension` subclass for resources
4. Create resource loaders/savers
5. Register in `register_types.cpp`
6. Add to `SCsub` build configuration
7. Write tests
8. Update documentation

### Adding New Syscalls

1. Define syscall number in `src/sandbox_syscalls.h`
2. Implement handler in appropriate `sandbox_syscalls_*.cpp`
3. Add to syscall table
4. Update API documentation
5. Add tests for syscall
6. Consider security implications

### Debugging

**Enable Debug Logging:**

```cpp
// In code
ERR_PRINT("Debug message: " + String(variable));
```

**Sandbox Debugging:**

- Use `Sandbox::set_debug_enabled(true)`
- Check sandbox execution logs
- Monitor syscall invocations
- Inspect memory state

**ELF Execution Debugging:**

- Check ELF binary loading
- Verify function address resolution
- Monitor argument marshaling
- Check return value extraction

## Architecture Details

### Script Language Implementation

Each script language follows this pattern:

1. **ScriptLanguage**: Extends `ScriptLanguageExtension`

   - Registers language with Godot
   - Handles script validation
   - Provides editor integration

2. **Script Resource**: Extends `ScriptExtension`

   - Represents script file
   - Handles compilation/loading
   - Manages script instances

3. **Script Instance**: Extends `ScriptInstanceExtension`

   - Manages script execution state
   - Handles property access
   - Executes functions

4. **Resource Loader/Saver**: Extends `ResourceFormatLoader`/`ResourceFormatSaver`
   - Loads script files
   - Saves script resources
   - Recognizes file extensions

### Execution Flow

```
Script Source File
    ↓
Resource Loader
    ↓
Script Resource (compiles to ELF)
    ↓
Sandbox Instance
    ↓
ELF Binary Loading
    ↓
Function Address Resolution
    ↓
Syscall Execution
    ↓
Return Value
```

### GDScriptELF Specific

GDScriptELF follows a different pattern:

1. **Compilation Pipeline**:

   - Tokenizer → Parser → Analyzer → Compiler
   - Bytecode Generation → ELF Compilation
   - Stores ELF binaries per function

2. **Execution**:

   - Function calls check for ELF binary
   - Execute in sandbox if available
   - Fallback to VM if ELF unavailable

3. **Integration**:
   - Uses existing GDScript compilation pipeline
   - Replaces bytecode generation with ELF compilation
   - Maintains GDScript syntax compatibility

## Dependencies

### External Dependencies

- **godot-cpp**: GDExtension C++ bindings (submodule)
- **libriscv**: RISC-V emulator (embedded or submodule)
- **RISC-V Cross-Compiler**: For compiling programs to ELF

### Internal Dependencies

- **modules/gdscript**: Read-only (for GDScriptELF compilation)
- **modules/sandbox**: Core sandbox functionality
- **Core Godot APIs**: ScriptLanguage, ResourceLoader, etc.

### Build Dependencies

- **SCons**: Godot build system
- **CMake**: Program compilation
- **Python 3**: Build scripts
- **C++17 Compiler**: Required

## Performance Considerations

### Optimization Strategies

- **Binary Translation**: Enable for 5-50x performance boost
- **JIT Compilation**: Experimental JIT builds available
- **Caching**: Cache compiled ELF binaries
- **Memory Management**: Efficient sandbox memory usage
- **Syscall Overhead**: Minimize syscall frequency

### Benchmarking

- Use performance profiling tools
- Measure execution time
- Compare VM vs ELF execution
- Monitor memory usage
- Track syscall frequency

## References

- **Project Website**: https://libriscv.no
- **GitHub Repository**: https://github.com/libriscv/godot-sandbox
- **Documentation**: https://libriscv.no/docs/godot_docs/
- **Demo Repository**: https://github.com/libriscv/godot-sandbox-demo
- **Godot Engine**: https://godotengine.org
- **GDExtension Docs**: https://docs.godotengine.org/en/stable/tutorials/scripting/gdextension/
- **RISC-V Specification**: https://riscv.org/technical/specifications/
- **agents.md Standard**: https://agents.md

## Implementation Status

### Completed Components

- ✅ Core Sandbox class and execution
- ✅ ELF script language
- ✅ C++ script language
- ✅ Rust script language (editor only)
- ✅ Zig script language (editor only)
- ✅ Resource loaders/savers
- ✅ Syscall API implementation
- ✅ Binary translation support
- ✅ GDScriptELF language structure

### In Progress

- 🚧 GDScriptELF full implementation
- 🚧 Comprehensive test coverage
- 🚧 Performance optimizations
- 🚧 Documentation completion

### Future Work

- ⏳ JIT compilation improvements
- ⏳ Additional script languages
- ⏳ Enhanced security features
- ⏳ Performance profiling tools
- ⏳ Editor integration improvements
