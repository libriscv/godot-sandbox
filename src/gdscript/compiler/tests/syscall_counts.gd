func array_calls(values: Array, count: int) -> int:
	var answer := 0
	for i in count:
		answer += values.size()
	return answer

func string_calls(value: String, count: int) -> int:
	var answer := 0
	for i in count:
		answer += value.length()
	return answer

func dictionary_calls(value: Dictionary, count: int) -> int:
	var answer := 0
	for i in count:
		answer += value.size()
	return answer

func first(value: String) -> String:
	return value[0]

func array_walk(values: Array) -> int:
	var answer := 0
	for value in values:
		answer += int(value)
	return answer

func array_index_calls(values: Array, count: int) -> int:
	var answer := 0
	for i in count:
		answer += int(values[0])
	return answer

func string_walk(value: String) -> int:
	var answer := 0
	for character in value:
		answer += character.length()
	return answer

func string_characters(value: String) -> String:
	var answer := ""
	for character in value:
		answer += character
	return answer

func codepoints(value: String) -> int:
	var answer := 0
	for character in value:
		answer += ord(character)
	return answer

func power_two(value: int) -> int:
	return nearest_po2(value)

func random_integer() -> int:
	return randi()

func random_range() -> int:
	return randi_range(7, 7)

func floating_point(value: float) -> float:
	return sin(value) + cos(value)

func array_create() -> Array:
	return [1, 2, 3]

func make_callable() -> Callable:
	return func() -> int: return 42

func breakpoint_check() -> void:
	breakpoint

func fault(value: String) -> String:
	return value[10000]

# Repeated calls with live floating-point values and backedges.
func math_loop(value: float, count: int) -> float:
	var answer := value
	for i in count:
		var keep := answer * 0.25 + 0.125
		answer = sin(answer) + cos(keep) + atan2(keep, answer + 2.0)
	return answer

func math_wide(a: float, b: float, c: float, d: float, e: float, f: float, g: float, h: float) -> float:
	return cubic_interpolate_in_time(a, b, c, d, e, f, g, h)

func math_ternary(a: float, b: float, weight: float) -> float:
	return lerp(a, b, weight)

func math_predicate(value: float) -> bool:
	return is_nan(value)

func random_float() -> float:
	return randf_range(0.75, 0.75)

func dictionary_edit(value: Dictionary) -> int:
	value["answer"] = 42
	return int(value["answer"])
