#!/bin/bash
# Soak Sandbox.assault() against a sanitizer-instrumented build of the addon.
#
# A plain soak only reports the crashes that actually take the process down. The system
# call handlers write into guest memory and into Godot's own containers at offsets the
# guest chose, and an overrun there lands in some other live allocation far more often
# than it lands unmapped -- so the run that matters is the instrumented one.
#
#   tests/fuzz_sanitize.sh ubsan [iterations] [extra fuzz_soak args...]
#   tests/fuzz_sanitize.sh asan  [iterations]
#
# The addon is loaded by a Godot binary nobody built with a sanitizer, and GCC leaves the
# sanitizer runtime undefined in a -shared object, so both modes preload it. UBSAN's build
# stays optimized and its runtime costs almost nothing, which is why it is the first to
# reach for; ASAN is the one that sees a write land in the wrong live allocation.
set -e

MODE="${1:-ubsan}"
ITERATIONS="${2:-200000}"
shift 2 2>/dev/null || shift $# 
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GODOT="${GODOT:-$HOME/Godot_v4.6.3-stable_linux.x86_64}"
LINK="$ROOT/tests/extensions/bin/libgodot-riscv.linux.release.x86_64.so"
DEBUG_LINK="$ROOT/tests/extensions/bin/libgodot-riscv.linux.debug.x86_64.so"

case "$MODE" in
	ubsan)
		BUILD="$ROOT/.build_ubsan"
		CMAKE_ARGS="-DCMAKE_BUILD_TYPE=RelWithDebInfo -DSANITIZE=ON"
		# GCC leaves the runtime undefined when it links with -shared, on the assumption
		# that the executable carries it. Godot does not, so it is preloaded here.
		export LD_PRELOAD="$(${CXX:-c++} -print-file-name=libubsan.so)"
		export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0:report_error_type=1"
		;;
	asan)
		# Debug: anything else adds -fmerge-all-constants, and ASAN aborts on asmjit's
		# globals when constants are merged.
		BUILD="$ROOT/.build_asan"
		CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Debug -DASAN=ON"
		# Godot dlopens the addon with RTLD_DEEPBIND, which the ASAN runtime refuses to
		# run under; the shim clears that one flag. Built here so the run is self-contained.
		SHIM="$BUILD/fuzz_no_deepbind.so"
		mkdir -p "$BUILD"
		cc -shared -fPIC -O2 -o "$SHIM" "$ROOT/tests/fuzz_no_deepbind.c" -ldl
		# The shim goes first: LD_PRELOAD resolves left to right, and ASAN installs its own
		# dlopen interceptor, so anything loaded after it never sees the call.
		# libstdc++ too: godot-cpp links it statically, so the only __cxa_throw in the
		# process would be the copy inside the addon, and ASAN's interceptor refuses to
		# start when it cannot find the real one. Every handler here refuses bad arguments
		# by throwing, so that is the first thing the run would hit.
		export LD_PRELOAD="$SHIM $(${CXX:-c++} -print-file-name=libasan.so) $(${CXX:-c++} -print-file-name=libstdc++.so)"
		# Leaks are Godot's and the guest heap's, and neither is what this is looking for.
		# verify_asan_link_order: the runtime is no longer first in LD_PRELOAD, by design above.
		export ASAN_OPTIONS="detect_leaks=0:detect_odr_violation=0:abort_on_error=0:print_stacktrace=1:verify_asan_link_order=0"
		;;
	*)
		echo "usage: $0 {ubsan|asan} [iterations] [extra args]" >&2
		exit 1
		;;
esac

cmake -S "$ROOT" -B "$BUILD" -G Ninja $CMAKE_ARGS > /dev/null
cmake --build "$BUILD" -j"$(nproc)"

# Point the project at the instrumented build for the duration, and put the ordinary one
# back however this exits -- a left-over sanitizer link makes every later test run confusing.
restore() {
	ln -fs ../../../.build/libgodot-riscv.so "$LINK"
	ln -fs ../../../.build/libgodot-riscv.so "$DEBUG_LINK"
}
trap restore EXIT
ln -fs "$BUILD/libgodot-riscv.so" "$LINK"
ln -fs "$BUILD/libgodot-riscv.so" "$DEBUG_LINK"

echo "== $MODE soak, $ITERATIONS iterations =="
"$GODOT" --path "$ROOT/tests" --headless -s fuzz_soak.gd -- --iterations="$ITERATIONS" "$@"
