# Stands in for the editor: listens on the remote-debugger port, enables the
# scripts/servers profilers, and fails if .sgd functions never report calls.
extends SceneTree

const PORT := 6017
const RUN_TICKS := 3000

var server := TCPServer.new()
var stream: StreamPeerTCP = null
var pps := PacketPeerStream.new()
var ticks := 0
var started := false
var stopped := false
var signatures := {}   # id -> name
var totals := {}       # id -> call count summed over frames

func _initialize():
	var err := server.listen(PORT, "127.0.0.1")
	if err != OK:
		printerr("[dbg] listen failed: ", err)
		quit(1)

func _put(message: String, data: Array) -> void:
	# [command, thread id, data]; Thread::MAIN_ID is 1.
	pps.put_var([message, 1, data])

func _process(_delta):
	ticks += 1
	if stream == null:
		if server.is_connection_available():
			stream = server.take_connection()
			pps.stream_peer = stream
			print("[dbg] connected")
		elif ticks > RUN_TICKS:
			printerr("[dbg] the game never connected")
			quit(1)
			return true
		return false
	stream.poll()
	if not started:
		started = true
		# [max frame functions, include native calls], as the Profiler panel sends.
		var opts := [64, false]
		_put("profiler:scripts", [true, opts])
		_put("profiler:servers", [true, opts])
	while pps.get_available_packet_count() > 0:
		_read(pps.get_var())
	if not stopped and (_saw_sgd() or ticks > RUN_TICKS - 500):
		# Turning the profiler off makes the engine ask for accumulated data.
		stopped = true
		_put("profiler:scripts", [false, []])
		_put("profiler:servers", [false, []])
	if ticks > RUN_TICKS or (stopped and stream.get_status() != StreamPeerTCP.STATUS_CONNECTED):
		return _report()
	return false

# True once any .sgd function has reported calls.
func _saw_sgd() -> bool:
	for id in totals:
		if totals[id] > 0 and str(signatures.get(id, "")).contains(".sgd::"):
			return true
	return false

func _read(v) -> void:
	if typeof(v) != TYPE_ARRAY or v.size() < 2:
		return
	var name := str(v[0])
	var payload = v[v.size() - 1]
	if name == "servers:function_signature":
		signatures[int(payload[1])] = str(payload[0])
	elif name == "servers:profile_frame":
		_read_functions(payload)

func _read_functions(payload: Array) -> void:
	# The frame ends with a count of the values that follow it, then one
	# (id, calls, self, total, internal) group per function that ran.
	var count := -1
	var at := -1
	for k in range(payload.size() - 1, -1, -1):
		if typeof(payload[k]) == TYPE_INT and payload[k] == payload.size() - 1 - k and payload[k] % 5 == 0:
			count = payload[k]
			at = k + 1
			break
	if count <= 0:
		return
	for i in range(at, at + count, 5):
		var id = payload[i]
		var calls = payload[i + 1]
		if typeof(id) == TYPE_INT and typeof(calls) == TYPE_INT:
			totals[id] = totals.get(id, 0) + calls

func _report() -> bool:
	var wanted := ["res://profcheck/target.sgd::1::busy", "res://profcheck/target.sgd::9::outer"]
	var seen := {}
	for id in signatures:
		seen[signatures[id]] = totals.get(id, 0)
	print("[dbg] profiled functions: ", seen)
	var failed := false
	for w in wanted:
		if not seen.has(w):
			printerr("[dbg] MISSING ", w)
			failed = true
		elif seen[w] <= 0:
			printerr("[dbg] NO CALLS RECORDED for ", w)
			failed = true
	print("[dbg] ", "FAILED" if failed else "OK")
	quit(1 if failed else 0)
	return true
