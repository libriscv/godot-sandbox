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

# The C99 translator and asmjit coexist: a cached AOT object claims decoder
# entries first and asmjit fills whatever remains. Desktop builds keep asmjit
# for immediate iteration and the C99 emitter/loader for baked shipping code.
option(RISCV_BINARY_TRANSLATION "" ON)
# Baking intentionally uses an external system compiler. Never embed TCC: its
# background compiler would suppress asmjit and would compile during play.
set(RISCV_LIBTCC OFF CACHE BOOL "" FORCE)

add_subdirectory(libriscv/lib libriscv)
target_compile_definitions(riscv PUBLIC
	RISCV_SYSCALLS_MAX=600
	RISCV_BRK_MEMORY_SIZE=0x100000  # 1MB (we don't need much)
)
