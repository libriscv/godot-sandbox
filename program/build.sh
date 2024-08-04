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
		-o) shift; output=$1; shift ;;
		--) shift; break ;;
		*) usage ;;
	esac
done

if [ -z "$output" ]; then
	usage
fi

riscv64-linux-gnu-g++-14 -static -O2 -std=gnu++23 -I/usr/api -fuse-ld=mold -Wl,--execute-only /usr/api/*.cpp -o $output $@
