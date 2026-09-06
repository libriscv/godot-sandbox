extends RefCounted

const TABLE_PATH := "res://tests/variant_api.json"

const UNSUPPORTED_PATH := "../src/gdscript/compiler/tests/variant_api_unsupported.txt"

# GDScript refuses non-constant integer indexing on these.
const NO_INTEGER_INDEX := ["Quaternion"]

# How many components `[]` reaches; the table carries the element type, not the count.
const INDEX_COUNT := {
	"Vector2": 2, "Vector2i": 2, "Vector3": 3, "Vector3i": 3,
	"Vector4": 4, "Vector4i": 4, "Quaternion": 4, "Color": 4,
	"Basis": 3, "Transform2D": 3, "Projection": 4,
}

# `target op= value` for the scalars, which have no rows in the type table.
const SCALAR_COMPOUND := {
	"int": [["+", "int"], ["-", "int"], ["*", "int"], ["/", "int"], ["%", "int"]],
	"float": [["+", "float"], ["-", "float"], ["*", "float"], ["/", "float"]],
}

class Options:
	extends RefCounted
	var functions := 4
	var max_statements := 5
	var max_depth := 3
	var node_budget := 24
	var leaf_chance := 20
	var typed_declaration_chance := 40
	var typed_return_chance := 40
	var lvalue_depth := 2
	var compound_chance := 35
	var opaque_chance := 25

var _state: int
var _seed: int
var _options: Options

var _types: Dictionary
var _samples: Dictionary
var _nonzero: Dictionary
var _producers: Dictionary
var _unsupported: Dictionary = {}
var _all_types: Array

var _budget := 0
var _locals: Array = []
var _local_count := 0


func _init(p_seed: int, p_table: Dictionary, p_options: Options = null) -> void:
	_seed = p_seed
	_state = (p_seed * 2654435761) ^ 0x2545F4914F6CDD1
	if _state == 0:
		_state = 0x9E3779B9
	_options = p_options if p_options != null else Options.new()
	_types = p_table["types"]
	_samples = p_table["samples"]
	_nonzero = p_table["nonzero"]
	_unsupported = load_unsupported()
	_all_types = []
	for name in _types:
		_all_types.append(name)
	for name in p_table["scalars"]:
		_all_types.append(name)
	_build_producers()


static func load_table() -> Dictionary:
	var text := FileAccess.get_file_as_string(TABLE_PATH)
	if text.is_empty():
		return {}
	var parsed = JSON.parse_string(text)
	return parsed if parsed is Dictionary else {}


func _next() -> int:
	_state ^= _ushr(_state, 12)
	_state ^= _state << 25
	_state ^= _ushr(_state, 27)
	var value: int = _state * 2685821657736338717
	return value if value >= 0 else ~value


func _ushr(value: int, bits: int) -> int:
	return (value >> bits) & ((1 << (64 - bits)) - 1)


func _below(bound: int) -> int:
	return _next() % bound if bound > 0 else 0


func _chance(percent: int) -> bool:
	return _below(100) < percent


func _pick(options: Array):
	return options[_below(options.size())]


static func row_id(recipe: Dictionary) -> String:
	match recipe["kind"]:
		"const":
			return "%s const %s" % [recipe["type"], recipe["name"]]
		"ctor":
			return "%s ctor(%s)" % [recipe["type"], ",".join(PackedStringArray(recipe["args"]))]
		"member":
			return "%s member %s" % [recipe["owner"], recipe["name"]]
		"index":
			return "%s index" % recipe["owner"]
		"op":
			return "%s op %s %s" % [recipe["owner"], recipe["op"], recipe["right"]]
		"unary":
			return "%s unary %s" % [recipe["owner"], recipe["op"]]
		"method":
			return "%s %s %s(%s)" % [
				recipe["owner"], "static" if recipe["static"] else "method",
				recipe["name"], ",".join(PackedStringArray(recipe["args"])),
			]
	return ""


static func load_unsupported() -> Dictionary:
	var path := ProjectSettings.globalize_path("res://").path_join(UNSUPPORTED_PATH)
	if not FileAccess.file_exists(path):
		return {}
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	var listed := {}
	while not file.eof_reached():
		var line := file.get_line().strip_edges()
		if line != "" and not line.begins_with("#"):
			listed[line] = true
	return listed


func _add_producer(result_type: String, recipe: Dictionary) -> void:
	if _unsupported.has(row_id(recipe)):
		return
	if not _producers.has(result_type):
		_producers[result_type] = []
	_producers[result_type].append(recipe)


