usage() {
	echo "Usage: $0 [--api] [-o output] -- [input...]"
	echo "  --api        Instead of compiling, copy the API files to the output."
	echo "  --line addr  Convert an address to a line number."
	echo "   -o output   Compile sources into an output ELF file, including API and inputs."
	echo "  --local      Compile as if locally (outside Docker), using the local API files."
	echo "   -v          Verbose output."
	echo "  --           Separate options from input files."
	exit 1
}

locally=false
verbose=false
current_version=2
CPPFLAGS="-g -O2 -std=gnu++23 -DVERSION=$current_version"

while [[ "$#" -gt 0 ]]; do
	case $1 in
		--api) cp -r /usr/api $1; exit ;;
		--line) shift; addr="$1"; shift; output="$1"; shift; addr2line -e $output $addr; exit ;;
		-o) shift; output="$1"; shift; break ;;
		--local) locally=true; shift ;;
		--version) shift; echo "$current_version"; exit ;;
		-v) verbose=true; shift ;;
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
LINKEROPS="$MEMOPS $STROPS $HEAPOPS"

if [ "$locally" = true ]; then
	API="api"
else
	API="/usr/api"
fi

# For each C++ file in *.cpp and api/*.cpp, compile it into a .o file asynchronously
for file in $@ $API/*.cpp; do
	if [ verbose = true ]; then
		echo "Compiling $file"
	fi
	if [ "$locally" = true ]; then
		riscv64-unknown-elf-g++ $CPPFLAGS -I$API -c $file -o $file.o &
	else
		riscv64-linux-gnu-g++-14 $CPPFLAGS -march=rv64gc_zba_zbb_zbs_zbc -mabi=lp64d -I$API -c $file -o $file.o &
	fi
done

# Wait for all files to compile
wait

# Link all .o files into the output
if [ "$locally" = true ]; then
	riscv64-unknown-elf-g++ -static $CPPFLAGS $LINKEROPS $@.o $API/*.cpp.o -o $output
else
	LINKEROPS="$LINKEROPS -fuse-ld=mold -Wl,--execute-only"
	riscv64-linux-gnu-g++-14 -static $CPPFLAGS -march=rv64gc_zba_zbb_zbs_zbc -mabi=lp64d $LINKEROPS $@.o $API/*.cpp.o -o $output
fi
