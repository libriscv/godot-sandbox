set -e
mkdir -p .build_mingw
pushd .build_mingw
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=../mingw_toolchain.cmake -DGODOT_DISABLE_EXCEPTIONS=OFF -DGODOTCPP_DISABLE_EXCEPTIONS=OFF -DGODOTCPP_TARGET=template_release
make -j16
popd

mv .build_mingw/libgodot-riscv.dll .build_mingw/libgodot_riscv.windows.template_release.x86_64.dll
cp .build_mingw/*.dll /srv/samba/share
cp .build_mingw/libgodot_riscv.windows.template_release.x86_64.dll ~/New\ Game\ Project/extensions/bin/libgodot_riscv.windows.template_release.x86_64.dll
