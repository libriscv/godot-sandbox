cmake_minimum_required(VERSION 3.10)
project(gdscript_compiler_program LANGUAGES C CXX)

add_subdirectory(../cmake sandbox_api)
add_subdirectory(../../../src/gdscript/compiler gdscript_compiler)

add_sandbox_program(gdscript src/compiler_main.cpp)
target_link_libraries(gdscript PRIVATE sandbox_api gdscript_compiler)
