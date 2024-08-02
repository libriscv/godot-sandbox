echo $@
riscv64-linux-gnu-g++-13 -static -O2 -std=gnu++23 -I/usr/api /usr/api/*.cpp *.cpp -o $1
