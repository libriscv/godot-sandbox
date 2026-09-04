extends GutTest

# The loader half of binary translation. Nothing in this addon produces a
# hash-cache object any more, so this suite is fixture-driven: point it at a
# directory of bintr-<HASH> objects and the ELF they were baked from, and it
# proves the loader finds them, refuses the ones it should, and computes the
# same results either way. Without a fixture it is pending, which is what a
# checkout with no baking tool looks like.
#
#   SGD_TEST_BINTR_ELF   path to the ELF the objects were baked from
#   SGD_TEST_BINTR_DIR   directory holding bintr-<HASH>.{so,dll,dylib}
#   SGD_TEST_BINTR_CALL  optional guest function to call on both sides

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

func before_all():
	_old_cache_dir = ProjectSettings.get_setting("sandbox/binary_translation/cache_dir")
	_old_lookup = ProjectSettings.get_setting("sandbox/binary_translation/enabled")

# Opting into the AOT cache is per test, not per script: a machine that may take
# part in the cache gets its own execute segment, so every load_buffer() while it
# is on re-decodes and re-JITs the program. A script-wide window would leak that
# cost into the rest of the suite if this script ever died before after_all().
func before_each():
	ProjectSettings.set_setting("sandbox/binary_translation/enabled", true)
	var dir := _fixture_dir()
	if not dir.is_empty():
		ProjectSettings.set_setting("sandbox/binary_translation/cache_dir", dir)

func after_each():
	ProjectSettings.set_setting("sandbox/binary_translation/enabled", _old_lookup)
	ProjectSettings.set_setting("sandbox/binary_translation/cache_dir", _old_cache_dir)

func after_all():
	ProjectSettings.set_setting("sandbox/binary_translation/cache_dir", _old_cache_dir)
	ProjectSettings.set_setting("sandbox/binary_translation/enabled", _old_lookup)

# The suite runs hundreds of load_buffer() calls on the unit-test ELF. They share
# one execute segment only while the AOT cache is off, so a default (or a
# project.godot) that turns it on costs ~19s per test in a Debug CI build. Fail
# here rather than in a three-hour CI run.
func test_the_suite_baseline_keeps_the_aot_cache_off():
	assert_false(_old_lookup,
			"sandbox/binary_translation/enabled must stay off outside this script")

func _fixture_dir() -> String:
	return OS.get_environment("SGD_TEST_BINTR_DIR")

func _fixture_elf() -> PackedByteArray:
	var path := OS.get_environment("SGD_TEST_BINTR_ELF")
	if path.is_empty() or not FileAccess.file_exists(path):
		return PackedByteArray()
	return FileAccess.get_file_as_bytes(path)

func _hash_name(hash: int) -> String:
	# String.pad_zeros() treats a leading A-F like a sign; the loader contract is
	# exactly eight hexadecimal digits, as emitted by libriscv's %08X filename.
	return "bintr-%08X" % (hash & 0xFFFFFFFF)

func _sandbox(content: PackedByteArray, nbit_as := true) -> Sandbox:
	var sandbox := Sandbox.new()
	sandbox.set_memory_max(32)
	sandbox.set_binary_translation_bg_compilation(false)
	sandbox.binary_translation_nbit_as = nbit_as
	sandbox.load_buffer(content)
	return sandbox

