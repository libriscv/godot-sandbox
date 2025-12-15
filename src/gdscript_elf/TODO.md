# GDScriptELF TODO

This document tracks remaining work and known issues for the GDScriptELF module after the GDExtension migration.

## Critical Issues

### Compilation Errors

#### Missing GDExtension API Methods

**File**: `src/gdscript_elf/compilation/gdscript_function.h`

**Issues**:
- `Variant::get_validated_object_with_check()` - Not available in GDExtension
- `Object::get_class_name()` - Not available in GDExtension  
- `Object::get_script_instance()` - Not available in GDExtension

**Solution**: Find GDExtension equivalents or implement workarounds using POD structures and Variant types.

**Affected Code**:
```cpp
// Current (module API):
Object *obj = variant.get_validated_object_with_check();
String class_name = obj->get_class_name();
ScriptInstance *inst = obj->get_script_instance();

// Needed (GDExtension):
// Use Variant::get_type() and type checking
// Use Object::get_class() or alternative
// Use ScriptInstanceExtension or alternative approach
```

#### Missing Variant Internal Types

**File**: `src/gdscript_elf/compilation/gdscript_function.h`

**Issues**: These internal types are not exposed in GDExtension:
- `Variant::ValidatedOperatorEvaluator`
- `Variant::ValidatedSetter`
- `Variant::ValidatedGetter`
- `Variant::ValidatedKeyedSetter`
- `Variant::ValidatedKeyedGetter`
- `Variant::ValidatedIndexedSetter`
- `Variant::ValidatedIndexedGetter`
- `Variant::ValidatedBuiltInMethod`
- `Variant::ValidatedConstructor`
- `Variant::ValidatedUtilityFunction`

**Solution**: Refactor to use public GDExtension API with POD structures:
- Use `Dictionary` for function metadata
- Use `Variant::call()` with proper error handling
- Create wrapper POD structs if needed:
  ```cpp
  struct FunctionCallInfo {
      StringName method_name;
      Array arguments;
      Variant return_value;
      GDExtensionCallError error;
  };
  ```

#### Incomplete Script Type

**File**: `src/gdscript_elf/compilation/gdscript_function.h`

**Issue**: `godot::Script` type is incomplete in some contexts.

**Solution**: Add forward declarations or include `<godot_cpp/classes/script.hpp>` explicitly.

## High Priority

### VM Fallback Implementation

**File**: `src/gdscript_elf/language/gdscript_elf_function.cpp`

**Tasks**:
- [ ] Implement full VM bytecode interpreter in `GDScriptELFFunction::execute_vm_fallback()`
- [ ] Handle all GDScript opcodes not supported by ELF compilation
- [ ] Integrate with GDScript VM execution path
- [ ] Test fallback execution for various opcodes

**Data Structures**: Use POD structures for VM state:
```cpp
struct VMState {
    Array stack;
    Dictionary locals;
    int32_t pc;  // Program counter
    Variant return_value;
};
```

### CoreConstants and Math:: Usage

**Files**: 
- `src/gdscript_elf/compilation/gdscript_analyzer.cpp`
- `src/gdscript_elf/compilation/gdscript_editor.cpp`
- `src/gdscript_elf/compilation/gdscript.cpp`
- `src/gdscript_elf/compilation/gdscript_parser.cpp`
- `src/gdscript_elf/compilation/gdscript_utility_functions.cpp`

**Tasks**:
- [ ] Replace `CoreConstants::get_global_constant_*()` with `ProjectSettings` or direct constants
- [ ] Replace `Math::PI`, `Math::TAU`, etc. with direct values or `Variant` constants
- [ ] Create wrapper functions using POD structures:
  ```cpp
  struct ConstantInfo {
      StringName name;
      Variant value;
      StringName enum_name;
  };
  
  Dictionary get_global_constants() {
      Dictionary result;
      // Populate from ProjectSettings or direct values
      return result;
  }
  ```

### Base Class Inheritance

**Files**: `src/gdscript_elf/compilation/gdscript_*.{h,cpp}`

**Tasks**:
- [ ] Complete base class handling in compilation pipeline
- [ ] Implement inheritance chain resolution using POD structures
- [ ] Handle method/property inheritance correctly
- [ ] Test inheritance scenarios

**Data Structures**: Use Variant/Dictionary for inheritance info:
```cpp
struct InheritanceInfo {
    StringName class_name;
    StringName base_class;
    Array inheritance_chain;  // Array of StringName
    Dictionary methods;
    Dictionary properties;
};
```

## Medium Priority

### Testing

