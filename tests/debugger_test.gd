# The editor's breakpoint gutter, end to end, which GUT cannot host: the engine
# debugger only exists when Godot was started under one, and every
# EngineDebugger call reports an error when it was not. run_debugger_test.sh
# starts it with -d, the local stdout debugger, and greps what the stop printed
# -- the frames below are drawn by SafeGDScriptLanguage's stack-level virtuals,
# so a broken one shows up there and not here.
#
# What this file checks is the other direction: the set the editor holds,
# mirrored into a script that has to recompile to carry it.
extends SceneTree

const PATH := "res://tests/test_debugger.sgd"
const BREAK_LINE := 9   # "return a + b", inside the OP_ADD arm
const OTHER_LINE := 11  # "return a - b", the line the project sets itself

var script_res: SafeGDScript
var mod_script: SafeGDScript
var mod_node: Node
var node: Node
var step := 0
var frames := 0
var failures := 0

func _initialize() -> void:
	if not EngineDebugger.is_active():
		_fail("no engine debugger: run this with -d")
		quit(1)
		return
	script_res = load(PATH)
	node = Node.new()
	node.set_script(script_res)
	root.add_child(node)
	if not node.has_method("alu"):
		_fail("the fixture did not compile")
		quit(1)
		return
	# A breakpoint the project set for itself. The editor knows nothing about
	# it, and mirroring the editor's set must not take it away.
	script_res.set_breakpoint(OTHER_LINE, true)
	_check(script_res.get_breakpoints() == PackedInt32Array([OTHER_LINE]),
		"set_breakpoint installed %d" % OTHER_LINE)

# One step per poll interval; the mirror runs from the language's _frame().
func _process(_delta: float) -> bool:
	frames += 1
	if frames % 12 != 0:
		return false
	step += 1
	match step:
		1:
			EngineDebugger.insert_breakpoint(BREAK_LINE, PATH)
		2:
			_check(script_res.get_breakpoints() == PackedInt32Array([BREAK_LINE, OTHER_LINE]),
				"the editor's line was added beside the project's")
			_check(script_res.get_active_breakpoints() == PackedInt32Array([BREAK_LINE, OTHER_LINE]),
				"both lines were compiled in")
			_check(script_res.is_debug_build(), "a breakpoint implies the debug build")
			# The stop itself: what it printed is checked by the shell script.
			print("[dbg] calling outer(2, 3), expecting a stop at line %d" % BREAK_LINE)
			_check(node.call("outer", 2, 3) == 5, "the guest resumed and returned 5")
		3:
			EngineDebugger.remove_breakpoint(BREAK_LINE, PATH)
		4:
			_check(script_res.get_breakpoints() == PackedInt32Array([OTHER_LINE]),
				"clearing the editor's line left the project's alone")
			script_res.set_breakpoint(OTHER_LINE, false)
			_check(script_res.get_breakpoints().is_empty(), "and the project can clear its own")
		5:
			_check(node.call("outer", 2, 3) == 5, "a program with no breakpoints runs on")
			EngineDebugger.insert_breakpoint(OTHER_LINE, PATH)
		6:
			# A mod loader's shape: source read at run time, so the script was
			# never loaded from a file and has no path of its own. Naming the
			# file it came from is what puts it back under the gutter -- and it
			# has to be named before the source, because setting the source is
			# what compiles.
			mod_script = SafeGDScript.new()
			mod_script.take_over_path(PATH)
			mod_script.set_source_code(FileAccess.get_file_as_string(PATH))
			_check(mod_script.get_breakpoints() == PackedInt32Array([OTHER_LINE]),
				"the first build already carries what the editor holds")
			mod_node = Node.new()
			mod_node.set_script(mod_script)
			root.add_child(mod_node)
			# State the poll must not throw away. Reloading the program after
			# the fact is what a mod's mod_init() does not survive.
			mod_node.call("init_state")
		7:
			_check(mod_script.get_active_breakpoints() == PackedInt32Array([OTHER_LINE]),
				"and it was compiled in without a rebuild")
			_check(mod_node.call("state") == 7,
				"the poll left the running program alone")
			print("[dbg] %d failure(s)" % failures)
			quit(1 if failures > 0 else 0)
			return true
	return false

func _check(condition: bool, what: String) -> void:
	if condition:
		print("[dbg] ok: ", what)
	else:
		_fail(what)

func _fail(what: String) -> void:
	failures += 1
	print("[dbg] FAILED: ", what)
