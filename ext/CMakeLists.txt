# Add godot-cpp library
option(GODOTCPP_DISABLE_EXCEPTIONS "" OFF)
set(GODOTCPP_TARGET "template_release" CACHE STRING "")
add_subdirectory(godot-cpp)

# Add sandboxing library
option(RISCV_32I "" OFF)
option(RISCV_MEMORY_TRAPS "" OFF)
option(RISCV_EXT_V "" ON)

# libriscv's asmjit back-end is written against asmjit's universal compiler,
# which only has a code generator for x86-64 and AArch64. Everywhere else
# (x86-32, 32-bit ARM, ...) asmjit defines ASMJIT_NO_UJIT and the back-end
# compiles to nothing, so those hosts keep the C99 binary translator.
# Exclude a 32-bit build on a 64-bit processor (-m32), but treat an unknown
# pointer size as 64-bit: a CC/CXX-only MinGW cross leaves CMAKE_SIZEOF_VOID_P
# empty, see mingw_toolchain.cmake.
set(ASMJIT_HOST_SUPPORTED OFF)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|AMD64|amd64|aarch64|arm64|ARM64)$"
	AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	set(ASMJIT_HOST_SUPPORTED ON)
endif()
option(RISCV_ASMJIT "" ${ASMJIT_HOST_SUPPORTED})

# asmjit compiles straight to machine code in-process, which leaves the C99
# binary translator with nothing to do: only one back-end can own the patched
# decoder cache, and carrying the other one costs an embedded TCC plus the
# emit/compile/dlopen path in every build. So a JIT build drops it, which
# no-ops emit_binary_translation(), try_compile_binary_translation() and
# load_binary_translation() -- see doc_classes/Sandbox.xml. Pass
# -DRISCV_BINARY_TRANSLATION=ON to build both for A/B comparison.
if (RISCV_ASMJIT)
	option(RISCV_BINARY_TRANSLATION "" OFF)
else()
	option(RISCV_BINARY_TRANSLATION "" ON)
endif()
if (NOT RISCV_BINARY_TRANSLATION)
	# libtcc is a binary-translation back-end, so without the translator there
	# is nothing for it to compile. Forced rather than defaulted: an inherited
	# -DRISCV_LIBTCC=ON would otherwise reach libriscv_settings.h and have
	# libtcc_enabled claim a TCC that was never built in.
	set(RISCV_LIBTCC OFF CACHE BOOL "" FORCE)
endif()

add_subdirectory(libriscv/lib libriscv)
target_compile_definitions(riscv PUBLIC
	RISCV_SYSCALLS_MAX=600
	RISCV_BRK_MEMORY_SIZE=0x100000  # 1MB (we don't need much)
)