**Tasks**:
- [ ] Create comprehensive unit tests for GDScriptELF compilation
- [ ] Test ELF execution for various function types
- [ ] Test VM fallback execution
- [ ] Integration tests for script loading and execution
- [ ] Performance benchmarks comparing ELF vs VM execution

**Test Data Structures**: Use Variant types for test cases:
```cpp
struct TestCase {
    String source_code;
    String function_name;
    Array arguments;
    Variant expected_result;
    bool use_elf;  // true for ELF, false for VM fallback
};
```

### Editor Integration

**Tasks**:
- [ ] Syntax highlighting for `.gde` files
- [ ] Code completion implementation
- [ ] Error reporting and diagnostics
- [ ] Debugging support
- [ ] Script templates

**Data Structures**: Use Dictionary for editor metadata:
```cpp
Dictionary get_editor_info() {
    Dictionary info;
    info["syntax_highlighting"] = true;
    info["code_completion"] = true;
    info["error_reporting"] = true;
    return info;
}
```

### Documentation

**Tasks**:
- [ ] Complete API documentation
- [ ] Usage examples
- [ ] Migration guide from GDScript to GDScriptELF
- [ ] Performance optimization guide

## Low Priority / Future Work

### Feature Completeness

**Tasks**:
- [ ] Yield/coroutines support
- [ ] Signals and connections
- [ ] RPC/network features
- [ ] All GDScript opcodes support
- [ ] Static typing support

**Data Structures**: Use Variant for feature metadata:
```cpp
Dictionary get_feature_support() {
    Dictionary features;
    features["yield"] = false;
    features["signals"] = false;
    features["rpc"] = false;
    return features;
}
```

### Performance Optimizations

**Tasks**:
- [ ] ELF binary caching
- [ ] Binary translation support improvements
- [ ] JIT compilation enhancements
- [ ] Memory management optimizations
- [ ] Parallel compilation support

**Data Structures**: Use POD structures for cache entries:
```cpp
struct ELFCacheEntry {
    String source_hash;
    PackedByteArray elf_binary;
    int64_t timestamp;
    Dictionary metadata;  // Function names, sizes, etc.
};
```

### Code Quality

**Tasks**:
- [ ] Refactor compilation pipeline for better GDExtension compatibility
- [ ] Remove or conditionally compile editor-only code
- [ ] Improve error handling and reporting using Variant/Dictionary
- [ ] Add more comprehensive logging

**Error Reporting Structure**:
```cpp
struct CompilationError {
    int32_t line;
    int32_t column;
    String message;
    String error_type;
};

Dictionary get_compilation_errors() {
    Dictionary result;
    Array errors;  // Array of CompilationError dictionaries
    result["errors"] = errors;
    result["warnings"] = Array();
    return result;
}
```

## Implementation Notes

### POD Structure Guidelines

When creating data structures for GDExtension compatibility:

1. **Prefer Variant-compatible types**:
   - Use `Dictionary` for key-value mappings
   - Use `Array` or `TypedArray<T>` for lists
   - Use `PackedStringArray` for string lists
   - Use `PackedByteArray` for binary data

2. **Simple POD structs** when needed:
   ```cpp
   struct SimplePOD {
       int32_t value;
       float fvalue;
       bool flag;
   };
   ```

3. **Avoid complex inheritance** - Use composition with Variant types

4. **Use StringName for identifiers** - More efficient than String for lookups

### Variant Usage Patterns

```cpp
// Function metadata
Dictionary function_info;
function_info["name"] = StringName("my_function");
function_info["return_type"] = Variant::Type::INT;
function_info["arguments"] = Array();  // Array of PropertyInfo dictionaries

// Error reporting
Dictionary error_info;
error_info["line"] = 42;
error_info["column"] = 10;
error_info["message"] = "Syntax error";
error_info["type"] = "syntax_error";

// Configuration
Dictionary config;
config["use_elf"] = true;
config["cache_enabled"] = true;
config["max_cache_size"] = 1000;
```

## Related Files

- `src/gdscript_elf/AGENTS.md` - Module documentation
- `src/gdscript_elf/compilation/` - Compilation pipeline (needs further work)
- `src/gdscript_elf/language/` - Language interface (mostly complete)
- `src/gdscript_elf/elf/` - ELF compilation components

## Status Summary

**Completed**: Language interface layer fully migrated to GDExtension

**In Progress**: Compilation pipeline GDExtension compatibility

**Blocking**: Missing GDExtension API methods and internal types

**Next Steps**: 
1. Fix compilation errors using POD structures and Variant types
2. Implement VM fallback
3. Complete CoreConstants/Math:: replacements
