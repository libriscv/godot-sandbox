# GDScript to RISC-V Compiler
cmake_minimum_required(VERSION 3.10)

project(gdscript_compiler)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Godot can be built with real_t = double, which widens every real_t-based
# Variant payload (vectors, Rect2, Plane, Quaternion, Color) and grows the
# Variant itself from 24 to 40 bytes. The generated code has to match the host,
# so the default comes from the build. It stays overridable at run time through
# CompilerOptions::double_precision.
# No-op if a parent project (the sandbox API, godot-cpp) already declared it.
option(DOUBLE_PRECISION "Enable double precision real_t" OFF)

# Compiler sources
set(COMPILER_SOURCES
    compiler_exception.cpp
    token.cpp
    lexer.cpp
    parser.cpp
    ir.cpp
    ir_optimizer.cpp
    codegen.cpp
    riscv_codegen.cpp
    register_allocator.cpp
    elf_builder.cpp
    compiler.cpp
    ir_interpreter.cpp
)

# Build as library
add_library(gdscript_compiler STATIC ${COMPILER_SOURCES})
target_include_directories(gdscript_compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if (DOUBLE_PRECISION OR GODOTCPP_PRECISION STREQUAL "double")
	# PUBLIC: the default in CompilerOptions is a header-inline value, so every
	# translation unit that includes compiler.h has to agree on it.
	target_compile_definitions(gdscript_compiler PUBLIC DOUBLE_PRECISION_REAL_T)
endif()

# Build demo executable
add_executable(gdscript_compiler_test test_compiler.cpp)
target_link_libraries(gdscript_compiler_test gdscript_compiler)

function(add_gdscript_test test_name test_source)
	add_executable(${test_name} ${test_source})
	target_compile_options(${test_name} PUBLIC -fsanitize=address,undefined)
	target_link_libraries(${test_name} -fsanitize=address,undefined gdscript_compiler)
	add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# Dump IR utility
add_executable(dump_ir dump_ir.cpp)
if(APPLE)
	target_link_libraries(dump_ir gdscript_compiler)
else()
	target_link_libraries(dump_ir -static gdscript_compiler)
endif()

# GDScript to RISC-V disassembly tool
add_executable(gdscript_to_riscv gdscript_to_riscv.cpp)
if(APPLE)
	target_link_libraries(gdscript_to_riscv gdscript_compiler)
else()
	target_link_libraries(gdscript_to_riscv -static gdscript_compiler)
endif()

if (BUILD_TESTING)
	enable_testing()

	add_gdscript_test(test_lexer tests/test_lexer.cpp)
	add_gdscript_test(test_parser tests/test_parser.cpp)
	add_gdscript_test(test_codegen tests/test_codegen.cpp)
	add_gdscript_test(test_integration tests/test_integration.cpp)
	add_gdscript_test(test_primitives tests/test_primitives.cpp)
	add_gdscript_test(test_compilation tests/test_compilation.cpp)
	add_gdscript_test(test_constant_pool tests/test_constant_pool.cpp)
	add_gdscript_test(test_ir_optimizer tests/test_ir_optimizer.cpp)
	add_gdscript_test(test_double_precision tests/test_double_precision.cpp)
	add_gdscript_test(test_regressions tests/test_regressions.cpp)

	# Run all tests target
	add_custom_target(run_tests
		COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
		DEPENDS test_lexer test_parser test_codegen test_integration test_primitives test_compilation test_constant_pool test_ir_optimizer test_double_precision test_regressions
		COMMENT "Running all GDScript compiler tests"
	)

	# Install test targets (only when BUILD_TESTING is ON)
	install(TARGETS test_lexer test_parser test_codegen test_integration test_primitives test_compilation test_constant_pool test_ir_optimizer test_double_precision test_regressions
	        RUNTIME DESTINATION bin)
endif()

# Install targets (always built)
install(TARGETS gdscript_compiler gdscript_compiler_test
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