func _build_producers() -> void:
	_producers = {}
	for type_name in _samples:
		_add_producer(type_name, {"kind": "sample", "type": type_name})
	for type_name in _types:
		var rows: Dictionary = _types[type_name]
		for row in rows["constants"]:
			_add_producer(row[1], {"kind": "const", "type": type_name, "name": row[0]})
		for arguments in rows["constructors"]:
			_add_producer(type_name, {"kind": "ctor", "type": type_name, "args": arguments})
		for row in rows["members"]:
			_add_producer(row[1], {"kind": "member", "owner": type_name, "name": row[0]})
		if rows["index"] != null and not NO_INTEGER_INDEX.has(type_name):
			_add_producer(rows["index"], {"kind": "index", "owner": type_name})
		for row in rows["operators"]:
			_add_producer(row[2], {"kind": "op", "owner": type_name, "op": row[0], "right": row[1]})
		for row in rows["unary"]:
			_add_producer(row[1], {"kind": "unary", "owner": type_name, "op": row[0]})
		for row in rows["methods"]:
			_add_producer(row[2], {
				"kind": "method", "owner": type_name, "name": row[0],
				"args": row[1], "static": row[3],
			})


func _leaf(type: String) -> String:
	var in_scope := []
	for local in _locals:
		if local["type"] == type:
			in_scope.append(local["name"])
	if not in_scope.is_empty() and _chance(45):
		return _pick(in_scope)
	if _samples.has(type):
		return _pick(_samples[type])
	return "null"


func _expression(type: String, depth: int) -> String:
	if depth <= 0 or _budget <= 0 or _chance(_options.leaf_chance):
		return _leaf(type)
	var options: Array = _producers.get(type, [])
	if options.is_empty():
		return _leaf(type)
	_budget -= 1
	return _render(_pick(options), depth)


# Axis/enum arguments; Godot errors on out-of-range values.
func _int_argument() -> String:
	return str(_below(3))


func _argument(type: String, depth: int) -> String:
	if type == "int":
		return _int_argument()
	return _expression(type, depth - 1)


func _arguments(types: Array, depth: int) -> String:
	var parts := PackedStringArray()
	for type in types:
		parts.append(_argument(type, depth))
	return ", ".join(parts)


func _divisor(type: String, depth: int) -> String:
	if _nonzero.has(type):
		return _pick(_nonzero[type])
	return _expression(type, depth - 1)


func _unary_spelling(op: String) -> String:
	match op:
		"unary-": return "-"
		"unary+": return "+"
		"not": return "not "
		_: return op


func _render(recipe: Dictionary, depth: int) -> String:
	match recipe["kind"]:
		"sample":
			return _leaf(recipe["type"])
		"const":
			return "%s.%s" % [recipe["type"], recipe["name"]]
		"ctor":
			return "%s(%s)" % [recipe["type"], _arguments(recipe["args"], depth)]
		"member":
			return "(%s).%s" % [_expression(recipe["owner"], depth - 1), recipe["name"]]
		"index":
			return "(%s)[%d]" % [_expression(recipe["owner"], depth - 1),
				_below(INDEX_COUNT.get(recipe["owner"], 2))]
		"op":
			var left := _expression(recipe["owner"], depth - 1)
			var op: String = recipe["op"]
			var right := (_divisor(recipe["right"], depth) if op == "/" or op == "%"
				else _expression(recipe["right"], depth - 1))
			return "(%s %s %s)" % [left, op, right]
		"unary":
			return "(%s%s)" % [_unary_spelling(recipe["op"]), _expression(recipe["owner"], depth - 1)]
		"method":
			var arguments := _arguments(recipe["args"], depth)
			if recipe["static"]:
				return "%s.%s(%s)" % [recipe["owner"], recipe["name"], arguments]
			return "(%s).%s(%s)" % [_expression(recipe["owner"], depth - 1), recipe["name"], arguments]
	return "null"


# An Array element is a Variant, so the value keeps its type while the compiler
# loses it: the same statements then lower through the run-time dispatch arms.
func _opaque(value: String) -> String:
	return "([%s])[0]" % value


func _declare(type: String, depth: int) -> String:
	var name := "v%d" % _local_count
	_local_count += 1
	var value := _expression(type, depth)
	if _chance(_options.opaque_chance):
		value = _opaque(value)
	var line: String
	if _chance(_options.typed_declaration_chance):
		line = "\tvar %s: %s = %s" % [name, type, value]
	else:
		line = "\tvar %s = %s" % [name, value]
	_locals.append({"name": name, "type": type})
	return line


