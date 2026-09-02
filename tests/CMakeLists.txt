cmake_minimum_required(VERSION 3.10)
project(unittests LANGUAGES C CXX)

add_subdirectory(../program/cpp/cmake sandbox_api)
add_subdirectory(../src/gdscript/compiler gdscript_compiler)

add_sandbox_program(unittests
	"tests/test_basic.cpp"
	"tests/test_bench.cpp"
	"tests/test_math.cpp"
	"tests/test_properties.cpp"
	"tests/test_gdscript_compiler.cpp"
)
target_link_libraries(unittests PRIVATE sandbox_api gdscript_compiler)
# test_gdscript_compiler.cpp exports the same compiler entry points as
# addons/godot_sandbox/gdscript.elf, and shares their implementation.
target_include_directories(unittests PRIVATE ../program/cpp/gdscript_compiler/src)

# Standalone program: all-numeric defaults so no side-effect keeps the array alive. GH #272.
add_sandbox_program(trivial_properties
	"tests/test_trivial_properties.cpp"
)

add_sandbox_program(dict_plateau
	"tests/test_dict_plateau.cpp"
)
