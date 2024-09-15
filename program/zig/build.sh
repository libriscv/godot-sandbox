#!/bin/bash
set -e

usage() {
	echo "Usage: $0 [--api] [-o output] -- [input...]"
	echo "  --api        Instead of compiling, copy the API files to the output."
	echo "   -o output   Compile sources into an output ELF file, including API and inputs."
	echo "  --local      Compile as if locally (outside Docker), using the local API files."
	echo "  --version    Print the current version of the API."
	echo "   -v          Verbose output."
	echo "  --           Separate options from input files."
	exit 1
}

locally=false
verbose=false
current_version=1

while [[ "$#" -gt 0 ]]; do
	case $1 in
		--api) cp -r /usr/api $1; exit ;;
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

DIR="$(dirname "${output}")"
FILE="$(basename "${output}")"
time /usr/zig/zig build-exe -O ReleaseSmall -fno-strip -flld -target riscv64-linux $@ --name $FILE
mv $FILE $output
rm -f $FILE.o
