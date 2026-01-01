# GDScript Compiler IR Optimizations

This document describes the IR-level optimizations implemented in the GDScript-to-RISC-V compiler, along with planned future optimizations.

## Overview

The optimizer operates on the intermediate representation (IR) before RISC-V code generation. Multiple optimization passes run iteratively to reduce code size, eliminate redundancy, and improve runtime performance.

## Currently Implemented Optimizations

### 1. Constant Folding
**Status:** ✅ Implemented
**Safety:** Very Safe
**Impact:** Medium

Evaluates constant expressions at compile time instead of runtime.

**Example:**
```gdscript
var x = 5 + 10  # Compiled as: x = 15
var y = 2.0 * 3.0  # Compiled as: y = 6.0
```

**IR Transformation:**
```
Before:
  LOAD_IMM r1, 5
  LOAD_IMM r2, 10
  ADD r0, r1, r2

After:
  LOAD_IMM r0, 15
```

**Details:**
- Handles integer and float arithmetic
- Supports comparison operations
- Respects GDScript type promotion (int + float → float)
- Avoids division by zero

### 2. Copy Propagation
**Status:** ✅ Implemented
**Safety:** Very Safe
**Impact:** Low-Medium

Eliminates redundant MOVE instructions after constant loads.

**IR Transformation:**
```
Before:
  LOAD_IMM r0, 5
  MOVE r1, r0
  # r0 never used again
  ADD r2, r1, r3

After:
  LOAD_IMM r1, 5
  ADD r2, r1, r3
```

### 3. Enhanced Copy Propagation
**Status:** ✅ Implemented (New!)
**Safety:** Very Safe
**Impact:** Medium

Advanced copy propagation that tracks copy chains and propagates sources through uses.

**Example:**
```gdscript
var a = x
var b = a
return b  # Can use x directly
```

**IR Transformation:**
```
Before:
  MOVE r1, r0
  CMP_LT r3, r1, r2  # Uses r1

After:
  MOVE r1, r0
  CMP_LT r3, r0, r2  # Uses r0 directly
```

**Details:**
- Follows copy chains to original source
- Verifies source register hasn't been modified
- Clears tracking at control flow boundaries
- Handles special case for BRANCH_ZERO/BRANCH_NOT_ZERO

### 4. Loop-Invariant Code Motion (LICM)
**Status:** ✅ Implemented (New!)
**Safety:** Safe
**Impact:** High (for simple loops)
**Limitation:** Currently disabled for functions with nested loops

Moves loop-invariant computations outside of loops to avoid redundant execution.

**Example:**
```gdscript
while i < 10:  # "10" is loop-invariant
    i = i + 1  # "1" is loop-invariant
```

**IR Transformation:**
```
Before:
  LABEL @loop
    LOAD_IMM r2, 10     # Executed every iteration
    CMP_LT r3, r1, r2
    BRANCH_ZERO r3, @end
    LOAD_IMM r5, 1      # Executed every iteration
    ADD r0, r0, r5
    JUMP @loop
  LABEL @end

After:
  LOAD_IMM r2, 10       # Hoisted out
  LOAD_IMM r5, 1        # Hoisted out
  LABEL @loop
    CMP_LT r3, r1, r2
    BRANCH_ZERO r3, @end
    ADD r0, r0, r5
    JUMP @loop
  LABEL @end
```

**Details:**
- Identifies loops via back edges (JUMP to earlier labels)
- Only hoists pure operations (LOAD_IMM, LOAD_FLOAT_IMM, LOAD_BOOL, LOAD_STRING, MOVE)
- Verifies destination register not read before definition
- Verifies source registers not modified anywhere in loop
- Iteratively finds transitive invariants
- **Skips entire function if any nested loops detected** (conservative approach to avoid hoisting past wrong loop headers)

### 5. Peephole Optimization
**Status:** ✅ Implemented
**Safety:** Very Safe
**Impact:** Medium

Pattern-based local optimizations on small instruction windows.

