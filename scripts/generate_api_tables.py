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
	return 0 if ok else 1


if __name__ == "__main__":
	raise SystemExit(main())
