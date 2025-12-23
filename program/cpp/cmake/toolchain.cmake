SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_CROSSCOMPILING 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

# Use the CXX environment variable if set (from run_unittests.sh)
if (DEFINED ENV{CXX})
	set(CMAKE_CXX_COMPILER $ENV{CXX})
else()
	# Try to find the compiler, preferring -14, then fallback to default
	find_program(CXX_COMPILER
		NAMES riscv64-linux-gnu-g++-14 riscv64-linux-gnu-g++
		PATHS /usr/bin /usr/local/bin
		NO_DEFAULT_PATH
	)
	if (CXX_COMPILER)
		set(CMAKE_CXX_COMPILER ${CXX_COMPILER})
	else()
		# Default to riscv64-linux-gnu-g++-14 if not set
		set(CMAKE_CXX_COMPILER "riscv64-linux-gnu-g++-14")
	endif()
endif()

# Set C compiler to match
if (DEFINED ENV{CC})
	set(CMAKE_C_COMPILER $ENV{CC})
else()
	# Try to find the compiler, preferring -14, then fallback to default
	find_program(C_COMPILER
		NAMES riscv64-linux-gnu-gcc-14 riscv64-linux-gnu-gcc
		PATHS /usr/bin /usr/local/bin
		NO_DEFAULT_PATH
	)
	if (C_COMPILER)
		set(CMAKE_C_COMPILER ${C_COMPILER})
	else()
		set(CMAKE_C_COMPILER "riscv64-linux-gnu-gcc-14")
	endif()
endif()