**Patterns:**
- Eliminate self-moves: `MOVE r0, r0` → *removed*
- Collapse move chains around operations
- Remove redundant move pairs: `MOVE r1, r0; MOVE r0, r1` → *removed*
- Optimize common patterns like `count = count + 1`

### 6. Redundant Store Elimination
**Status:** ✅ Implemented
**Safety:** Safe
**Impact:** Low-Medium

Removes dead stores and consecutive identical stores.

**Example:**
```
LOAD_IMM r0, 10
LOAD_IMM r0, 20  # First store is dead

→

LOAD_IMM r0, 20
```

### 7. Dead Code Elimination
**Status:** ✅ Implemented
**Safety:** Conservative (safe)
**Impact:** Low

Removes instructions that produce unused values.

**Details:**
- Only eliminates truly pure operations (LOAD_IMM, LOAD_BOOL)
- Conservative to avoid breaking control flow or side effects
- Tracks live registers through the function

## Planned Future Optimizations

### 8. Common Subexpression Elimination (CSE)
**Status:** 🔜 Planned
**Safety:** Safe
**Impact:** Medium
**Priority:** High

Detect and reuse previously computed values.

**Example:**
```
ADD r1, r0, r2
...
ADD r3, r0, r2  # Same computation

→

ADD r1, r0, r2
...
MOVE r3, r1     # Reuse
```

**Implementation Notes:**
- Track available expressions with their result registers
- Invalidate on register writes
- Only for pure operations

### 9. Strength Reduction
**Status:** 🔜 Planned
**Safety:** Moderate (requires type hints)
**Impact:** Medium
**Priority:** Medium

Replace expensive operations with cheaper equivalents.

**Examples:**
```
MUL r0, r1, 2   →  ADD r0, r1, r1    (for type-hinted ints)
DIV r0, r1, 4   →  SRA r0, r1, 2     (for type-hinted ints)
MUL r0, r1, 0   →  LOAD_IMM r0, 0
```

**Implementation Notes:**
- Only safe with proper type hints
- Must respect GDScript semantics (int vs float)
- Be careful with overflow/underflow

### 10. Algebraic Simplifications
**Status:** 🔜 Planned
**Safety:** Very Safe
**Impact:** Low-Medium
**Priority:** Medium

Apply algebraic identities to simplify expressions.

**Examples:**
```
ADD r0, r1, 0    →  MOVE r0, r1
MUL r0, r1, 1    →  MOVE r0, r1
MUL r0, r1, 0    →  LOAD_IMM r0, 0
SUB r0, r1, 0    →  MOVE r0, r1
```

**Implementation Notes:**
- Very safe for integers
- Be careful with NaN/infinity for floats

### 11. Branch Simplification
**Status:** 🔜 Planned
**Safety:** Safe
**Impact:** Low
**Priority:** Low

Simplify redundant branch patterns.

**Example:**
```
CMP_LT r0, r1, r2
BRANCH_ZERO r0, @skip
LOAD_BOOL r0, 1
JUMP @cont
LABEL @skip
LOAD_BOOL r0, 0
LABEL @cont

→

CMP_LT r0, r1, r2  # Already produces bool
```

### 12. Dead Label Elimination
**Status:** 🔜 Planned
**Safety:** Very Safe
**Impact:** Low (code size only)
**Priority:** Low

Remove labels that are never jumped to.

**Implementation:**
- Scan all JUMP/BRANCH instructions for target labels
- Remove unreferenced labels

### 13. Type-Hint Propagation
**Status:** 🔜 Planned
**Safety:** Safe
**Impact:** Medium (enables other opts)
**Priority:** High

Track and propagate type hints through the IR.

**Example:**
```
LOAD_IMM r0, 5        # Mark as INT
LOAD_IMM r1, 10       # Mark as INT
ADD r2, r0, r1        # Propagate INT type hint
```

**Benefits:**
- Enables strength reduction
- Allows specialized codegen for known types
- Can skip dynamic type checks in VEVAL

### 14. Safe Register Renumbering
**Status:** 🔜 Planned
**Safety:** Moderate (must preserve ABI)
**Impact:** Low (reduces stack frame size)
**Priority:** Low

Compact virtual register numbering while preserving calling convention.

