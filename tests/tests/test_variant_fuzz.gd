#   GDSC_FUZZ_SEED=$(date +%s) GDSC_FUZZ_COUNT=5000 ./run_unittests.sh -gselect variant_fuzz
#   GDSC_FUZZ_SEED=<n> GDSC_FUZZ_COUNT=1 reproduces one seed exactly.
extends GutTest

const VariantFuzz = preload("res://tests/variant_fuzz.gd")

const DEFAULT_COUNT := 60
const REPORTED_FAILURES := 3

var _table: Dictionary = {}
var _compared := 0
var _skipped := 0


func before_all() -> void:
	_table = VariantFuzz.load_table()
	assert_false(_table.is_empty(),
		"the generated API table should load from " + VariantFuzz.TABLE_PATH)


# Claim engine errors so they don't fail the run; .sgd-only errors are findings.
func _drain_errors() -> Array:
	var fresh := []
	for error in get_errors():
		if not error.handled:
			error.handled = true
			fresh.append(error)
	return fresh


func _error_text(errors: Array) -> String:
	var parts := PackedStringArray()
	for error in errors:
		parts.append(str(error.code))
	return "\n    ".join(parts)


func _failure(source: String, only: String = "") -> Dictionary:
	_drain_errors()
	var engine := VariantFuzz.compile_engine(source)
	# GDScript folds constants at compile time; drain those before checking .sgd.
	var folded := _drain_errors()
	if not engine["ok"]:
		return {"kind": "generator", "detail": engine["error"], "function": ""}
	if not folded.is_empty():
		_skipped += 1
		return {"kind": "", "detail": "", "function": ""}

	var sgd := VariantFuzz.compile_sgd(source)
	var compile_errors := _drain_errors()
	if not sgd["ok"]:
		VariantFuzz.release(sgd)
		return {"kind": "compile", "detail": sgd["error"], "function": ""}
	if not compile_errors.is_empty():
		VariantFuzz.release(sgd)
		return {"kind": "error", "detail": _error_text(compile_errors), "function": ""}

	var result := {"kind": "", "detail": "", "function": ""}
	for name in _function_names(source):
		if only != "" and name != only:
			continue
		var expected = engine["object"].callv(name, [])
		if not _drain_errors().is_empty():
			_skipped += 1
			continue
		var actual = sgd["object"].callv(name, [])
		var errors := _drain_errors()
		if not errors.is_empty():
			result = {"kind": "error", "detail": _error_text(errors), "function": name}
			break
		var difference := VariantFuzz.difference(expected, actual)
		if difference != "":
			result = {
				"kind": "value",
				"detail": "%s: %s" % [name, difference],
				"function": name,
			}
			break
		_compared += 1
	VariantFuzz.release(sgd)
	return result


func _function_names(source: String) -> PackedStringArray:
	var names := PackedStringArray()
	for line in source.split("\n"):
		if line.begins_with("func "):
			names.append(line.substr(5, line.find("(") - 5))
	return names


# Normalize digits so failures differing only in values are grouped.
func _signature(failure: Dictionary) -> String:
	var detail: String = failure["detail"]
	var digits := RegEx.create_from_string("[-0-9.]+")
	return "%s:%s" % [failure["kind"], digits.sub(detail, "N", true)]


func _reduce(seed_value: int, source: String, failure: Dictionary) -> String:
	var reduced := source
	if failure["function"] != "":
		var isolated := VariantFuzz.isolate_function(source, failure["function"])
		if _failure(isolated, failure["function"])["kind"] == failure["kind"]:
			reduced = isolated
	var kind: String = failure["kind"]
	var name: String = failure["function"]
	reduced = VariantFuzz.shrink(reduced, func(candidate: String) -> bool:
		return _failure(candidate, name)["kind"] == kind)
	return "\n".join([
		"seed %d: %s" % [seed_value, failure["detail"]],
		"reproduce with GDSC_FUZZ_SEED=%d GDSC_FUZZ_COUNT=1" % seed_value,
		"",
		reduced,
	])


func _run_seeds(first: int, count: int) -> void:
	_compared = 0
	_skipped = 0
	var found := {}
	for offset in count:
		var seed_value := first + offset
		var generator := VariantFuzz.new(seed_value, _table)
		var program: Dictionary = generator.generate()
		var failure := _failure(program["source"])
		if failure["kind"] == "":
			continue
		var signature := _signature(failure)
		if not found.has(signature):
			found[signature] = {"seed": seed_value, "source": program["source"], "failure": failure}

	gut.p("%d seeds: %d functions compared, %d not comparable, %d distinct failures"
		% [count, _compared, _skipped, found.size()])
	if found.is_empty():
		pass_test("the compiler answered what GDScript answers for %d seeds" % count)
		return
	var reported := 0
	for signature in found:
		var entry: Dictionary = found[signature]
		if reported < REPORTED_FAILURES:
			fail_test(_reduce(entry["seed"], entry["source"], entry["failure"]))
		else:
			fail_test("seed %d: %s (reproduce with GDSC_FUZZ_SEED=%d GDSC_FUZZ_COUNT=1)"
				% [entry["seed"], entry["failure"]["detail"], entry["seed"]])
		reported += 1


func test_the_math_builtins_answer_what_gdscript_answers() -> void:
	var first := int(OS.get_environment("GDSC_FUZZ_SEED"))
	var count := int(OS.get_environment("GDSC_FUZZ_COUNT"))
	_run_seeds(first, count if count > 0 else DEFAULT_COUNT)


func test_the_generator_is_deterministic_and_produces_whole_programs() -> void:
	var first: Dictionary = VariantFuzz.new(1234, _table).generate()
	var again: Dictionary = VariantFuzz.new(1234, _table).generate()
	assert_eq(first["source"], again["source"], "one seed should produce one program")
	assert_ne(VariantFuzz.new(1235, _table).generate()["source"], first["source"],
		"different seeds should produce different programs")
	assert_gt(first["functions"].size(), 0, "a program should have functions to call")
	for name in first["functions"]:
		assert_true(first["source"].contains("func %s(" % name),
			"%s should be in the source" % name)
