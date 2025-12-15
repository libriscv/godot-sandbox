# GDScriptELF TODO

This document tracks remaining work and known issues for the GDScriptELF module after the GDExtension migration.

## Critical Issues

### Compilation Errors

1. **Missing GDExtension API Methods in gdscript_function.h**

   - `Variant::get_validated_object_with_check()` - Not available in GDExtension
   - `Object::get_class_name()` - Not available in GDExtension
   - `Object::get_script_instance()` - Not available in GDExtension
   - **Solution**: Need to find GDExtension equivalents or implement workarounds

2. **Missing Variant Internal Types**

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
   - **Solution**: These are internal types that may not be exposed in GDExtension. May need to refactor to use public API or create wrapper implementations.

3. **Incomplete Script Type**
   - `godot::Script` type is incomplete in some contexts
   - **Solution**: May need forward declarations or additional includes

## High Priority

### VM Fallback Implementation

- [ ] Implement full VM bytecode interpreter in `GDScriptELFFunction::execute_vm_fallback()`
- [ ] Handle all GDScript opcodes not supported by ELF compilation
- [ ] Integrate with GDScript VM execution path
- [ ] Test fallback execution for various opcodes

### CoreConstants and Math:: Usage

- [ ] Replace remaining `CoreConstants` usage in compilation pipeline files:
  - `gdscript_analyzer.cpp`
  - `gdscript_editor.cpp`
  - `gdscript.cpp`
  - `gdscript_parser.cpp`
- [ ] Replace remaining `Math::` usage:
  - `gdscript_utility_functions.cpp`
  - `gdscript_parser.cpp`
- [ ] Create wrapper functions or use ProjectSettings/Engine equivalents

### Base Class Inheritance

- [ ] Complete base class handling in compilation pipeline
- [ ] Implement inheritance chain resolution
- [ ] Handle method/property inheritance correctly
- [ ] Test inheritance scenarios

## Medium Priority

### Testing

- [ ] Create comprehensive unit tests for GDScriptELF compilation
- [ ] Test ELF execution for various function types
- [ ] Test VM fallback execution
- [ ] Integration tests for script loading and execution
- [ ] Performance benchmarks comparing ELF vs VM execution

### Editor Integration

- [ ] Syntax highlighting for `.gde` files
- [ ] Code completion implementation
- [ ] Error reporting and diagnostics
- [ ] Debugging support
- [ ] Script templates

### Documentation

- [ ] Complete API documentation
- [ ] Usage examples
- [ ] Migration guide from GDScript to GDScriptELF
- [ ] Performance optimization guide

## Low Priority / Future Work

### Feature Completeness

- [ ] Yield/coroutines support
- [ ] Signals and connections
- [ ] RPC/network features
- [ ] All GDScript opcodes support
- [ ] Static typing support

### Performance Optimizations

- [ ] ELF binary caching
- [ ] Binary translation support improvements
- [ ] JIT compilation enhancements
- [ ] Memory management optimizations
- [ ] Parallel compilation support

### Code Quality

- [ ] Refactor compilation pipeline for better GDExtension compatibility
- [ ] Remove or conditionally compile editor-only code
- [ ] Improve error handling and reporting
- [ ] Add more comprehensive logging

## Notes

- The GDExtension migration is complete for the language interface layer
- Compilation pipeline files have been updated with GDExtension includes but may need further refactoring for full compatibility
- Some Godot engine module-specific APIs may need alternative implementations for GDExtension
- Consider creating wrapper classes or utility functions for missing GDExtension APIs

## Related Files

- `src/gdscript_elf/AGENTS.md` - Module documentation
- `src/gdscript_elf/compilation/` - Compilation pipeline (needs further work)
- `src/gdscript_elf/language/` - Language interface (mostly complete)
- `src/gdscript_elf/elf/` - ELF compilation components
