extends GutTest

const SOURCE := """
func answer():
	return 42

func compute(n: int):
	var total: int = 0
	for i in range(n):
		total += i * i
	return total
"""

var _old_cache_dir: String
var _old_lookup: bool
var _baked_paths: PackedStringArray

func before_all():
	_old_cache_dir = ProjectSettings.get_setting("sandbox/binary_translation/cache_dir")
	_old_lookup = ProjectSettings.get_setting("sandbox/binary_translation/enabled")
	ProjectSettings.set_setting("sandbox/binary_translation/cache_dir", "user://test_sandbox_bintr/")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("user://test_sandbox_bintr/"))

# Opting into the AOT cache is per test, not per script: a machine that may take
# part in the cache gets its own execute segment, so every load_buffer() while it
# is on re-decodes and re-JITs the program. A script-wide window would leak that
# cost into the rest of the suite if this script ever died before after_all().
func before_each():
	ProjectSettings.set_setting("sandbox/binary_translation/enabled", true)

func after_each():
	ProjectSettings.set_setting("sandbox/binary_translation/enabled", _old_lookup)

func after_all():
	for path in _baked_paths:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	# The resolved cache directory is remembered until it stops existing
	DirAccess.remove_absolute(ProjectSettings.globalize_path("user://test_sandbox_bintr/"))
	ProjectSettings.set_setting("sandbox/binary_translation/cache_dir", _old_cache_dir)
	ProjectSettings.set_setting("sandbox/binary_translation/enabled", _old_lookup)

# The suite runs hundreds of load_buffer() calls on the unit-test ELF. They share
# one execute segment only while the AOT cache is off, so a default (or a
# project.godot) that turns it on costs ~19s per test in a Debug CI build. Fail
# here rather than in a three-hour CI run.
func test_the_suite_baseline_keeps_the_aot_cache_off():
	assert_false(_old_lookup,
			"sandbox/binary_translation/enabled must stay off outside this script")

func _compiler_available() -> bool:
	var compiler: String = ProjectSettings.get_setting("sandbox/binary_translation/compiler")
	var args := PackedStringArray(["version"] if compiler.get_file().get_basename() == "zig" else ["--version"])
	return OS.execute(compiler, args, [], true) == 0

func _compiled_content() -> PackedByteArray:
	var script := SafeGDScript.new()
	script.set_source_code(SOURCE)
	assert_eq(script.get_compile_error(), "")
	assert_eq(script.get_translation_hash(), 0, "a script without an instance has no live machine")
	return script.get_content()

func _sandbox(content: PackedByteArray, nbit_as := true) -> Sandbox:
	var sandbox := Sandbox.new()
	sandbox.set_memory_max(32)
	sandbox.set_binary_translation_bg_compilation(false)
	sandbox.binary_translation_nbit_as = nbit_as
	sandbox.load_buffer(content)
	return sandbox

func test_hash_named_binary_translation():
	if not Sandbox.has_feature_binary_translation():
		pending("the extension was built without the C99 binary translator")
		return
	if not _compiler_available():
		pending("the configured binary-translation C compiler is unavailable")
		return

	var first := _compiled_content()
	var second := _compiled_content()
	assert_true(first == second, "SafeGDScript compilation is deterministic in one process")

	var had_jit := Sandbox.is_jit_enabled()
	Sandbox.set_jit_enabled(false)
	var sandbox := _sandbox(first)
	var hash := sandbox.get_translation_hash()
	assert_ne(hash, 0)
	var expected := [sandbox.vmcallv("answer"), sandbox.vmcallv("compute", 20)]
	var path := sandbox.bake_binary_translation()
	assert_false(path.is_empty())
	_baked_paths.push_back(path)
	assert_true(sandbox.is_translation_baked())
	sandbox.free()

	var live_script := SafeGDScript.new()
	live_script.set_source_code(SOURCE)
	var owner := Node.new()
	owner.set_script(live_script)
	assert_eq(live_script.get_translation_hash(), hash, "the SafeGDScript wrapper reaches its live machine")
	assert_true(live_script.is_translation_baked())
	assert_eq(live_script.bake_translation(), path, "the wrapper reuses the existing hash object")
	owner.free()

	# A cache hit must outrank asmjit. Leaving JIT enabled here catches a hybrid
	# segment, for which is_jit() would remain true even though the object loaded.
	Sandbox.set_jit_enabled(true)
	var translated := _sandbox(first)
	assert_true(translated.is_binary_translated(), "the next machine loads the baked object")
	assert_false(translated.is_jit(), "the loaded object is AOT, not asmjit")
	assert_eq([translated.vmcallv("answer"), translated.vmcallv("compute", 20)], expected)
	translated.free()

	var mismatch := _sandbox(first, false)
	assert_ne(mismatch.get_translation_hash(), hash)
	assert_false(mismatch.is_binary_translated() and not mismatch.is_jit(),
			"a different option hash safely misses the AOT object")
	assert_eq([mismatch.vmcallv("answer"), mismatch.vmcallv("compute", 20)], expected)
	mismatch.free()

	var translated_again := _sandbox(first)
	assert_true(translated_again.is_binary_translated())
	translated_again.free()
	Sandbox.set_jit_enabled(had_jit)

	assert_false(Sandbox.load_binary_translation(path, true),
			"the legacy self-registering loader refuses a hash-cache object")
	assert_engine_error("The library is a hash-cache translation")
