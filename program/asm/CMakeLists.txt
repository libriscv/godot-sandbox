cmake_minimum_required(VERSION 3.10)
project(asmjit)

add_subdirectory(../cpp/cmake cmake)

option(ZLIB_BUILD_EXAMPLES "" OFF)

include(FetchContent)
FetchContent_Declare(
  zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG develop
)
FetchContent_MakeAvailable(zlib)

# Add the RISC-V asmjit static library
add_library(riscv_asmjit STATIC IMPORTED)
set_target_properties(riscv_asmjit PROPERTIES
	IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libRiscvAsmLib.a
)
target_link_libraries(riscv_asmjit INTERFACE zlibstatic)

# Add the asmjit sandbox program
add_sandbox_program(asmjit
	main.cpp
)

target_link_libraries(asmjit PRIVATE -static riscv_asmjit)