**Current Issue:**
The existing `reduce_register_pressure()` is disabled because it breaks the calling convention (parameters in r0-r6, return in r0).

**Implementation Plan:**
- Preserve r0-r6 for parameters and return value
- Only renumber temporary registers (r7+)
- Track register lifetime intervals
- Compact non-overlapping lifetimes

### 15. Induction Variable Optimization
**Status:** 🔜 Planned
**Safety:** Moderate
**Impact:** High (for counted loops)
**Priority:** Medium

Optimize loop counter variables.

**Example:**
```gdscript
for i in range(10):
    array[i] = i * 2
```

**Optimizations:**
- Strength reduction: `i * 2` → increment by 2
- Dead code elimination if induction variable unused after loop

### 16. Load/Store Coalescing
**Status:** 🔜 Planned
**Safety:** Safe
**Impact:** Medium
**Priority:** Medium

Merge adjacent stack operations targeting the same Variant.

**Already partially implemented** in `eliminate_redundant_stores()` but could be enhanced.

## Optimization Pipeline Order

The optimizations run in this order (with iteration):

1. **Constant Folding** - Enables downstream optimizations
2. **Copy Propagation** - Basic cleanup
3. **Enhanced Copy Propagation** - Advanced cleanup
4. **Loop-Invariant Code Motion** - Hoist invariants
5. **Peephole Optimization** - Local patterns
6. **Peephole Optimization** - Second pass for new patterns
7. **Redundant Store Elimination** - Before dead code
8. **Peephole Optimization** - Final cleanup
9. **Dead Code Elimination** - Remove unused code

## Why Register Allocation Doesn't Help (Currently)

The compiler has a sophisticated register allocator that assigns 18 physical RISC-V registers (t0-t6, s1-s11) to virtual registers. However, **the RISC-V codegen is entirely stack-based and ignores these allocations**.

### Current Codegen Approach

All operations:
1. Load Variants from stack
2. Operate using only `t0` as scratch register
3. Store results back to stack

**Example from generated assembly:**
```asm
ld   t0, 24(sp)      # Load from stack
sd   t0, 48(sp)      # Store to stack
ld   t0, 32(sp)      # Load from stack
sd   t0, 56(sp)      # Store to stack
# ... only t0 is used!
```

### Why This Approach Was Chosen

The Variant ABI is complex:
- Variants are 24 bytes (type + padding + 16-byte data)
- Register passing would require unpacking/packing
- System calls operate on Variant pointers (stack addresses)
- Using `vmcallv()` keeps everything Variant-based

### Future: Register-Based Codegen

A future optimization could leverage register allocation for:
- **Type-hinted scalars**: Keep int/float in registers directly
- **Reference tracking**: Keep object IDs in registers
- **Specialized operations**: Fast paths for known types

This would require:
- Type hint propagation (planned optimization #13)
- Specialized IR opcodes for typed operations
- Codegen that generates different code for hinted vs dynamic paths

## Performance Notes

### Optimization Impact on Loop Performance

For a simple counting loop (`while i < 10: i = i + 1`):
- **LICM** reduces per-iteration instruction count by ~20%
- **Enhanced copy propagation** eliminates redundant MOVE instructions
- Combined: Significant reduction in loop overhead

### Current Bottleneck

The main bottleneck is the stack-based Variant codegen, not the IR optimizations. Future work should focus on:
1. Type-hint propagation
2. Specialized codegen for hinted types
3. Register-based fast paths

## Contributing

When adding new optimizations:

1. **Safety First**: Ensure the optimization preserves semantics
2. **Add Tests**: Both unit tests (CMake) and integration tests
3. **Document**: Update this file with examples and impact analysis
4. **Benchmark**: Measure actual performance impact
5. **Iterate**: Consider interaction with existing passes

## References

- Variant ABI: `program/cpp/docker/api/variant.hpp`
- IR Definition: `ir.h`
- Optimizer: `ir_optimizer.{h,cpp}`
- Codegen: `riscv_codegen.cpp`
- Register Allocator: `register_allocator.{h,cpp}` (currently unused by codegen)
