#!/bin/bash
# Nightly fuzzing.
#
# The test suite runs a fixed seed corpus on every commit, which is fast and
# deterministic. This runs the same generators for much longer, from seeds
# nobody has tried, which is where the programs nobody thought to write come
# from. It is meant for a nightly job rather than a commit hook.
#
# Any failure prints the seed it came from, and re-running the named binary with
# that seed reproduces it exactly:
#
#   ./test_fuzz --seed <n> --count 1
#   ./test_differential --fuzz --seed <n> --count 1
#
# Usage: tests/fuzz_nightly.sh [build-dir] [minutes]
set -u

BUILD=${1:-build}
MINUTES=${2:-30}
SECONDS_PER_STAGE=$(( MINUTES * 60 / 2 ))

# A seed nobody has used before, derived from the clock. Printed, so a failure
# from this run can be reproduced later.
BASE_SEED=$(date +%s)
echo "Nightly fuzz: base seed ${BASE_SEED}, ${MINUTES} minutes total"

status=0

echo
echo "=== Verifier and optimization invariance ==="
timeout "${SECONDS_PER_STAGE}" "${BUILD}/test_fuzz" --seed "${BASE_SEED}" --count 1000000
case $? in
	0)   echo "(completed the whole count)" ;;
	124) echo "(stopped at the time limit, nothing found)" ;;
	*)   echo "FAILURES above"; status=1 ;;
esac

if [ -x "${BUILD}/test_differential" ]; then
	echo
	echo "=== Interpreter against a real RISC-V machine ==="
	timeout "${SECONDS_PER_STAGE}" "${BUILD}/test_differential" --fuzz \
		--seed "$(( BASE_SEED + 1000000 ))" --count 1000000
	case $? in
		0)   echo "(completed the whole count)" ;;
		124) echo "(stopped at the time limit, nothing found)" ;;
		*)   echo "FAILURES above"; status=1 ;;
	esac
else
	echo
	echo "test_differential was not built (libriscv not found): skipping"
fi

exit $status
