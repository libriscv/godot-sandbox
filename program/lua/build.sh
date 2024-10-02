#!/bin/bash

# Exit on error
set -e

CC=riscv64-linux-gnu-gcc-12
CXX=riscv64-linux-gnu-g++-12

mkdir -p .build
pushd .build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake
ninja
popd

