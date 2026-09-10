extends SceneTree

# Run with --headless --path <project with the extension> --script <this file>.
var failures := 0

func check(ok: bool, message: String) -> void:
	if not ok:
		failures += 1
		push_error(message)

func _initialize() -> void:
	call_deferred("run_checks")

func run_checks() -> void:
	for nested_call in [false, true]:
		var script = SafeGDScript.new()
		script.source_code = """extends CompositorEffect
var shader: RID
func touch() -> void:
	Engine.set_meta("compat_touched", shader.is_valid())
func _notification(what: int) -> void:
	if what == NOTIFICATION_PREDELETE:
		Engine.set_meta("compat_deleted", shader.is_valid())
"""
		if nested_call:
			script.source_code += "\nfunc _init() -> void:\n\tRenderingServer.call_on_render_thread(touch)\n"
		check(script.reload() == OK, "Lifetime fixture compiles")
		var instance = script.new()
		var weak = weakref(instance)
		await process_frame
		if nested_call:
			check(Engine.get_meta("compat_touched", null) == false, "Nested render callback reads fields")
		instance = null
		check(weak.get_ref() == null, "Scoped call references do not keep the effect alive")
		check(Engine.get_meta("compat_deleted", null) == false, "Predelete runs and reads the RID")
		if Engine.has_meta("compat_deleted"):
			Engine.remove_meta("compat_deleted")
		if Engine.has_meta("compat_touched"):
			Engine.remove_meta("compat_touched")
		script = null
		await process_frame
	# These script-backed registrations are released by Godot after SceneTree's
	# final deletion-queue flush. Their idle Sandbox nodes must be freed then.
	var loader_script = SafeGDScript.new()
	loader_script.source_code = """extends ResourceFormatLoader
func _get_recognized_extensions() -> PackedStringArray:
	return PackedStringArray(["compat_lifetime"])
"""
	check(loader_script.reload() == OK, "Shutdown loader compiles")
	var loader = loader_script.new()
	ResourceLoader.add_resource_format_loader(loader)
	check("compat_lifetime" in ResourceLoader.get_recognized_extensions_for_type(""), "Script loader is registered")
	var saver_script = SafeGDScript.new()
	saver_script.source_code = """extends ResourceFormatSaver
func _get_recognized_extensions(_resource: Resource) -> PackedStringArray:
	return PackedStringArray(["compat_lifetime"])
"""
	check(saver_script.reload() == OK, "Shutdown saver compiles")
	var saver = saver_script.new()
	ResourceSaver.add_resource_format_saver(saver)
	check("compat_lifetime" in ResourceSaver.get_recognized_extensions(Resource.new()), "Script saver is registered")
	print("SafeGDScript lifetime checks: ", failures, " failures")
	quit(1 if failures else 0)
