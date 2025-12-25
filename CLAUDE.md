This project is a sandbox addon for the Godot engine. It implements a Sandbox node which can load a RISC-V ELF binary, which can then execute code and access the host Godot instance. The access can be restricted by class names, methods, properties etc. Execution is memory safe and has an optional execution timeout using instruction counting. The underlying emulator is libriscv. The emulator runs either in JIT or interpreter mode. This can be toggled during init. Both JIT and interpreter modes are faster than GDScript. Whitespace is tabs.

The Sandbox node can be instantiated, given an ELF and then it can export functions and properties making it a sort-of script-like instance. It can also be used as a Script, by attaching it to the Script of any Node. In that case, calling functions on the node will be forwarded to the script, which again calls into the sandboxed guest program. The sandbox node is implemented in src/sandbox.cpp and src/sandbox.h as well as a few src/sandbox_*.cpp files. The cpp ScriptLanguage is in src/cpp/*. The sandbox API is implemented in src/sandbox_syscalls.cpp and sandbox_syscalls_*.cpp.

The API inside the Sandbox follows the public GDScript API closely, simply because it implements the complete Variant with all types, as well as the ability to call functions on objects.

The Sandbox API is written in C++. It accesses the host using system calls. Most system calls are dedicated to handling methods in the very common Variant types. Eg. the most important methods in PackedVector3Array will have dedicated system calls, while the rest are achieved through vcall (a call on a Variant which holds the PackedVector3Array). This also means that the Godot Sandbox API is complete, matching the GDScript capabilities.

ELFScript is a Godot-specific Resource type that is the result of loading and ELF into a Godot Project. It can be placed into a Node, which will make a Sandbox with that resource (ELF) loaded. It really has nothing to do with the ELF format. It's just a handler for resources that end with .elf (which _are_ ELF binaries), but any actual ELF parsing happens only in the libriscv emulator.

The host-side and guest-side share a common system call API for all languages supported. The system calls are defined in syscalls.h, and are fixed numbers that cannot be reassigned, as that would break existing users programs. Instead new functionality is added as new system calls, or as options to existing system calls when that is possible.

There is an ongoing GDScript-to-RISC-V compiler project under the src/gdscript/compiler folder. It's parsing GDScript into AST, then to IR and it will finally be transformed to 64-bit RISC-V and packed into an ELF container as the last step. At that point the goal is to make it executable inside Godot Sandbox. It has written like a CMake library, and it currently being used in the unit tests. One unit test compiles an ELF inside the sandbox and then runs the result in another sandbox. When running tests for the compiler, they should have a timeout as loops may run forever. Do NOT under any circumstance disable tests, FIX the problem. The unit tests can be executed with `ctest .` in the src/gdscript/compiler/build folder. IR can be inspected using the `cat /tmp/test_simple.gd | ./dump_ir` program in the compiler build folder.

The GDScript-to-RISC-V unit tests are under /tests/tests. You can only visually inspect RISC-V ELFs using riscv64-linux-gnu-objdump. Executing the unit tests specific to GDScript is from the tests folder:
./run_unittests.sh -gselect compiler
Always run unit tests from the tests folder. There is a separate script for running Zig C++ unit tests:
./zig_unittests.sh -gselect compiler
Also must be run from the tests folder. The GDScript compiler is RUNNING INSIDE a sandbox instance in all unit tests. That means when there's a failure and the originating call is `compile_to_elf`, it means the compiler crashed/failed INSIDE the sandbox instance.

The Godot Sandbox ABI has a complex passing scheme in order to be fast and low-latency. However, it has a second mode where all arguments in and out are always Variants. Using Variant only is the best choice for the GDScript compiler, as it makes it fully dynamic just like real GDScript. Most people don't use type hints anyway, and we'd have to verify them regardless. Real GDScript has an optimization phase where bytecodes are replaced with faster specific bytecodes once types have been checked. We are not going to focus on that, but rather on getting everything up and working.

Register A0 is always the Variant return value pointer. A1-A7 are function arguments as pointers.
The Variant ABI is a 4-byte type, 4-byte padding and then 16-bytes of inlined data (for certain types), making it 24 bytes with regular settings. Variants are placed on the stack and the pointer is placed in registers for calls. In order to make the sandbox not unbox Variants into registers `vmcallv()` is used instead of `vmcall()`. Variants don't need be zeroed. Only the relevant portion has to have non-garbage value. For example Integer type needs only m_type and v.i to be valid, covering only 12/24 bytes.

The GDScript compiler has a register allocator that can be used to avoid clobbered registers especially when performing system calls. Any new feature needs an internal (CMake) test before an integration test in the unit tests is written.

The RISC-V codegen has to use Variants as that is the fundamental unit of GDScript. The structure of the Variant can be seen in program/cpp/docker/api/variant.hpp, including the enum types. This means we can actually add together two Vector2's, two Vector4i's, concatenate strings etc. without really knowing what they are. Any optimization needs to carefully consider whether or not we know the actual Variant types being dealt with before reconstructing the logic. Under some circumstances it might be okay to use type hints without verifying them. Sandboxed Variants aren't real Variants - they are closely guarded, so "using them wrong" is not going to hurt the host (unless there is a bug). So, I think that for example adding two type-hinted integers or floats could be performed in physical registers, or at least without going through VEVAL.

When dealing with floats it's CRUCIAL to understand that:
1. v.f (the regular float) in the Variant structure is _always_ 64-bit.
2. real_t is CONFIGURABLE, but 32-bit float by default
3. Adding integer and float (whether constant or not) produces a float result
