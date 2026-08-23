#!/bin/bash
# The .sgd side of the editor's debugger, which needs an engine debugger to
# exist at all. -d is the local stdout one: it makes EngineDebugger active, so
# the breakpoint set is there to mirror, and it prints the stop -- which is the
# only place the stack-level virtuals can be seen from outside the engine.
#
# Feeds "bt" then "c" at every stop: the backtrace is what shows the outer
# frames, and the continue is what lets the guest go. Without either the
# debugger waits on stdin forever.
set -e
cd "$(dirname "$0")"
if [ -z "$GODOT" ]; then
	GODOT=~/Godot_v4.6.3-stable_linux.x86_64
fi

OUTPUT=$( (while :; do printf 'bt\nc\n'; sleep 0.05; done) \
	| timeout 300 "$GODOT" --path "$PWD" --headless -d -s debugger_test.gd 2>&1) || true
# The debugger's prompt carries no newline, so a line of ours can start with it.
echo "$OUTPUT" | grep -E '\[dbg\]|Debugger Break|Frame [0-9]' || true

status=0
expect() {
	if ! echo "$OUTPUT" | grep -qF "$1"; then
		echo "MISSING: $1"
		status=1
	fi
}
# The stop reached the engine's debugger, and the stack it drew came from the
# shadow stack: the inner frame is the line that broke, the outer one the call
# it is sitting in.
expect "Debugger Break"
expect "Frame 0 - res://tests/test_debugger.sgd:9 in function 'alu'"
expect "Frame 1 - res://tests/test_debugger.sgd:15 in function 'outer'"
expect "[dbg] 0 failure(s)"
if echo "$OUTPUT" | grep -q "\[dbg\] FAILED"; then
	status=1
fi

if [ $status -eq 0 ]; then
	echo "debugger test: PASSED"
else
	echo "debugger test: FAILED"
fi
exit $status
