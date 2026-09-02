cmake_minimum_required(VERSION 3.9.4)
project(riscv CXX)

option(ENABLE_SAFEGDSCRIPT "Build the SafeGDScript compiler and script language" ON)
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

# Published metadata formats the SafeGDScript host needs without an in-process
# compiler. An in-process build gets these sources from the full compiler library.
set(ELF_METADATA_SOURCES
	src/gdscript/compiler/function_signature.cpp
	src/gdscript/compiler/line_table.cpp
	src/gdscript/compiler/gdsmeta.cpp
)

if (ENABLE_SAFEGDSCRIPT)
	list(APPEND SOURCES
		src/safegdscript/builtin_api_safegdscript.cpp
		src/safegdscript/compiler_backend.cpp
		src/safegdscript/convert_safegdscript.cpp
		src/safegdscript/debug_safegdscript.cpp
		src/safegdscript/editor_plugin_safegdscript.cpp
		src/safegdscript/direct_compiler.cpp
		src/safegdscript/editor_analysis_safegdscript.cpp
		src/safegdscript/sandboxed_compiler.cpp
		src/safegdscript/profiling_safegdscript.cpp
		src/safegdscript/placeholder_instance_safegdscript.cpp
		src/safegdscript/script_class_safegdscript.cpp
		src/safegdscript/script_instance_safegdscript.cpp
		src/safegdscript/script_language_safegdscript.cpp
		src/safegdscript/script_safegdscript.cpp
		src/safegdscript/syntax_highlighter_safegdscript.cpp
		src/safegdscript/resource_loader_safegdscript.cpp
		src/safegdscript/resource_saver_safegdscript.cpp
	)

	# Where SafeGDScript compiles:
	#   sandboxed   gdscript.elf in a Sandbox, for every script
	#   restricted  in process, except for a restricted Sandbox's scripts (default)
	#   direct      in process, for all scripts (not recommended, except for debugging)
	# See src/safegdscript/compiler_backend.h.
	set(SAFEGDSCRIPT_COMPILER "restricted" CACHE STRING
		"Where the SafeGDScript compiler runs: sandboxed, restricted or direct")
	set_property(CACHE SAFEGDSCRIPT_COMPILER PROPERTY STRINGS sandboxed restricted direct)

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
		# A sandboxed compiler still needs the published formats and editor symbol
		# tables on the host. In-process policies get these from the full library.
		list(APPEND SOURCES
			${ELF_METADATA_SOURCES}
			src/gdscript/compiler/property_signature.cpp
			src/gdscript/compiler/source_model.cpp
			src/gdscript/compiler/debug_layout.cpp
			src/gdscript/compiler/globals.cpp
			src/gdscript/compiler/compiler_exception.cpp
			# analyze_source() parses; editor analysis is host-side whatever the policy.
			src/gdscript/compiler/token.cpp
			src/gdscript/compiler/lexer.cpp
			src/gdscript/compiler/parser.cpp
			src/gdscript/compiler/ast_clone.cpp
			src/gdscript/compiler/export_hints.cpp
		)
	endif()
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

if (ENABLE_SAFEGDSCRIPT)
	target_compile_definitions(godot-riscv PRIVATE
		SAFEGDSCRIPT_COMPILER_POLICY=${SAFEGDSCRIPT_COMPILER_POLICY})

	if (NOT SAFEGDSCRIPT_COMPILER_POLICY EQUAL 0)
		# EXCLUDE_FROM_ALL: don't build automatically
		add_subdirectory(src/gdscript/compiler gdscript_compiler EXCLUDE_FROM_ALL)
		target_link_libraries(godot-riscv PUBLIC gdscript_compiler)
		target_compile_definitions(godot-riscv PRIVATE SAFEGDSCRIPT_DIRECT_COMPILER)
	endif()
else()
	target_compile_definitions(godot-riscv PRIVATE SAFEGDSCRIPT_DISABLED)
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
