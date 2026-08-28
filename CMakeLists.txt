cmake_minimum_required(VERSION 3.9.4)
project(riscv CXX)

# Where SafeGDScript compiles:
#   sandboxed   gdscript.elf in a Sandbox, for every script
#   restricted  in process, except for a restricted Sandbox's scripts (default)
#   direct      in process, for every script -- gdb sees the compiler's frames
# See src/safegdscript/compiler_backend.h.
set(SAFEGDSCRIPT_COMPILER "restricted" CACHE STRING
	"Where the SafeGDScript compiler runs: sandboxed, restricted or direct")
set_property(CACHE SAFEGDSCRIPT_COMPILER PROPERTY STRINGS sandboxed restricted direct)

option(SANITIZE    "Enable the undefined-behaviour sanitizer" OFF)
option(ASAN        "Enable the address sanitizer" OFF)
option(STATIC_BUILD "Build statically" OFF)

set(SOURCES
	src/cpp/resource_loader_cpp.cpp
	src/cpp/resource_saver_cpp.cpp
	src/cpp/script_cpp.cpp
	src/cpp/script_cpp_instance.cpp
	src/cpp/script_language_cpp.cpp
	src/elf/resource_loader_elf.cpp
	src/elf/resource_saver_elf.cpp
	src/elf/script_elf.cpp
	src/elf/script_instance.cpp
	src/elf/script_language_elf.cpp
	src/rust/resource_loader_rust.cpp
	src/rust/resource_saver_rust.cpp
	src/rust/script_rust.cpp
	src/rust/script_language_rust.cpp
	src/zig/resource_loader_zig.cpp
	src/zig/resource_saver_zig.cpp
	src/zig/script_zig.cpp
	src/zig/script_language_zig.cpp
	src/safegdscript/compiler_backend.cpp
	src/safegdscript/debug_safegdscript.cpp
	src/safegdscript/direct_compiler.cpp
	src/safegdscript/editor_analysis_safegdscript.cpp
	src/safegdscript/sandboxed_compiler.cpp
	src/safegdscript/profiling_safegdscript.cpp
	src/safegdscript/placeholder_instance_safegdscript.cpp
	src/safegdscript/script_class_safegdscript.cpp
	src/safegdscript/script_instance_safegdscript.cpp
	src/safegdscript/script_language_safegdscript.cpp
	src/safegdscript/script_safegdscript.cpp
	src/safegdscript/resource_loader_safegdscript.cpp
	src/safegdscript/resource_saver_safegdscript.cpp
	src/docker.cpp
	src/godot/script_instance.cpp
	src/guest_variant.cpp
	src/register_types.cpp
	src/sandbox.cpp
	src/sandbox_bintr.cpp
	src/sandbox_coroutines.cpp
	src/sandbox_debug.cpp
	src/sandbox_exception.cpp
	src/sandbox_functions.cpp
	src/sandbox_globals.cpp
	src/sandbox_generated_api.cpp
	src/sandbox_profiling.cpp
	src/sandbox_programs.cpp
	src/sandbox_project_settings.cpp
	src/sandbox_restrictions.cpp
	src/sandbox_syscalls.cpp
	src/sandbox_syscalls_2d.cpp
	src/sandbox_syscalls_3d.cpp
	src/override_libriscv.cpp

	src/tests/assault.cpp
)

# Pieces of the compiler the host needs whatever the policy is: the definition
# of the formats the compiler publishes beside the ELF (the guest side of both
# lives inside gdscript.elf), and the symbol tables editor completion and lookup
# resolve from. A build with an in-process compiler links the whole library
# instead, and these are part of it.
set(COMPILER_HOST_SOURCES
	src/gdscript/compiler/function_signature.cpp
	src/gdscript/compiler/property_signature.cpp
	src/gdscript/compiler/source_model.cpp
	src/gdscript/compiler/line_table.cpp
	src/gdscript/compiler/debug_layout.cpp
	src/gdscript/compiler/gdsmeta.cpp
	src/gdscript/compiler/globals.cpp
	src/gdscript/compiler/compiler_exception.cpp
)

