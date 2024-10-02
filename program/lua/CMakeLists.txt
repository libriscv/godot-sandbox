cmake_minimum_required(VERSION 3.10)
project(lua)

add_subdirectory(../cpp/cmake cmake)

add_sandbox_program(lua
	main.cpp
)

target_link_libraries(lua PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/libluajit.a
)
target_include_directories(lua PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
)
