# Add godot-cpp library
add_subdirectory(godot-cpp)

# Add sandboxing library
option(RISCV_32I "" OFF)
option(RISCV_BINARY_TRANSLATION "" OFF)
option(RISCV_MEMORY_TRAPS "" OFF)
add_subdirectory(libriscv/lib libriscv)
target_compile_definitions(riscv PUBLIC
	RISCV_SYSCALLS_MAX=600
	RISCV_BRK_MEMORY_SIZE=0x100000  # 1MB (we don't need much)
)
if (RISCV_BINARY_TRANSLATION AND RISCV_LIBTCC)
	target_compile_options(libtcc PRIVATE -fPIC)
endif()
