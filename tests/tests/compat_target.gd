class_name GdCompatTarget
extends RefCounted

const ANSWER := 41
enum Kind { A, B = 8 }
static var constructions := 0

func _init():
	constructions += 1

static func add(a: int, b: int) -> int:
	return a + b

static func construction_count() -> int:
	return constructions
