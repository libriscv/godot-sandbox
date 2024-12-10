cmake_minimum_required(VERSION 3.14)
project (godot-sandbox-programs)

# Fetch godot-sandbox repository and add_subdirectory
include(FetchContent)
FetchContent_Declare(
	godot-sandbox
	GIT_REPOSITORY https://github.com/libriscv/godot-sandbox.git
	GIT_TAG        main
	GIT_PROGRESS   TRUE
	SOURCE_SUBDIR  "program/cpp/cmake"
)
FetchContent_MakeAvailable(godot-sandbox)

add_sandbox_program(sandbox_program.elf
	"src/program.cpp"
)