# A SafeGDScript program has a hash whether or not anything ever bakes it: the
# hash is what a loader looks the object up by, and it is the only part of the
# bake contract this addon owns.
func test_the_hash_names_the_object_a_loader_looks_for():
	if not Sandbox.has_feature_binary_translation():
		pending("the extension was built without the C99 binary translator")
		return

	var script := SafeGDScript.new()
	script.set_source_code(SOURCE)
	assert_eq(script.get_compile_error(), "")
	assert_eq(script.get_translation_hash(), 0, "a script without an instance has no live machine")
	var content := script.get_content()

	var second := SafeGDScript.new()
	second.set_source_code(SOURCE)
	assert_true(content == second.get_content(),
			"SafeGDScript compilation is deterministic in one process")

	var sandbox := _sandbox(content)
	var hash := sandbox.get_translation_hash()
	assert_ne(hash, 0, "a loaded program names an object")
	assert_eq(_hash_name(hash).length(), 14, "eight hexadecimal digits, no sign shuffle")

	# The hash covers the ABI-affecting options, not only the execute segment.
	var mismatch := _sandbox(content, false)
	assert_ne(mismatch.get_translation_hash(), hash,
			"a different option set looks for a different object")
	assert_eq([mismatch.vmcallv("answer"), mismatch.vmcallv("compute", 20)],
			[sandbox.vmcallv("answer"), sandbox.vmcallv("compute", 20)],
			"the execution mode never changes what a program computes")
	mismatch.free()

	var owner := Node.new()
	var live_script := SafeGDScript.new()
	live_script.set_source_code(SOURCE)
	owner.set_script(live_script)
	assert_ne(live_script.get_translation_hash(), 0,
			"the SafeGDScript wrapper reaches its live machine")
	owner.free()
	sandbox.free()

func test_a_baked_object_outranks_the_jit():
	var content := _fixture_elf()
	if content.is_empty():
		pending("set SGD_TEST_BINTR_ELF and SGD_TEST_BINTR_DIR to a baked fixture")
		return
	if not Sandbox.has_feature_binary_translation():
		pending("the extension was built without the C99 binary translator")
		return

	# Leaving the JIT enabled is the point: a cache hit must outrank asmjit, and
	# a hybrid segment would keep is_jit() true even though the object loaded.
	var had_jit := Sandbox.is_jit_enabled()
	Sandbox.set_jit_enabled(true)

	var translated := _sandbox(content)
	var hash := translated.get_translation_hash()
	assert_ne(hash, 0)
	assert_true(FileAccess.file_exists(_fixture_dir().path_join(_hash_name(hash)) + _suffix()),
			"the fixture directory should hold %s for this host" % _hash_name(hash))
	assert_true(translated.is_binary_translated(), "the machine loads the baked object")
	assert_false(translated.is_jit(), "the loaded object is AOT, not asmjit")

	var call_name := OS.get_environment("SGD_TEST_BINTR_CALL")
	if not call_name.is_empty():
		Sandbox.set_jit_enabled(false)
		var interpreted := _sandbox(content, false)
		assert_eq(translated.vmcallv(call_name), interpreted.vmcallv(call_name),
				"the baked object computes what the interpreter computes")
		interpreted.free()

	translated.free()
	Sandbox.set_jit_enabled(had_jit)

func test_a_wrong_option_set_misses_instead_of_crashing():
	var content := _fixture_elf()
	if content.is_empty():
		pending("set SGD_TEST_BINTR_ELF and SGD_TEST_BINTR_DIR to a baked fixture")
		return
	if not Sandbox.has_feature_binary_translation():
		pending("the extension was built without the C99 binary translator")
		return

	var had_jit := Sandbox.is_jit_enabled()
	Sandbox.set_jit_enabled(false)
	var mismatch := _sandbox(content, false)
	assert_ne(mismatch.get_translation_hash(), 0)
	assert_false(mismatch.is_binary_translated(),
			"an object baked for another option set is a miss, not a load")
	mismatch.free()
	Sandbox.set_jit_enabled(had_jit)

func test_the_legacy_loader_refuses_a_hash_cache_object():
	var content := _fixture_elf()
	if content.is_empty():
		pending("set SGD_TEST_BINTR_ELF and SGD_TEST_BINTR_DIR to a baked fixture")
		return
	if not Sandbox.has_feature_binary_translation():
		pending("the extension was built without the C99 binary translator")
		return

	var probe := _sandbox(content)
	var path := _fixture_dir().path_join(_hash_name(probe.get_translation_hash())) + _suffix()
	probe.free()
	assert_false(Sandbox.load_binary_translation(path, true),
			"the legacy self-registering loader refuses a hash-cache object")
	assert_engine_error("The library is a hash-cache translation")

func _suffix() -> String:
	match OS.get_name().to_lower():
		"windows": return ".dll"
		"macos": return ".dylib"
		_: return ".so"
