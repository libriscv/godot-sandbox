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
    function_signature.cpp
    line_table.cpp
    gdsmeta.cpp
    token.cpp
    lexer.cpp
    parser.cpp
    chain.cpp
    ir.cpp
    ir_optimizer.cpp
    ir_verifier.cpp
    codegen.cpp
    riscv_codegen.cpp
    riscv_globals.cpp
    riscv_profiling.cpp
    riscv_debug.cpp
    register_allocator.cpp
    elf_builder.cpp
    compiler.cpp
    ir_interpreter.cpp
    globals.cpp
    export_hints.cpp
)

# Build as library
add_library(gdscript_compiler STATIC ${COMPILER_SOURCES})
target_include_directories(gdscript_compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# A dispatch over IROpcode must not have a bare `default:`. Without one, adding
# an opcode to ir_opcodes.def turns every dispatch site into a compile error
# listing exactly what still needs handling; with one, it compiles and quietly
# does the wrong thing. -Werror=switch is what makes that a rule rather than a
# convention. PRIVATE, so a project embedding this library is not held to it.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
	target_compile_options(gdscript_compiler PRIVATE -Wswitch -Werror=switch)

	# -Wunused-variable is what catches an option parsed and then never passed
	# on, which is how `gdscript_to_riscv --no-optimize` spent a while quietly
	# optimizing. Warnings, not errors: the guest build uses a different
	# compiler version and must not fail on one it words differently.
	# Unused parameters are excluded -- several are there for an interface.
	set(GDSC_WARNINGS -Wall -Wextra -Wno-unused-parameter)
	target_compile_options(gdscript_compiler PRIVATE ${GDSC_WARNINGS})
endif()

if (DOUBLE_PRECISION OR GODOTCPP_PRECISION STREQUAL "double")
	# PUBLIC: the default in CompilerOptions is a header-inline value, so every
	# translation unit that includes compiler.h has to agree on it.
	target_compile_definitions(gdscript_compiler PUBLIC DOUBLE_PRECISION_REAL_T)
endif()

# Build demo executable
add_executable(gdscript_compiler_test test_compiler.cpp)
target_compile_options(gdscript_compiler_test PRIVATE ${GDSC_WARNINGS})
target_link_libraries(gdscript_compiler_test gdscript_compiler)

function(add_gdscript_test test_name test_source)
	add_executable(${test_name} ${test_source})
	target_compile_options(${test_name} PUBLIC -fsanitize=address,undefined)
	target_compile_options(${test_name} PRIVATE ${GDSC_WARNINGS})
	target_link_libraries(${test_name} -fsanitize=address,undefined gdscript_compiler)
	add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# Dump IR utility
add_executable(dump_ir dump_ir.cpp)
target_compile_options(dump_ir PRIVATE ${GDSC_WARNINGS})
if(APPLE)
	target_link_libraries(dump_ir gdscript_compiler)
else()
	target_link_libraries(dump_ir -static gdscript_compiler)
endif()

# Compile-speed harness, host only: the cross build that puts this library
# inside gdscript.elf does not use either and pays for both.
# bench_phases needs a Release directory -- an -O0 build reports timings ~20x off.
if (NOT CMAKE_CROSSCOMPILING)
	add_executable(bench_phases tests/bench/bench_phases.cpp)
	target_compile_options(bench_phases PRIVATE ${GDSC_WARNINGS})
	target_link_libraries(bench_phases gdscript_compiler)

	add_executable(corpus_hash tests/bench/corpus_hash.cpp)
	target_compile_options(corpus_hash PRIVATE ${GDSC_WARNINGS})
	target_link_libraries(corpus_hash gdscript_compiler)
endif()

# GDScript to RISC-V disassembly tool
add_executable(gdscript_to_riscv gdscript_to_riscv.cpp)
target_compile_options(gdscript_to_riscv PRIVATE ${GDSC_WARNINGS})
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
	add_gdscript_test(test_operators tests/test_operators.cpp)
	add_gdscript_test(test_frames tests/test_frames.cpp)
	add_gdscript_test(test_call_overhead tests/test_call_overhead.cpp)
	add_gdscript_test(test_structs tests/test_structs.cpp)
	add_gdscript_test(test_globals tests/test_globals.cpp)
	add_gdscript_test(test_switch tests/test_switch.cpp)
	add_gdscript_test(test_scope tests/test_scope.cpp)
	add_gdscript_test(test_match tests/test_match.cpp)
	add_gdscript_test(test_containers tests/test_containers.cpp)
	add_gdscript_test(test_strings tests/test_strings.cpp)
	add_gdscript_test(test_load tests/test_load.cpp)
	add_gdscript_test(test_lvalues tests/test_lvalues.cpp)
	add_gdscript_test(test_constructors tests/test_constructors.cpp)
	add_gdscript_test(test_syntax tests/test_syntax.cpp)
	add_gdscript_test(test_inline_suites tests/test_inline_suites.cpp)
	add_gdscript_test(test_signatures tests/test_signatures.cpp)
	add_gdscript_test(test_properties tests/test_properties.cpp)
	add_gdscript_test(test_export_hints tests/test_export_hints.cpp)
	add_gdscript_test(test_classes tests/test_classes.cpp)
	add_gdscript_test(test_signals tests/test_signals.cpp)
	add_gdscript_test(test_line_table tests/test_line_table.cpp)
	add_gdscript_test(test_diagnostics tests/test_diagnostics.cpp)
	add_gdscript_test(test_opt_invariance tests/test_opt_invariance.cpp)
	add_gdscript_test(test_ir_verifier tests/test_ir_verifier.cpp)
	add_gdscript_test(test_encoders tests/test_encoders.cpp)
	add_gdscript_test(test_gdsmeta tests/test_gdsmeta.cpp)
	add_gdscript_test(test_fuzz tests/test_fuzz.cpp)

	# Differential testing: the same programs through the IR interpreter and
	# through a real RISC-V machine. libriscv is already a dependency of the
	# repository this library lives in; when it is not reachable (a standalone
	# checkout, or the cross build that runs the compiler inside the sandbox)
	# the rest of the tests still build.
	set(LIBRISCV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../ext/libriscv/lib)
	if (NOT CMAKE_CROSSCOMPILING AND EXISTS ${LIBRISCV_DIR}/CMakeLists.txt)
		if (NOT TARGET riscv)
			set(RISCV_32I OFF CACHE BOOL "" FORCE)
			set(RISCV_BINARY_TRANSLATION OFF CACHE BOOL "" FORCE)
			add_subdirectory(${LIBRISCV_DIR} libriscv)
			# The Godot Sandbox API numbers its syscalls from 500, past
			# libriscv's default table size.
			target_compile_definitions(riscv PUBLIC RISCV_SYSCALLS_MAX=600)
		endif()
		# No sanitizers on this one: it links libriscv, which is built without
		# them, and whose interpreter dispatch ASan has opinions about.
		add_executable(test_differential tests/test_differential.cpp)
		target_link_libraries(test_differential gdscript_compiler riscv)
		add_test(NAME test_differential COMMAND test_differential)

		# Same reason for no sanitizers, and the same need for a real machine:
		# the profiling accounting is only checkable by running the program.
		add_executable(test_profiling tests/test_profiling.cpp)
		target_link_libraries(test_profiling gdscript_compiler riscv)
		add_test(NAME test_profiling COMMAND test_profiling)

		# The shadow stack is only checkable by running a program and reading
		# the area back out of the machine it ran on.
		add_executable(test_debug_info tests/test_debug_info.cpp)
		target_link_libraries(test_debug_info gdscript_compiler riscv)
		add_test(NAME test_debug_info COMMAND test_debug_info)

		# So is a breakpoint: whether it stopped is a fact about a run.
		add_executable(test_breakpoints tests/test_breakpoints.cpp)
		target_link_libraries(test_breakpoints gdscript_compiler riscv)
		add_test(NAME test_breakpoints COMMAND test_breakpoints)

		# And so is a suspension: what `await` compiles to is only really pinned
		# by running it against a host that answers the two syscalls.
		add_executable(test_await tests/test_await.cpp)
		target_link_libraries(test_await gdscript_compiler riscv)
		add_test(NAME test_await COMMAND test_await)

		# Two instance records only differ when a machine runs them: the
		# addressing is a fact about what tp held on each call.
		add_executable(test_instances tests/test_instances.cpp)
		target_link_libraries(test_instances gdscript_compiler riscv)
		add_test(NAME test_instances COMMAND test_instances)

		# A Callable is the same kind of fact: the capture contract is that the
		# host prepends what ECALL_CALLABLE_CREATE was given, which only a run
		# against a host answering that syscall can pin down.
		add_executable(test_callables tests/test_callables.cpp)
		target_link_libraries(test_callables gdscript_compiler riscv)
		add_test(NAME test_callables COMMAND test_callables)

		# Which globals are known to name an object, and where the retain that
		# keeps a RefCounted alive between calls is emitted.
		add_executable(test_object_retain tests/test_object_retain.cpp)
		target_link_libraries(test_object_retain gdscript_compiler riscv)
		add_test(NAME test_object_retain COMMAND test_object_retain)
	else()
		message(STATUS "libriscv not found: skipping the differential test")
	endif()

	# Run all tests target
	add_custom_target(run_tests
		COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
		DEPENDS test_lexer test_parser test_codegen test_call_overhead test_integration test_primitives test_compilation test_constant_pool test_ir_optimizer test_double_precision test_regressions test_operators test_frames test_globals test_opt_invariance test_ir_verifier test_encoders test_fuzz test_match test_containers test_strings test_lvalues test_signatures test_properties
		COMMENT "Running all GDScript compiler tests"
	)

	# Install test targets (only when BUILD_TESTING is ON)
	install(TARGETS test_lexer test_parser test_codegen test_call_overhead test_integration test_primitives test_compilation test_constant_pool test_ir_optimizer test_double_precision test_regressions test_operators test_frames test_globals test_opt_invariance test_ir_verifier test_encoders test_fuzz test_match test_containers test_strings test_lvalues test_signatures test_properties
	        RUNTIME DESTINATION bin)
endif()

# Install targets (always built)
install(TARGETS gdscript_compiler gdscript_compiler_test
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
