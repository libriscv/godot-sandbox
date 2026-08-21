#!/bin/bash
# The benchmark suite. Not part of CI, and not run by run_unittests.sh: GUT's
# default config points at res://tests/, this one at res://bench/.
#
#   ./run_benchmarks.sh                    run everything
#   ./run_benchmarks.sh -gselect=cpu       run one bench file
#   ./run_benchmarks.sh --no-jit           run the guest in the interpreter
#   ./run_benchmarks.sh --save-baseline    run, then keep the result as the
#                                          baseline the next run compares against
set -e

if [ -z "$GODOT" ]; then
	GODOT=~/Godot_v4.6.3-stable_linux.x86_64
fi

export CXX="${CXX:-riscv64-linux-gnu-g++-14}"

SAVE_BASELINE=0
SUFFIX=""
ARGS=()
for arg in "$@"; do
	case "$arg" in
		--save-baseline)
			SAVE_BASELINE=1
			;;
		--no-jit)
			# libriscv caches a translated execute segment per binary, so the
			# interpreter is a property of the whole run, not of one sandbox. Its
			# results and its baseline are kept apart from the JIT ones.
			export GDSC_BENCH_NO_JIT=1
			SUFFIX="-nojit"
			;;
		*)
			ARGS+=("$arg")
			;;
	esac
done

# The benchmarks compile GDScript with the compiler inside tests.elf, the same
# binary the unit tests use.
mkdir -p .build
pushd .build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake -DSTRIPPED=OFF -DFLTO=ON
make -j4
popd
ln -fs ../.build/unittests tests/tests.elf

$GODOT --path "$PWD" --headless -s addons/gut/gut_cmdln.gd -gconfig=res://.gutconfig.bench.json "${ARGS[@]}"

if [ "$SAVE_BASELINE" = "1" ]; then
	cp "bench/results/latest$SUFFIX.json" "bench/baseline$SUFFIX.json"
	echo "baseline saved to bench/baseline$SUFFIX.json"
fi