# Every write target reachable from `text`, one assignable step at a time.
func _extend_lvalues(text: String, type: String, remaining: int, into: Array) -> void:
	if remaining <= 0 or not _types.has(type):
		return
	var rows: Dictionary = _types[type]
	for member in rows["members"]:
		if _unsupported.has("%s member= %s" % [type, member[0]]):
			continue
		var reached := "%s.%s" % [text, member[0]]
		into.append({"text": reached, "type": member[1]})
		_extend_lvalues(reached, member[1], remaining - 1, into)
	if rows["index"] == null or NO_INTEGER_INDEX.has(type):
		return
	if _unsupported.has("%s index=" % type):
		return
	var reached := "%s[%d]" % [text, _below(INDEX_COUNT.get(type, 2))]
	into.append({"text": reached, "type": rows["index"]})
	_extend_lvalues(reached, rows["index"], remaining - 1, into)


func _lvalues() -> Array:
	var targets := []
	for local in _locals:
		targets.append({"text": local["name"], "type": local["type"]})
		_extend_lvalues(local["name"], local["type"], _options.lvalue_depth, targets)
	return targets


# `op` such that `type op right` is again a `type`, so `target op= right` type-checks.
func _compound_operators(type: String) -> Array:
	if SCALAR_COMPOUND.has(type):
		return SCALAR_COMPOUND[type]
	if not _types.has(type):
		return []
	var rows := []
	for row in _types[type]["operators"]:
		if row[2] == type and (row[0] == "+" or row[0] == "-" or row[0] == "*"
			or row[0] == "/" or row[0] == "%"):
			rows.append([row[0], row[1]])
	return rows


func _assignment(target: Dictionary, depth: int) -> String:
	var type: String = target["type"]
	var operators := _compound_operators(type)
	if not operators.is_empty() and _chance(_options.compound_chance):
		var chosen: Array = _pick(operators)
		var op: String = chosen[0]
		var right := (_divisor(chosen[1], depth) if op == "/" or op == "%"
			else _expression(chosen[1], depth - 1))
		return "\t%s %s= %s" % [target["text"], op, right]
	return "\t%s = %s" % [target["text"], _expression(type, depth - 1)]


func _statement(depth: int) -> String:
	if _locals.is_empty() or _below(10) < 6:
		return _declare(_pick(_all_types), depth)
	var targets := _lvalues()
	if targets.is_empty():
		return _declare(_pick(_all_types), depth)
	return _assignment(_pick(targets), depth)


func _function(index: int) -> Dictionary:
	_locals = []
	_local_count = 0
	_budget = _options.node_budget
	var name := "f%d" % index
	var lines := PackedStringArray()
	var statements := 1 + _below(_options.max_statements)
	for i in statements:
		lines.append(_statement(_options.max_depth))
	var return_type: String = _pick(_all_types)
	var head := "func %s():" % name
	if _chance(_options.typed_return_chance):
		head = "func %s() -> %s:" % [name, return_type]
	lines.append("\treturn " + _expression(return_type, _options.max_depth))
	return {
		"name": name,
		"source": head + "\n" + "\n".join(lines) + "\n",
	}


func generate() -> Dictionary:
	var functions := PackedStringArray()
	var sources := PackedStringArray()
	for i in _options.functions:
		var built := _function(i)
		functions.append(built["name"])
		sources.append(built["source"])
	return {
		"seed": _seed,
		"functions": functions,
		"source": "\n".join(sources),
	}


static func compile_engine(source: String) -> Dictionary:
	var script := GDScript.new()
	script.source_code = source
	var error := script.reload()
	if error != OK:
		return {"ok": false, "error": "GDScript.reload() returned %d" % error}
	return {"ok": true, "object": script.new(), "node": null}


static func compile_sgd(source: String, instructions_max: int = 8000000) -> Dictionary:
	var script := SafeGDScript.new()
	script.set_source_code(source)
	var error := script.get_compile_error()
	if error != "":
		return {"ok": false, "error": error}
	var node := Node.new()
	node.set_script(script)
	node.set_instructions_max(instructions_max)
	return {"ok": true, "object": node, "node": node}


static func release(compiled: Dictionary) -> void:
	if compiled.get("node") != null:
		compiled["node"].free()


# Tolerance for compiler-lowered arithmetic where the last float bit can differ.
const EPSILON := 1e-9