if (SAFEGDSCRIPT_COMPILER STREQUAL "sandboxed")
	set(SAFEGDSCRIPT_COMPILER_POLICY 0)
elseif (SAFEGDSCRIPT_COMPILER STREQUAL "restricted")
	set(SAFEGDSCRIPT_COMPILER_POLICY 1)
elseif (SAFEGDSCRIPT_COMPILER STREQUAL "direct")
	set(SAFEGDSCRIPT_COMPILER_POLICY 2)
else()
	message(FATAL_ERROR "SAFEGDSCRIPT_COMPILER must be sandboxed, restricted or direct")
endif()

if (SAFEGDSCRIPT_COMPILER_POLICY EQUAL 0)
	list(APPEND SOURCES ${COMPILER_HOST_SOURCES})
endif()

# Add bintr sources
file(GLOB BINTR_FILES src/bintr/*.cpp)
list(APPEND SOURCES ${BINTR_FILES})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -fvisibility=hidden -fvisibility-inlines-hidden")

if (MINGW OR MINGW_TOOLCHAIN)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++ -static")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static-libgcc -static-libstdc++ -static")
endif()

add_subdirectory(ext)

add_library(godot-riscv SHARED ${SOURCES})
target_link_libraries(godot-riscv PUBLIC riscv godot-cpp)
target_compile_definitions(godot-riscv PRIVATE
	SAFEGDSCRIPT_COMPILER_POLICY=${SAFEGDSCRIPT_COMPILER_POLICY})

if (NOT SAFEGDSCRIPT_COMPILER_POLICY EQUAL 0)
	# The compiler in this process. EXCLUDE_FROM_ALL: the library is what is
	# wanted here, not dump_ir and the other tools that live beside it.
	# gdscript_compiler carries DOUBLE_PRECISION_REAL_T when godot-cpp does, so
	# the Variant layout it emits for and the one this build uses stay the same.
	add_subdirectory(src/gdscript/compiler gdscript_compiler EXCLUDE_FROM_ALL)
	target_link_libraries(godot-riscv PUBLIC gdscript_compiler)
	target_compile_definitions(godot-riscv PRIVATE SAFEGDSCRIPT_DIRECT_COMPILER)
endif()

# Don't re-export the statically linked libstdc++ (godot-cpp passes
# -static-libstdc++). Its std::locale facet ids are STB_GNU_UNIQUE and get
# merged process-wide with the system libstdc++.so.6 that the GPU drivers
# drag in, which leaves our facet table indexed by the other copy's ids.
# See godot-riscv.version for the full story.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT MINGW AND NOT MINGW_TOOLCHAIN)
	target_link_options(godot-riscv PRIVATE
		"-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/godot-riscv.version")
	set_target_properties(godot-riscv PROPERTIES
		LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/godot-riscv.version")
endif()

if (STATIC_BUILD)
	target_link_libraries(godot-riscv PUBLIC -static)
endif()
# The addon is a shared library loaded by a Godot binary that was not built with any
# sanitizer, so the runtime has to come from the compiler building this library and be
# loaded before Godot's first allocation. For ASAN that means LD_PRELOAD of the matching
# libasan; tests/fuzz_sanitize.sh works both out.
#
# -fno-sanitize=vptr throughout: godot-cpp hands back pointers to engine-side objects
# this library never constructed, and vptr checking needs the RTTI of the definition.
if (SANITIZE)
	target_compile_options(godot-riscv PUBLIC -fsanitize=undefined -fno-sanitize=vptr)
	target_link_options(godot-riscv PUBLIC -fsanitize=undefined)
endif()
if (ASAN)
	target_compile_options(godot-riscv PUBLIC -fsanitize=address -fno-omit-frame-pointer)
	target_link_options(godot-riscv PUBLIC -fsanitize=address)
endif()
if (ANDROID_TOOLCHAIN)
	target_link_libraries(godot-riscv PUBLIC android log)
endif()
