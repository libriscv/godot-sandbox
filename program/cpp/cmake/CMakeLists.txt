# Contents: CMake configuration for a Godot Sandbox C++ program
# This file is part of the Godot Sandbox project
#
# This file is licensed under the BSD-3-Clause license
# See the LICENSE file in the project root for more information
#
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS ON)
set(VERSION 9)

option(SANDBOX_RISCV_EXT_C "Enable RISC-V C extension" ON)
option(DOUBLE_PRECISION    "Enable double precision real_t" OFF)

set(CXXFLAGS -O2 -std=gnu++23 -fno-stack-protector -fno-threadsafe-statics)
set(CFLAGS   -O2 -std=gnu11 -fno-stack-protector -fno-threadsafe-statics)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(CXXFLAGS ${CXXFLAGS} -O0 -gdwarf-4)
	set(CFLAGS   ${CFLAGS} -O0 -gdwarf-4)
elseif (CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
	set(CXXFLAGS ${CXXFLAGS} -O2 -gdwarf-4)
	set(CFLAGS   ${CFLAGS} -O2 -gdwarf-4)
endif()

# If the compiler contains Zig, don't set RISCV_ARCH and RISCV_ABI
if (${CMAKE_CXX_COMPILER} MATCHES "zig")
	set(RISCV_ARCH "-mcpu=baseline_rv64+rva22u64")
	set(RISCV_ABI "-mabi=lp64d")
else()
	if (SANDBOX_RISCV_EXT_C)
		set(RISCV_ARCH "-march=rv64gc_zba_zbb_zbs_zbc")
	else()
		set(RISCV_ARCH "-march=rv64g_zba_zbb_zbs_zbc")
	endif()
	set(RISCV_ABI "-mabi=lp64d")
endif()

set(API_DIR ${CMAKE_CURRENT_LIST_DIR}/api)

set(API_SOURCES
	"${API_DIR}/api.cpp"
	"${API_DIR}/array.cpp"
	"${API_DIR}/basis.cpp"
	"${API_DIR}/dictionary.cpp"
	"${API_DIR}/native.cpp"
	"${API_DIR}/node.cpp"
	"${API_DIR}/node2d.cpp"
	"${API_DIR}/node3d.cpp"
	"${API_DIR}/object.cpp"
	"${API_DIR}/packed_array.cpp"
	"${API_DIR}/quaternion.cpp"
	"${API_DIR}/string.cpp"
	"${API_DIR}/timer.cpp"
	"${API_DIR}/transform2d.cpp"
	"${API_DIR}/transform3d.cpp"
	"${API_DIR}/variant.cpp"
	"${API_DIR}/vector.cpp"
)

# Sandbox API library
add_library(sandbox_api STATIC ${API_SOURCES})
target_compile_definitions(sandbox_api PUBLIC
	VERSION=${VERSION}
)
target_compile_options(sandbox_api PUBLIC
	${RISCV_ARCH} ${RISCV_ABI}
	$<$<COMPILE_LANGUAGE:CXX>:${CXXFLAGS}>
	$<$<COMPILE_LANGUAGE:C>:${CFLAGS}>
)
target_include_directories(sandbox_api PUBLIC
	${API_DIR}
)
if (NOT ZIG_COMPILER)
	target_link_libraries(sandbox_api PUBLIC
		-Wl,--wrap=memcpy,--wrap=memset,--wrap=memcmp,--wrap=memmove
		-Wl,--wrap=strlen,--wrap=strcmp,--wrap=strncmp
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
	)
else()
	target_compile_definitions(sandbox_api PUBLIC
		ZIG_COMPILER=1
	)
	target_link_libraries(sandbox_api PUBLIC
		-Wl,--image-base=0x100000
	)
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	# If the GCC version is 14.0 or higher, we can use the -fuse-ld=mold flag
	if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14.0)
		find_program(MOLD_LINKER "mold")
		if (MOLD_LINKER)
			message(STATUS "Using MOLD linker")
			target_link_libraries(sandbox_api PUBLIC -fuse-ld=mold)
		endif()
	endif()
endif()

function (add_sandbox_program name)
	add_executable(${name} ${ARGN})
	target_link_libraries(${name} PRIVATE
		-Wl,--whole-archive sandbox_api -Wl,--no-whole-archive
	)
	target_link_libraries(${name} PUBLIC
		-static
	)
	if (DOUBLE_PRECISION)
		target_compile_definitions(${name} PUBLIC
			DOUBLE_PRECISION_REAL_T
		)
	endif()
	target_include_directories(${name} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
	)
endfunction()

function (add_sandbox_program_at name path)
	add_sandbox_program(${name} ${ARGN})
	set_target_properties(${name} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${path}
		RUNTIME_OUTPUT_NAME ${name}.elf
	)
endfunction()

function (add_sandbox_library name)
	add_library(${name} STATIC ${ARGN})
	target_link_libraries(${name} INTERFACE
		sandbox_api
	)
endfunction()

# Example:
# add_sandbox_program(<name> <source_files>)
# add_sandbox_library(<name> <source_files>)
