#!/usr/bin/env bash
set -e

here="$(cd "$(dirname "$0")" && pwd)"
compiler_dir="$(cd "$here/../.." && pwd)"
repo="$(cd "$compiler_dir/../../.." && pwd)"
build="${1:-build-rel}"
mode="${2:-check}"
baseline="$here/baseline_hashes.txt"

cd "$repo"
if [ ! -f "$here/big_cpu.sgd" ]; then
	python3 "$here/make_big.py" "$repo/tests/tests/test_cpu.sgd" > "$here/big_cpu.sgd"
fi
files=("$compiler_dir/tests/bench/big_cpu.sgd")
while IFS= read -r f; do files+=("$f"); done < <(find tests/tests examples -name '*.sgd' | sort)

out="$(mktemp)"
"$compiler_dir/$build/corpus_hash" "${files[@]}" > "$out"

if [ "$mode" = "save" ]; then
	mv "$out" "$baseline"
	echo "baseline saved: $(wc -l < "$baseline") entries"
else
	if diff -u "$baseline" "$out"; then
		echo "output invariant: $(wc -l < "$out") entries identical"
		rm -f "$out"
	else
		echo "OUTPUT CHANGED"
		rm -f "$out"
		exit 1
	fi
fi
