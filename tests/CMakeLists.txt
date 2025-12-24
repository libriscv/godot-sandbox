cmake_minimum_required(VERSION 3.10)
project(unittests LANGUAGES C CXX)

add_subdirectory(../program/cpp/cmake sandbox_api)
add_subdirectory(../src/gdscript/compiler gdscript_compiler)

add_sandbox_program(unittests
	"tests/test_basic.cpp"
	"tests/test_math.cpp"
	"tests/test_properties.cpp"
	"tests/test_shm.cpp"
	"tests/test_gdscript_compiler.cpp"
)
target_link_libraries(unittests PRIVATE sandbox_api gdscript_compiler)
