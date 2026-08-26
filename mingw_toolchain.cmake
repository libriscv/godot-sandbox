# Cross-compile the addon for Windows with MinGW-w64.
#
# CMAKE_SYSTEM_NAME earns its place here beyond picking the compiler prefix:
# setting CC/CXX alone leaves CMake describing a Linux host while the compiler
# emits PE objects, so its ABI probe cannot size a pointer. CMAKE_SIZEOF_VOID_P
# comes back empty, godot-cpp's 32/64-bit switch (math(EXPR BITS ...)) fails the
# configure outright, and asmjit configures its POSIX virtual-memory backend
# (mmap/shm_open) for a target that only has VirtualAlloc.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(MINGW_TOOLCHAIN ON)

# The win32 threading model: GCC 13's libstdc++ implements std::thread on it,
# and it keeps winpthreads out of the DLL.
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc-win32)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++-win32)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
