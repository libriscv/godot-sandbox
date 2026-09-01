extends SceneTree

# Kept outside GUT's `bench_*.gd` naming convention. The runner uses it to
# decide whether Full is available before resetting or baking the cache.
func _initialize() -> void:
	if not Sandbox.has_feature_binary_translation():
		print("GDSC_BENCH_FULL_SKIP=the addon was built without the C99 binary translator")
		quit()
		return
	var compiler: String = ProjectSettings.get_setting("sandbox/binary_translation/compiler")
	var args := PackedStringArray(["version"] if compiler.get_file().get_basename() == "zig" else ["--version"])
	if OS.execute(compiler, args, [], true) != 0:
		print("GDSC_BENCH_FULL_SKIP=the configured C compiler '%s' is unavailable" % compiler)
		quit()
		return
	print("GDSC_BENCH_FULL_OK=%s" % compiler)
	quit()
