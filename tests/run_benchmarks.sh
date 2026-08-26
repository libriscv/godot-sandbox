#!/bin/bash
# Benchmark suite. JIT and interpreter need separate Godot runs (libriscv caches
# the execution mode per binary for the lifetime of the process).
#
#   ./run_benchmarks.sh                    both modes, then the tables
#   ./run_benchmarks.sh --jit              the JIT run only
#   ./run_benchmarks.sh --no-jit           the interpreter run only
#   ./run_benchmarks.sh --report           re-print the tables from saved results
#   ./run_benchmarks.sh --repeat 5         runs of the suite per mode (default 3)
#   ./run_benchmarks.sh --verbose          stream each run instead of logging it
#   ./run_benchmarks.sh --samples 20       rounds per case (default 12)
#   ./run_benchmarks.sh --cpu 0-7          which cores to run on
#   ./run_benchmarks.sh --no-pin           do not pin to cores at all
#   ./run_benchmarks.sh -gselect=cpu       one bench file
#   ./run_benchmarks.sh --save-baseline    keep each run's result as the baseline
#                                          its next run compares against
set -e

if [ -z "$GODOT" ]; then
	GODOT=~/Godot_v4.6.3-stable_linux.x86_64
fi

export CXX="${CXX:-riscv64-linux-gnu-g++-14}"

# Default to cores sharing L3 with cpu0 to avoid cross-die migration.
PIN_CPUS=""
NO_PIN=0
if [ -r /sys/devices/system/cpu/cpu0/cache/index3/shared_cpu_list ]; then
	PIN_CPUS=$(cat /sys/devices/system/cpu/cpu0/cache/index3/shared_cpu_list)
fi

# Runs of the suite per mode. One process measures its own layout as much as the
# code -- where the loader and the allocator put things moves some kernels by
# more than ten percent, and no number of samples inside that process can see it
# -- so each mode is run more than once and bench_report.py pools the samples.
REPEAT=3

VERBOSE=0
SAVE_BASELINE=0
REPORT_ONLY=0
RUN_JIT=1
RUN_NOJIT=1
ARGS=()
# A flag that takes a value, passed last with nothing after it, would otherwise
# shift its own value off the end and leave the variable empty -- which under
# `set -e` aborts somewhere far from the mistake, or silently changes the run.
need_value() {
	if [ $# -lt 2 ]; then
		echo "run_benchmarks.sh: $1 needs a value" >&2
		exit 1
	fi
}
while [ $# -gt 0 ]; do
	case "$1" in
		--save-baseline)
			SAVE_BASELINE=1
			;;
		--no-jit)
			RUN_JIT=0
			;;
		--jit|--jit-only)
			RUN_NOJIT=0
			;;
		--report|--report-only)
			REPORT_ONLY=1
			;;
		--cpu)
			need_value "$@"
			shift
			PIN_CPUS="$1"
			;;
		--no-pin)
			PIN_CPUS=""
			NO_PIN=1
			;;
		--repeat)
			need_value "$@"
			shift
			REPEAT="$1"
			;;
		--verbose)
			VERBOSE=1
			;;
		--samples)
			need_value "$@"
			shift
			export GDSC_BENCH_SAMPLES="$1"
			;;
		--min-sample-ms)
			# How long one sample must last. Shorter samples read the timer and
			# the scheduler as much as the code.
			need_value "$@"
			shift
			export GDSC_BENCH_MIN_SAMPLE_US=$(( $1 * 1000 ))
			;;
		*)
			ARGS+=("$1")
			;;
	esac
	shift
done

report() {
	python3 bench/bench_report.py --results-dir "$PWD/bench/results"
}

if [ "$REPORT_ONLY" = "1" ]; then
	report
	exit 0
fi

# The benchmarks compile GDScript with the compiler inside tests.elf, the same
# binary the unit tests use.
mkdir -p .build
pushd .build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake -DSTRIPPED=OFF -DFLTO=ON
make -j4
popd
ln -fs ../.build/unittests tests/tests.elf

# One run of the suite. $1 is the guest execution mode.
run_suite() {
	local mode="$1"
	local suffix=""
	local pin=()
	if [ "$NO_PIN" = "0" ] && [ -n "$PIN_CPUS" ] && command -v taskset > /dev/null; then
		pin=(taskset -c "$PIN_CPUS")
		export GDSC_BENCH_CPUS="$PIN_CPUS"
	else
		unset GDSC_BENCH_CPUS
	fi
	if [ "$REPEAT" -gt 1 ] && [ "$VERBOSE" != "1" ]; then
		export GDSC_BENCH_QUIET=1
	else
		unset GDSC_BENCH_QUIET
	fi
	if [ "$mode" = "nojit" ]; then
		export GDSC_BENCH_NO_JIT=1
		suffix="-nojit"
	else
		unset GDSC_BENCH_NO_JIT
	fi
	# The harness merges the bench files of one process into this file, so it has
	# to start empty or a filtered run pools groups it did not measure.
	rm -f "bench/results/latest$suffix.json"

	local log="bench/results/runs/$mode-$2.log"
	printf '%s' "run ${2}/${REPEAT} ${mode}${pin:+ on cores $PIN_CPUS} ... "
	if [ "$VERBOSE" = "1" ]; then
		echo ""
		# tee's status is the pipeline's, so Godot's has to be read out of
		# PIPESTATUS -- otherwise a crashed run is only noticed by the grep below,
		# and only if it happened to print one of the strings it looks for.
		"${pin[@]}" $GODOT --path "$PWD" --headless -s addons/gut/gut_cmdln.gd -gconfig=res://.gutconfig.bench.json "${ARGS[@]}" 2>&1 | tee "$log"
		local status=${PIPESTATUS[0]}
		if [ "$status" -ne 0 ]; then
			echo "failed (exit $status)"
			exit 1
		fi
	elif ! "${pin[@]}" $GODOT --path "$PWD" --headless -s addons/gut/gut_cmdln.gd -gconfig=res://.gutconfig.bench.json "${ARGS[@]}" > "$log" 2>&1; then
		echo "failed"
		tail -40 "$log"
		exit 1
	fi
	# GUT reports a failed assert in its summary rather than in its exit status,
	# and a benchmark whose two implementations disagree is not a measurement.
	# 'Failing Tests' and 'Errors' are printed only when they are non-zero.
	if grep -qE "Failing Tests|^Errors|SCRIPT ERROR" "$log"; then
		echo "failed"
		tail -40 "$log"
		exit 1
	fi
	if [ "$VERBOSE" != "1" ]; then
		echo "ok ($(grep -oP '(?<=^Time)\s+\S+' "$log" | tail -1 | tr -d ' '))"
	fi
	cp "bench/results/latest$suffix.json" "bench/results/runs/$mode-$2.json"
	if [ "$SAVE_BASELINE" = "1" ]; then
		cp "bench/results/latest$suffix.json" "bench/baseline$suffix.json"
		echo "baseline saved to bench/baseline$suffix.json"
	fi
}

# Only the modes being run are cleared: a --no-jit run keeps the JIT run it is
# meant to be compared against.
mkdir -p bench/results/runs
for mode in jit nojit; do
	if [ "$mode" = "jit" ] && [ "$RUN_JIT" = "0" ]; then continue; fi
	if [ "$mode" = "nojit" ] && [ "$RUN_NOJIT" = "0" ]; then continue; fi
	rm -f bench/results/runs/$mode-*
	for i in $(seq 1 "$REPEAT"); do
		run_suite "$mode" "$i"
	done
done

# The tables the suite exists to produce, joined across both runs.
report
