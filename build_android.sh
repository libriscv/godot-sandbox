CC=aarch64-linux-gnu-gcc-14
CXX=aarch64-linux-gnu-g++-14

mkdir -p .build_android
pushd .build_android
cmake .. -DCMAKE_BUILD_TYPE=Release -DRISCV_LIBTCC=ON -DANDROID_TOOLCHAIN=ON -DGODOT_DISABLE_EXCEPTIONS=OFF -DGODOTCPP_DISABLE_EXCEPTIONS=OFF -DGODOTCPP_TARGET=template_release
make -j16
popd

mv .build_android/libgodot-riscv.so .build_android/libgodot_riscv.android.template_release.arm64.so
