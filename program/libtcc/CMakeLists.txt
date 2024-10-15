cmake_minimum_required(VERSION 3.10)
project(cjit)

add_subdirectory(../cpp/cmake cmake)

add_sandbox_program(cjit
	main.cpp
)

include(FetchContent)
FetchContent_Declare(tinycc
	GIT_REPOSITORY https://github.com/TinyCC/tinycc.git
	GIT_TAG        mob
)
FetchContent_MakeAvailable(tinycc)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/_deps/tinycc-src/config.h)

add_library(libtcc STATIC
	${tinycc_SOURCE_DIR}/libtcc.c
	${tinycc_SOURCE_DIR}/libtcc.h
)
target_compile_definitions(libtcc PRIVATE
	CONFIG_TCC_PREDEFS=1
	TCC_TARGET_RISCV64=1
	TARGETOS_Linux=1
	TCC_VERSION="0.9.27"
)
target_include_directories(libtcc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(cjit PRIVATE libtcc)
