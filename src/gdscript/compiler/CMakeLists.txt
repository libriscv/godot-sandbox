# GDScript to RISC-V Compiler
cmake_minimum_required(VERSION 3.10)

project(gdscript_compiler)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler sources
set(COMPILER_SOURCES
    token.cpp
    lexer.cpp
    parser.cpp
    ir.cpp
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

# Build demo executable
add_executable(gdscript_compiler_test test_compiler.cpp)
target_link_libraries(gdscript_compiler_test gdscript_compiler)

# Build unit tests
enable_testing()

add_executable(test_lexer tests/test_lexer.cpp)
target_link_libraries(test_lexer gdscript_compiler)
add_test(NAME LexerTests COMMAND test_lexer)

add_executable(test_parser tests/test_parser.cpp)
target_link_libraries(test_parser gdscript_compiler)
add_test(NAME ParserTests COMMAND test_parser)

add_executable(test_codegen tests/test_codegen.cpp)
target_link_libraries(test_codegen gdscript_compiler)
add_test(NAME CodeGenTests COMMAND test_codegen)

add_executable(test_integration tests/test_integration.cpp)
target_link_libraries(test_integration gdscript_compiler)
add_test(NAME IntegrationTests COMMAND test_integration)

add_executable(test_primitives tests/test_primitives.cpp)
target_link_libraries(test_primitives gdscript_compiler)
add_test(NAME PrimitivesTests COMMAND test_primitives)

add_executable(test_compilation tests/test_compilation.cpp)
target_link_libraries(test_compilation gdscript_compiler)
add_test(NAME CompilationTests COMMAND test_compilation)

# Run all tests target
add_custom_target(run_tests
	COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
	DEPENDS test_lexer test_parser test_codegen test_integration test_primitives test_compilation
	COMMENT "Running all GDScript compiler tests"
)

# Install targets
install(TARGETS gdscript_compiler gdscript_compiler_test
        test_lexer test_parser test_codegen test_integration test_primitives test_compilation
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
