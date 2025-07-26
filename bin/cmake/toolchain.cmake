
if (NOT DEFINED ZIG_PATH)
	set(ZIG_PATH "zig")
endif()
set(CMAKE_SYSTEM_NAME "Linux")
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR "riscv64")
set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER "${ZIG_PATH}" cc -target riscv64-linux-musl)
set(CMAKE_CXX_COMPILER "${ZIG_PATH}" c++ -target riscv64-linux-musl)

if (CMAKE_HOST_WIN32)
	# Windows: Disable .d files
	set(CMAKE_C_LINKER_DEPFILE_SUPPORTED FALSE)
	set(CMAKE_CXX_LINKER_DEPFILE_SUPPORTED FALSE)
	# Windows: Work-around for zig ar and zig ranlib
	set(CMAKE_AR "${CMAKE_CURRENT_LIST_DIR}/zig-ar.cmd")
	set(CMAKE_RANLIB "${CMAKE_CURRENT_LIST_DIR}/zig-ranlib.cmd")
endif()
