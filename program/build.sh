usage() {
	echo "Usage: $0 [--api] [-o output] -- [input...]"
	echo "  --api        Instead of compiling, copy the API files to the output."
	echo "   -o output   Compile sources into an output ELF file, including API and inputs."
	echo "  --           Separate options from input files."
	exit 1
}

while [[ "$#" -gt 0 ]]; do
	case $1 in
		--api) cp -r /usr/api $1; exit ;;
		-o) shift; output=$1; shift; break ;;
		--) shift; break ;;
		*) usage ;;
	esac
done

if [ -z "$output" ]; then
	usage
fi

MEMOPS=-Wl,--wrap=memcpy,--wrap=memset,--wrap=memcmp,--wrap=memmove
STROPS=-Wl,--wrap=strlen,--wrap=strcmp,--wrap=strncmp
HEAPOPS=-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
LINKEROPS="-fuse-ld=mold -Wl,--execute-only $MEMOPS $STROPS $HEAPOPS"

riscv64-linux-gnu-g++-14 -static -O2 -std=gnu++23 -I/usr/api $LINKEROPS /usr/api/*.cpp -o $output $@
