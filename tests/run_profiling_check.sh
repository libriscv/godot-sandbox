#!/bin/bash
# Drives the Script Functions profiler over a real remote-debugger connection:
# profcheck/debugger.gd stands in for the editor, profcheck/game.gd runs an
# instrumented .sgd. Exits non-zero when the .sgd functions do not reach the
# profiler. Run from the tests folder, after building the addon.
set -u
GODOT=${GODOT:-~/Godot_v4.6.3-stable_linux.x86_64}
HERE=$(cd "$(dirname "$0")" && pwd)
LOG=$(mktemp -d)

"$GODOT" --path "$HERE" --headless -s profcheck/debugger.gd > "$LOG/debugger.log" 2>&1 &
DBG=$!
sleep 3
"$GODOT" --path "$HERE" --headless --remote-debug tcp://127.0.0.1:6017 -s profcheck/game.gd > "$LOG/game.log" 2>&1
GAME=$?
wait $DBG
DBG_STATUS=$?

grep -E "^\[game\]" "$LOG/game.log"
grep -E "^\[dbg\]" "$LOG/debugger.log"
if [ $GAME -ne 0 ]; then
	echo "FAILED: the game exited with $GAME (see $LOG/game.log)"
	exit 1
fi
if [ $DBG_STATUS -ne 0 ]; then
	echo "FAILED: the profiler never saw the .sgd functions (see $LOG/debugger.log)"
	exit 1
fi
rm -rf "$LOG"
echo "Profiler integration OK"
