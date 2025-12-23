SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_CROSSCOMPILING 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

# Use the CXX environment variable if set (from run_unittests.sh)
if (DEFINED ENV{CXX})
	set(CMAKE_CXX_COMPILER $ENV{CXX})
else()
	# Default to riscv64-linux-gnu-g++-14 if not set
	set(CMAKE_CXX_COMPILER "riscv64-linux-gnu-g++-14")
endif()

# Set C compiler to match
if (DEFINED ENV{CC})
	set(CMAKE_C_COMPILER $ENV{CC})
else()
	set(CMAKE_C_COMPILER "riscv64-linux-gnu-gcc-14")
endif()
