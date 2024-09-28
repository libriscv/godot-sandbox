set -e

GODOT=~/Godot_v4.3-stable_linux.x86_64

#$GODOT --path "$PWD" --headless --import
#$GODOT --path "$PWD" --headless --import

export CXX="riscv64-linux-gnu-g++-12"

mkdir -p .build
pushd .build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake
make -j4
popd

cp .build/unittests tests/tests.elf

$GODOT --path "$PWD" --headless -s addons/gut/gut_cmdln.gd
