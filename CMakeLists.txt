cmake_minimum_required(VERSION 3.9.4)
project(riscv CXX)

option(SANITIZE    "Enable sanitizers" OFF)

set(SOURCES
	src/shmain.cpp
    src/register_types.cpp
)

if (SANITIZE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined")
endif()

add_subdirectory(godot-cpp)

add_subdirectory(libriscv/lib libriscv)

add_library(godot-riscv SHARED ${SOURCES})
target_link_libraries(godot-riscv PUBLIC riscv godot-cpp)