static func _floats_equal(a: float, b: float) -> bool:
	if a == b:
		return true
	if is_nan(a) or is_nan(b):
		return is_nan(a) and is_nan(b)
	if is_inf(a) or is_inf(b):
		return false
	var scale: float = maxf(1.0, maxf(absf(a), absf(b)))
	return absf(a - b) <= EPSILON * scale


static func difference(expected, actual) -> String:
	if typeof(expected) != typeof(actual):
		return "type %s vs %s" % [type_string(typeof(expected)), type_string(typeof(actual))]
	match typeof(expected):
		TYPE_FLOAT:
			if _floats_equal(expected, actual):
				return ""
			return "%s vs %s" % [String.num(expected, 17), String.num(actual, 17)]
		TYPE_VECTOR2, TYPE_VECTOR3, TYPE_VECTOR4, TYPE_QUATERNION, TYPE_COLOR:
			return _components(expected, actual, _axis_count(expected))
		TYPE_PLANE:
			var normal := difference(expected.normal, actual.normal)
			return normal if normal != "" else difference(expected.d, actual.d)
		TYPE_BASIS:
			for axis in 3:
				var d := difference(expected[axis], actual[axis])
				if d != "":
					return "row %d: %s" % [axis, d]
			return ""
		TYPE_TRANSFORM2D:
			for axis in 3:
				var d := difference(expected[axis], actual[axis])
				if d != "":
					return "column %d: %s" % [axis, d]
			return ""
		TYPE_TRANSFORM3D:
			var basis := difference(expected.basis, actual.basis)
			return basis if basis != "" else difference(expected.origin, actual.origin)
		TYPE_PROJECTION:
			for axis in 4:
				var d := difference(expected[axis], actual[axis])
				if d != "":
					return "column %d: %s" % [axis, d]
			return ""
		TYPE_RECT2, TYPE_AABB:
			var position := difference(expected.position, actual.position)
			return position if position != "" else difference(expected.size, actual.size)
	return "" if expected == actual else "%s vs %s" % [expected, actual]


# Godot's own answer at an infinity is not reproducible: the same
# `Projection.create_orthogonal_aspect(0.0, ...)` call comes back +inf or -inf
# depending on the statement before it, with no compiler involved. A result
# holding one says nothing about the compiler, so it is not compared.
static func has_infinity(value) -> bool:
	match typeof(value):
		TYPE_FLOAT:
			return is_inf(value)
		TYPE_VECTOR2, TYPE_VECTOR3, TYPE_VECTOR4, TYPE_QUATERNION, TYPE_COLOR:
			return _any_component_infinite(value, _axis_count(value))
		TYPE_PLANE:
			return has_infinity(value.normal) or has_infinity(value.d)
		TYPE_BASIS, TYPE_TRANSFORM2D:
			return _any_component_infinite(value, 3)
		TYPE_PROJECTION:
			return _any_component_infinite(value, 4)
		TYPE_TRANSFORM3D:
			return has_infinity(value.basis) or has_infinity(value.origin)
		TYPE_RECT2, TYPE_AABB:
			return has_infinity(value.position) or has_infinity(value.size)
	return false


static func _any_component_infinite(value, count: int) -> bool:
	for axis in count:
		if has_infinity(value[axis]):
			return true
	return false


static func _axis_count(value) -> int:
	match typeof(value):
		TYPE_VECTOR2: return 2
		TYPE_VECTOR3: return 3
		TYPE_COLOR, TYPE_VECTOR4, TYPE_QUATERNION: return 4
	return 0


static func _components(expected, actual, count: int) -> String:
	for axis in count:
		var d := difference(expected[axis], actual[axis])
		if d != "":
			return "component %d: %s" % [axis, d]
	return ""


static func shrink(source: String, still_fails: Callable) -> String:
	var lines := source.split("\n")
	var index := 0
	while index < lines.size():
		var line: String = lines[index]
		if not line.begins_with("\t") or line.begins_with("\treturn"):
			index += 1
			continue
		var candidate := lines.duplicate()
		candidate.remove_at(index)
		var attempt := "\n".join(candidate)
		if still_fails.call(attempt):
			lines = candidate
		else:
			index += 1
	return "\n".join(lines)


static func isolate_function(source: String, name: String) -> String:
	var kept := PackedStringArray()
	var keeping := false
	for line in source.split("\n"):
		if line.begins_with("func "):
			keeping = line.begins_with("func %s(" % name)
		if keeping:
			kept.append(line)
	return "\n".join(kept) + "\n"
