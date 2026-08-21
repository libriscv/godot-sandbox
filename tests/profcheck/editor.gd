extends SceneTree

var server := TCPServer.new()
var stream: StreamPeerTCP = null
var pps := PacketPeerStream.new()
var sent := false
var ticks := 0
var counts := {}

func _initialize():
	var err = server.listen(6017, "127.0.0.1")
	print("[dbg] listen err=", err)

func _process(_d):
	ticks += 1
	if stream == null:
		if server.is_connection_available():
			stream = server.take_connection()
			pps.stream_peer = stream
			print("[dbg] connected")
		return ticks > 3000
	stream.poll()
	if not sent:
		sent = true
		var opts := [64, false]
		pps.put_var(["profiler:scripts", 1, [true, opts]])
		pps.put_var(["profiler:servers", 1, [true, opts]])
		print("[dbg] profilers enabled")
	while pps.get_available_packet_count() > 0:
		var v = pps.get_var()
		if typeof(v) != TYPE_ARRAY or v.size() < 2:
			continue
		var name = str(v[0])
		var payload = v[v.size() - 1]
		counts[name] = counts.get(name, 0) + 1
		if name == "servers:function_signature":
			print("[dbg] SIGNATURE ", payload)
		elif name == "servers:profile_frame":
			var d = payload
			if counts[name] <= 3 or counts[name] % 60 == 0:
				print("[dbg] profile_frame #", counts[name], " -> ", d)
	if ticks > 3000 or (stream.get_status() != StreamPeerTCP.STATUS_CONNECTED and ticks > 60):
		print("[dbg] message counts: ", counts)
		return true
	return false
