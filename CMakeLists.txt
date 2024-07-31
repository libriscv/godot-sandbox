cmake_minimum_required(VERSION 3.9.4)
project(riscv CXX)

option(SANITIZE    "Enable sanitizers" OFF)
option(STATIC_BUILD "Build statically" OFF)

set(SOURCES
    src/gvar.cpp
    src/register_types.cpp
    src/resource_cpp.cpp
    src/resource_loader_cpp.cpp
    src/resource_elf.cpp
    src/resource_loader_elf.cpp
    src/sandbox.cpp
    src/sandbox_functions.cpp
	src/syscalls.cpp
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

if (MINGW)    
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++ -static")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")
endif()

if (SANITIZE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined")
endif()

# Add godot-cpp library
add_subdirectory(godot-cpp)

# Add sandboxing library
option(RISCV_BINARY_TRANSLATION "" ON)
add_subdirectory(libriscv/lib libriscv)
target_compile_definitions(riscv PUBLIC
	RISCV_SYSCALLS_MAX=600
	RISCV_BRK_MEMORY_SIZE=0x100000  # 1MB (we don't need much)
)
if (RISCV_BINARY_TRANSLATION AND RISCV_LIBTCC)
	target_compile_options(libtcc PRIVATE -fPIC)
endif()

add_library(godot-riscv SHARED ${SOURCES})
target_link_libraries(godot-riscv PUBLIC riscv godot-cpp)

if (STATIC_BUILD)
    target_link_libraries(godot-riscv PUBLIC -static)
endif()
