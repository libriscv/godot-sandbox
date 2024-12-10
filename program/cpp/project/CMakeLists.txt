cmake_minimum_required(VERSION 3.14)
project (godot-sandbox-programs)

if (${CMAKE_C_COMPILER} MATCHES "riscv64-unknown-elf*")
	set(EMBEDDED_RISCV 1)
	message(STATUS "Building for embedded RISC-V")
elseif (${CMAKE_C_COMPILER} MATCHES "zig")
	set(ZIG_COMPILER 1)
	message(STATUS "Building using zig c++")
	# Not necessary, but lowers binary size dramatically
	add_library(atomic STATIC "${CMAKE_SOURCE_DIR}/src/atomic.c")
	link_directories("${CMAKE_BINARY_DIR}")
endif()

add_subdirectory("${CMAKE_SOURCE_DIR}/../cmake" cmake)

add_sandbox_program(sandbox_program
	"src/program.cpp"
)
