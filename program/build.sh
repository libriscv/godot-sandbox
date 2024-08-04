#echo $@
riscv64-linux-gnu-g++-14 -static -O2 -std=gnu++23 -I/usr/api -fuse-ld=mold -Wl,--execute-only /usr/api/*.cpp -o $1 $2 $3
