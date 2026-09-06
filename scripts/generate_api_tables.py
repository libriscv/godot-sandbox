#!/usr/bin/env python3
"""Generate the small compiler lookup tables derived from extension_api.json."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


REPOSITORY = Path(__file__).resolve().parents[1]
DEFAULT_API = REPOSITORY / "ext/godot-cpp/gdextension/extension_api.json"
COMPILER = REPOSITORY / "src/gdscript/compiler"
TESTS = REPOSITORY / "tests/tests"
HOST_CONSTANT_TYPES = {"Transform2D", "Transform3D", "Basis", "Quaternion", "Projection"}


def version_name(api: dict[str, object]) -> str:
	header = api["header"]
	assert isinstance(header, dict)
	return f"{header['version_major']}.{header['version_minor']}.{header['version_patch']}"


def class_enums(api: dict[str, object]) -> str:
	rows: list[tuple[str, str]] = []
	for cls in api["classes"]:
		for enum in cls.get("enums", []):
			rows.append((cls["name"], enum["name"]))
	rows.sort()
	lines = [
		"// Engine class enumeration names.",
		f"// Generated from extension_api.json, Godot {version_name(api)}; do not edit.",
		"// Requires GDSC_CLASS_ENUM defined before inclusion.",
		"",
		"#ifndef GDSC_CLASS_ENUM",
		"#define GDSC_CLASS_ENUM(class_name, enum_name)",
		"#endif",
		"",
	]
	current = ""
	for class_name, enum_name in rows:
		if class_name != current:
			if current:
				lines.append("")
			lines.append(f"// {class_name}")
			current = class_name
		lines.append(f"GDSC_CLASS_ENUM({class_name}, {enum_name})")
	return "\n".join(lines) + "\n"


def constructor_values(type_name: str, value: str) -> list[str]:
	prefix = f"{type_name}("
	if not value.startswith(prefix) or not value.endswith(")"):
		raise ValueError(f"unsupported {type_name} constant spelling: {value}")
	inside = value[len(prefix):-1]
	return [part.strip() for part in inside.split(",") if part.strip()]


def host_constants(api: dict[str, object]) -> str:
	rows: list[tuple[str, str, list[str]]] = []
	for builtin in api["builtin_classes"]:
		type_name = builtin["name"]
		if type_name not in HOST_CONSTANT_TYPES:
			continue
		for constant in builtin.get("constants", []):
			values = constructor_values(type_name, constant["value"])
			if len(values) > 16:
				raise ValueError(f"too many components in {type_name}.{constant['name']}")
			rows.append((type_name, constant["name"], values))
	rows.sort()
	lines = [
		"// Built-in constants whose values need Godot's Variant constructor.",
		f"// Generated from extension_api.json, Godot {version_name(api)}; do not edit.",
		"// Values are the flattened constructor components, padded to sixteen.",
		"// Requires GDSC_HOST_CONSTANT defined before inclusion.",
		"",
		"#ifndef GDSC_HOST_CONSTANT",
		"#define GDSC_HOST_CONSTANT(type, name, count, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15)",
		"#endif",
		"",
	]
	current = ""
	for type_name, name, values in rows:
		if type_name != current:
			if current:
				lines.append("")
			lines.append(f"// {type_name}")
			current = type_name
		padded = values + ["0"] * (16 - len(values))
		lines.append(
			f"GDSC_HOST_CONSTANT({type_name}, {name}, {len(values)}, " + ", ".join(padded) + ")"
		)
	return "\n".join(lines) + "\n"


FUZZ_TYPES = [
	"Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i",
	"Rect2", "Rect2i", "Plane", "Quaternion", "AABB", "Basis",
	"Transform2D", "Transform3D", "Projection", "Color",
]
FUZZ_SCALARS = ["bool", "int", "float"]
FUZZ_ALL = set(FUZZ_TYPES) | set(FUZZ_SCALARS)

# No GDScript syntax for these.
UNSPELLABLE_OPERATORS = {"xor", "in"}

# RHS is Variant in the API; use the owning type instead.
VARIANT_RIGHT_OPERATORS = {"==", "!=", "and", "or"}

# Error on most inputs, not a compiler limitation.
FUZZ_METHOD_DENYLIST = {
	("Projection", "create_for_hmd"),       # eye index must be 1 or 2
	("Projection", "create_perspective_hmd"),
	("Basis", "looking_at"),                # errors on zero/parallel target
}


# Well-conditioned values so inversions and normalizations don't error.
FUZZ_SAMPLES = {
	"bool": ["true", "false"],
	"int": ["3", "-7", "0"],
	"float": ["1.5", "-2.25", "0.0"],
	"Vector2": ["Vector2(1.5, -2.25)", "Vector2(0.0, 1.0)", "Vector2(-3.5, 4.25)"],
	"Vector2i": ["Vector2i(3, -4)", "Vector2i(1, 1)", "Vector2i(-9, 12)"],
	"Vector3": ["Vector3(1.5, -2.25, 0.5)", "Vector3(0.0, 1.0, 0.0)", "Vector3(-3.5, 4.25, 6.0)"],
	"Vector3i": ["Vector3i(3, -4, 5)", "Vector3i(1, 1, 1)", "Vector3i(-9, 12, 7)"],
	"Vector4": ["Vector4(1.5, -2.25, 0.5, 2.0)", "Vector4(0.0, 1.0, 0.0, 1.0)", "Vector4(-3.5, 4.25, 6.0, -1.5)"],
	"Vector4i": ["Vector4i(3, -4, 5, 6)", "Vector4i(1, 1, 1, 1)", "Vector4i(-9, 12, 7, -2)"],
	"Rect2": ["Rect2(1.0, 2.0, 3.0, 4.0)", "Rect2(-2.5, -1.5, 5.0, 2.0)"],
	"Rect2i": ["Rect2i(1, 2, 3, 4)", "Rect2i(-2, -1, 5, 2)"],
	"Plane": ["Plane(Vector3(0.0, 1.0, 0.0), 2.0)", "Plane(Vector3(1.0, 0.0, 0.0), -1.5)"],
	"Quaternion": ["Quaternion(Vector3(0.0, 1.0, 0.0), 0.75)", "Quaternion(Vector3(1.0, 0.0, 0.0), -1.25)"],
	"AABB": ["AABB(Vector3(-1.0, -1.0, -1.0), Vector3(2.0, 3.0, 4.0))", "AABB(Vector3(0.5, 0.5, 0.5), Vector3(1.0, 1.0, 1.0))"],
	"Basis": ["Basis(Vector3(0.0, 1.0, 0.0), 0.75)", "Basis(Vector3(1.0, 0.0, 0.0), -1.25)"],
	"Transform2D": ["Transform2D(0.5, Vector2(1.0, 2.0))", "Transform2D(-1.25, Vector2(-3.0, 0.5))"],
	"Transform3D": [
		"Transform3D(Basis(Vector3(0.0, 1.0, 0.0), 0.75), Vector3(1.0, 2.0, 3.0))",
		"Transform3D(Basis(Vector3(1.0, 0.0, 0.0), -1.25), Vector3(-2.0, 0.5, 4.0))",
	],
	"Projection": [
		"Projection(Vector4(1.0, 0.0, 0.0, 0.0), Vector4(0.0, 1.0, 0.0, 0.0), Vector4(0.0, 0.0, 1.0, 0.0), Vector4(0.0, 0.0, 0.0, 1.0))",
		"Projection(Vector4(2.0, 0.0, 0.0, 0.0), Vector4(0.0, 0.5, 0.0, 0.0), Vector4(0.0, 0.0, 1.5, 0.0), Vector4(1.0, 2.0, 3.0, 1.0))",
	],
	"Color": ["Color(0.25, 0.5, 0.75, 1.0)", "Color(1.0, 0.0, 0.25, 0.5)"],
}

# Non-zero divisors for `/` and `%` (every component must be non-zero).
FUZZ_NONZERO = {
	"int": ["3", "-7", "11"],
	"float": ["1.5", "-2.25", "0.5"],
	"Vector2i": ["Vector2i(3, -4)", "Vector2i(1, 2)"],
	"Vector3i": ["Vector3i(3, -4, 5)", "Vector3i(1, 2, 3)"],
	"Vector4i": ["Vector4i(3, -4, 5, 6)", "Vector4i(1, 2, 3, 4)"],
}



def fuzz_rows(api: dict[str, object]) -> dict[str, dict]:
	"""The usable surface of every math builtin, keyed by type name."""
	missing = [t for t in FUZZ_TYPES + FUZZ_SCALARS if t not in FUZZ_SAMPLES]
	if missing:
		raise ValueError(f"no sample values for {', '.join(missing)}")
	by_name = {b["name"]: b for b in api["builtin_classes"]}
	table: dict[str, dict] = {}
	for type_name in FUZZ_TYPES:
		builtin = by_name[type_name]
		constructors = []
		for ctor in builtin.get("constructors", []):
			arguments = ctor.get("arguments", [])
			if not arguments:
				continue
			types = [a["type"] for a in arguments]
			if all(t in FUZZ_ALL for t in types):
				constructors.append(types)
		members = [
			[m["name"], m["type"]]
			for m in builtin.get("members", []) or []
			if m["type"] in FUZZ_ALL
		]
		constants = [
			[c["name"], c["type"]]
			for c in builtin.get("constants", []) or []
			if c["type"] in FUZZ_ALL
		]
		operators = []
		unary = []
		# Dedup: the API lists equality against both Variant and the concrete type.
		seen: set[tuple[str, str, str]] = set()
		for op in builtin.get("operators", []):
			name = op["name"]
			if name in UNSPELLABLE_OPERATORS:
				continue
			if op["return_type"] not in FUZZ_ALL:
				continue
			right = op.get("right_type")
			if right is None:
				unary.append([name, op["return_type"]])
				continue
			if right == "Variant":
				if name not in VARIANT_RIGHT_OPERATORS:
					continue
				right = type_name
			if right not in FUZZ_ALL:
				continue
			row = (name, right, op["return_type"])
			if row in seen:
				continue
			seen.add(row)
			operators.append(list(row))
		methods = []
		for method in builtin.get("methods", []) or []:
			if method.get("is_vararg"):
				continue
			if (type_name, method["name"]) in FUZZ_METHOD_DENYLIST:
				continue
			return_type = method.get("return_type")
			if return_type not in FUZZ_ALL:
				continue
			arguments = [a["type"] for a in method.get("arguments", [])]
			if not all(t in FUZZ_ALL for t in arguments):
				continue
			methods.append([
				method["name"], arguments, return_type,
				bool(method.get("is_static")),
			])
		table[type_name] = {
			"index": builtin.get("indexing_return_type") or None,
			"constructors": constructors,
			"members": members,
			"constants": constants,
			"operators": operators,
			"unary": unary,
			"methods": methods,
		}
	return table


def variant_api_json(api: dict[str, object]) -> str:
	document = {
		"godot": version_name(api),
		"comment": (
			"Generated from extension_api.json by scripts/generate_api_tables.py; "
			"do not edit."
		),
		"scalars": FUZZ_SCALARS,
		"samples": FUZZ_SAMPLES,
		"nonzero": FUZZ_NONZERO,
		"types": fuzz_rows(api),
	}
	return json.dumps(document, indent="\t", sort_keys=False) + "\n"


def variant_api_def(api: dict[str, object]) -> str:
	table = fuzz_rows(api)
	lines = [
		f"// Generated from extension_api.json, Godot {version_name(api)}; do not edit.",
		"// Args padded to eight with Nil. Each macro defaults to nothing.",
		"",
	]
	macros = [
		("GDSC_VARIANT_TYPE", "type, index_type"),
		("GDSC_VARIANT_CTOR", "type, argc, a0, a1, a2, a3, a4, a5, a6, a7"),
		("GDSC_VARIANT_MEMBER", "type, member, member_type"),
		("GDSC_VARIANT_CONSTANT", "type, constant, constant_type"),
		("GDSC_VARIANT_OPERATOR", "type, op, right_type, result_type"),
		("GDSC_VARIANT_UNARY", "type, op, result_type"),
		("GDSC_VARIANT_METHOD",
		 "type, method, is_static, result_type, argc, a0, a1, a2, a3, a4, a5, a6, a7"),
		("GDSC_VARIANT_SAMPLE", "type, expression"),
		("GDSC_VARIANT_NONZERO", "type, expression"),
	]
	for name, params in macros:
		lines.append(f"#ifndef {name}")
		lines.append(f"#define {name}({params})")
		lines.append("#endif")
	lines.append("")

	def padded(types: list[str]) -> str:
		filled = list(types) + ["Nil"] * (8 - len(types))
		return ", ".join(filled)

	for type_name in FUZZ_SCALARS:
		for expression in FUZZ_SAMPLES[type_name]:
			lines.append(f'GDSC_VARIANT_SAMPLE({type_name}, "{expression}")')
		for expression in FUZZ_NONZERO.get(type_name, []):
			lines.append(f'GDSC_VARIANT_NONZERO({type_name}, "{expression}")')
	lines.append("")

	for type_name, rows in table.items():
		lines.append(f"// -= {type_name} =-")
		lines.append(f"GDSC_VARIANT_TYPE({type_name}, {rows['index'] or 'Nil'})")
		for types in rows["constructors"]:
			if len(types) > 8:
				raise ValueError(f"{type_name} constructor takes more than eight arguments")
			lines.append(f"GDSC_VARIANT_CTOR({type_name}, {len(types)}, {padded(types)})")
		for member, member_type in rows["members"]:
			lines.append(f"GDSC_VARIANT_MEMBER({type_name}, {member}, {member_type})")
		for constant, constant_type in rows["constants"]:
			lines.append(f"GDSC_VARIANT_CONSTANT({type_name}, {constant}, {constant_type})")
		for op, right, result in rows["operators"]:
			lines.append(f'GDSC_VARIANT_OPERATOR({type_name}, "{op}", {right}, {result})')
		for op, result in rows["unary"]:
			lines.append(f'GDSC_VARIANT_UNARY({type_name}, "{op}", {result})')
		for expression in FUZZ_SAMPLES[type_name]:
			lines.append(f'GDSC_VARIANT_SAMPLE({type_name}, "{expression}")')
		for expression in FUZZ_NONZERO.get(type_name, []):
			lines.append(f'GDSC_VARIANT_NONZERO({type_name}, "{expression}")')
		for method, arguments, result, is_static in rows["methods"]:
			if len(arguments) > 8:
				raise ValueError(f"{type_name}.{method} takes more than eight arguments")
			lines.append(
				f"GDSC_VARIANT_METHOD({type_name}, {method}, {1 if is_static else 0}, "
				f"{result}, {len(arguments)}, {padded(arguments)})"
			)
		lines.append("")
	return "\n".join(lines).rstrip("\n") + "\n"


def update(path: Path, content: str, check: bool) -> bool:
	old = path.read_text() if path.exists() else None
	if old == content:
		return True
	if check:
		print(f"out of date: {path.relative_to(REPOSITORY)}", file=sys.stderr)
		return False
	path.write_text(content)
	print(f"wrote {path.relative_to(REPOSITORY)}")
	return True


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("--api", type=Path, default=DEFAULT_API)
	parser.add_argument("--check", action="store_true")
	args = parser.parse_args()
	api = json.loads(args.api.read_text())
	ok = update(COMPILER / "class_enums.def", class_enums(api), args.check)
	ok = update(COMPILER / "host_constants.def", host_constants(api), args.check) and ok
	ok = update(COMPILER / "tests/variant_api.def", variant_api_def(api), args.check) and ok
	ok = update(TESTS / "variant_api.json", variant_api_json(api), args.check) and ok
	return 0 if ok else 1


if __name__ == "__main__":
	raise SystemExit(main())
