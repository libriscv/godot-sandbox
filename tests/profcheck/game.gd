# The game half of the profiler check: a node running target.sgd, its Sandbox
# instrumented from frame 20 on, calling into the guest every frame so the
# records have something in them.
extends SceneTree

var node: Node = null
var frames := 0

func _initialize():
	node = Node.new()
	node.set_script(load("res://profcheck/target.sgd"))
	root.add_child(node)
	print("[game] busy(10) = ", node.call("busy", 10))

func _process(_delta):
	frames += 1
	if frames == 20:
		# Toggling recompiles with instrumentation and reloads.
		node.set("profiling", true)
		print("[game] profiling = ", node.get("profiling"))
	if frames > 20:
		for i in 20:
			node.call("busy", 300)
			node.call("outer", 100)
	return frames > 240
