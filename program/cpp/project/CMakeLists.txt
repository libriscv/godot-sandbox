cmake_minimum_required(VERSION 3.14)
project (godot-sandbox-programs)

add_subdirectory("${CMAKE_SOURCE_DIR}/../cmake" cmake)

add_sandbox_program(sandbox_program.elf
	"src/program.cpp"
)
