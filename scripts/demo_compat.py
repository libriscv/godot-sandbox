#!/usr/bin/env python3
"""Exercise godot-demo-projects with GDScript and an alternate script language.

Original .gd files remain byte-identical. Generated peers convert known script
path literals as well as resource references, preserving typed script identity.
A state file makes conversion reversible and protects edits made in that mode.
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import dataclasses
import datetime as dt
import fnmatch
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys
import time
from collections import Counter
from typing import Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DEMOS_ROOT = REPO_ROOT.parent / "godot-demo-projects"
SCRIPT_MODE = "sgd"
SCRIPT_SUFFIX = ".sgd"
LANGUAGE_NAME = "SafeGDScript"
LIBRARY_NAME = "libgodot-riscv.so"
ENTRY_SYMBOL = "riscv_library_init"
MINIMUM_VERSION = "4.4"
DESCRIPTOR_NAME = "safegdscript.gdextension"
STATE_NAME = ".safegdscript-compat.json"
ADDON_REL = Path("addons/safegdscript_compat_harness")
EXTENSION_RESOURCE_PATH = "res://addons/safegdscript_compat_harness/safegdscript.gdextension"
REFERENCE_SUFFIXES = {
    ".cfg",
    ".gdextension",
    ".godot",
    ".ini",
    ".json",
    ".tscn",
    ".tres",
}
GD_SUFFIX = re.compile(rb"\.gd(?![A-Za-z0-9_])")
QUOTED_GD_PATH = re.compile(rb'''["'][^"'\r\n]*\.gd["']''')
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
DIAGNOSTIC_MARKERS = (
    "SCRIPT ERROR:",
    "Parse Error:",
    "Compile Error:",
    "SafeGDScript:",
    "ERROR:",
    "WARNING:",
)
DEFAULT_RUNTIME_SECONDS = 5.0
SIMULATED_FPS = 60

# Demos that this environment cannot judge: they fail the same way with plain
# GDScript, so comparing the two modes says nothing about SafeGDScript.  Pass
# --no-default-excludes to run them anyway.
ENVIRONMENT_EXCLUSIONS = {
    "xr/*": "needs an XR runtime and a headset",
    "mono/*": "C# scripts need a .NET-enabled Godot; no script loads in either mode",
    "audio/midi_piano": "aborts under --headless in either mode",
    "audio/text_to_speech": "the display server exposes no voices; null call in either mode",
    "compute/texture": "RenderingDevice is null under --headless; fails in either mode",
    "networking/webrtc_minimal": "no WebRTC extension is configured; fails in either mode",
    "mobile/android_iap": "the Google Play Billing addon is not part of the checkout",
}

# Leaks are reported as a count inside the message, so an unrelated drift of a
# few objects reads as a brand new diagnostic.  Compare the counts instead.
COUNTED_DIAGNOSTIC = re.compile(
    r"^(?:WARNING|ERROR): (\d+) (?=.*(?:RIDs? of type|RID allocations|"
    r"instances were leaked|resources? still in use))"
)
COUNT_TOLERANCE = 1.25
COUNT_SLACK = 2

# GDScript and SafeGDScript word a call on a null base differently.  Both mean
# the same thing, so give them one spelling and let the comparison cancel them.
NULL_CALL_FORMS = (
    re.compile(r"^SCRIPT ERROR: Cannot call method '([^']+)' on a null value\.$"),
    re.compile(
        r"^SCRIPT ERROR: Attempt to call function '([^']+)' in base "
        r"'null instance' on a null instance\.$"
    ),
    re.compile(
        r"^ERROR: (?:Exception: )?Variant::call\(\): Invalid call\. "
        r"Nonexistent function '([^']+)' in base 'Nil'\.$"
    ),
)


class HarnessError(RuntimeError):
    pass


@dataclasses.dataclass
class RunResult:
    project: str
    phase: str
    command: list[str]
    returncode: int | None
    timed_out: bool
    seconds: float
    diagnostics: list[str]
    log: str

    def as_dict(self) -> dict[str, object]:
        return dataclasses.asdict(self)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    existing_mode = stat.S_IMODE(path.stat().st_mode) if path.exists() else None
    temporary = path.with_name(path.name + ".safegdscript-tmp")
    temporary.write_bytes(data)
    if existing_mode is not None:
        temporary.chmod(existing_mode)
    os.replace(temporary, path)


def state_path(project: Path) -> Path:
    return project / STATE_NAME


def load_state(project: Path) -> dict[str, object] | None:
    path = state_path(project)
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HarnessError(f"cannot read {path}: {error}") from error


def save_state(project: Path, state: dict[str, object]) -> None:
    payload = (json.dumps(state, indent=2, sort_keys=True) + "\n").encode()
    atomic_write(state_path(project), payload)


def matches(relative: str, pattern: str) -> bool:
    return fnmatch.fnmatch(relative, pattern) or fnmatch.fnmatch(relative, f"*{pattern}*")


def discover_projects(
    root: Path, patterns: Sequence[str], excludes: Sequence[str] = ()
) -> list[Path]:
    if not root.is_dir():
        raise HarnessError(f"demo root does not exist: {root}")
    projects = sorted(path.parent for path in root.rglob("project.godot"))
    if patterns:
        projects = [
            project
            for project in projects
            if any(matches(project.relative_to(root).as_posix(), pattern) for pattern in patterns)
        ]
    if excludes:
        projects = [
            project
            for project in projects
            if not any(matches(project.relative_to(root).as_posix(), pattern) for pattern in excludes)
        ]
    if not projects:
        detail = f" matching {', '.join(patterns)}" if patterns else ""
        raise HarnessError(f"no projects found under {root}{detail}")
    return projects


def selected_excludes(args: argparse.Namespace) -> list[str]:
    excludes = list(getattr(args, "exclude", None) or [])
    if not getattr(args, "no_default_excludes", False):
        excludes.extend(ENVIRONMENT_EXCLUSIONS)
    return excludes


def project_name(project: Path, root: Path) -> str:
    return project.relative_to(root).as_posix()


def find_godot(explicit: str | None) -> Path:
    candidates: list[str] = []
    if explicit:
        candidates.append(explicit)
    if os.environ.get("GODOT"):
        candidates.append(os.environ["GODOT"])
    candidates.extend(("godot", "godot4"))
    for directory in (REPO_ROOT.parent, REPO_ROOT.parent.parent):
        candidates.extend(str(path) for path in sorted(directory.glob("Godot_v*-stable_linux.*"), reverse=True))
    for candidate in candidates:
        resolved = shutil.which(candidate) or candidate
        path = Path(resolved).expanduser()
        if path.is_file() and os.access(path, os.X_OK):
            return path.resolve()
    raise HarnessError("Godot was not found; pass --godot or set GODOT")


def find_extension(explicit: str | None) -> Path:
    candidates = [
        explicit,
        os.environ.get("SAFEGDSCRIPT_EXTENSION"),
        str(REPO_ROOT / ".build/libgodot-riscv.so"),
        str(REPO_ROOT / "bin/addons/godot_sandbox/bin/libgodot_riscv.linux.template_release.x86_64.so"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).expanduser().is_file():
            return Path(candidate).expanduser().resolve()
    raise HarnessError(
        "the SafeGDScript extension library was not found; build the project or pass --extension"
    )


def source_files(project: Path) -> list[Path]:
    files: list[Path] = []
    for path in project.rglob("*.gd"):
        if path.is_symlink():
            continue
        relative = path.relative_to(project)
        if ".godot" in relative.parts or ADDON_REL in relative.parents:
            continue
        files.append(path)
    return sorted(files)


def reference_files(project: Path) -> Iterable[Path]:
    for path in project.rglob("*"):
        if not path.is_file():
            continue
        if path.is_symlink():
            continue
        relative = path.relative_to(project)
        if ".godot" in relative.parts or ADDON_REL in relative.parents:
            continue
        if path.name == STATE_NAME:
            continue
        if path.name == "project.godot" or path.suffix.lower() in REFERENCE_SUFFIXES:
            yield path


def read_uid(path: Path) -> bytes | None:
    try:
        value = path.read_bytes().strip()
    except OSError:
        return None
    return value if value.startswith(b"uid://") else None


def uid_map(project: Path, scripts: Sequence[str]) -> dict[bytes, bytes]:
    mapping: dict[bytes, bytes] = {}
    for relative in scripts:
        gd = project / relative
        old_uid = read_uid(gd.with_name(gd.name + ".uid"))
        sgd = gd.with_suffix(SCRIPT_SUFFIX)
        new_uid = read_uid(sgd.with_name(sgd.name + ".uid"))
        if old_uid and new_uid and old_uid != new_uid:
            mapping[old_uid] = new_uid
    return mapping


def stage_extension(project: Path, library: Path) -> None:
    addon = project / ADDON_REL
    if addon.exists():
        raise HarnessError(f"generated addon path already exists: {addon}")
    addon.mkdir(parents=True)
    system = platform.system()
    machine = platform.machine().lower()
    if system != "Linux" or machine not in ("x86_64", "amd64"):
        raise HarnessError(f"automatic extension staging currently supports Linux x86_64, not {system} {machine}")
    link = addon / LIBRARY_NAME
    link.symlink_to(library)
    descriptor = f"""[configuration]

entry_symbol = "{ENTRY_SYMBOL}"
compatibility_minimum = "{MINIMUM_VERSION}"

[libraries]

linux.debug.x86_64 = "./{LIBRARY_NAME}"
linux.release.x86_64 = "./{LIBRARY_NAME}"
"""
    atomic_write(addon / DESCRIPTOR_NAME, descriptor.encode())

    godot_dir = project / ".godot"
    godot_dir.mkdir(exist_ok=True)
    extensions = godot_dir / "extension_list.cfg"
    existing = extensions.read_text(encoding="utf-8").splitlines() if extensions.exists() else []
    if EXTENSION_RESOURCE_PATH not in existing:
        existing.append(EXTENSION_RESOURCE_PATH)
        atomic_write(extensions, ("\n".join(existing) + "\n").encode())


def unstage_extension(project: Path) -> None:
    addon = project / ADDON_REL
    if addon.is_dir():
        shutil.rmtree(addon)
    extensions = project / ".godot/extension_list.cfg"
    if extensions.exists():
        lines = extensions.read_text(encoding="utf-8").splitlines()
        filtered = [line for line in lines if line.strip() != EXTENSION_RESOURCE_PATH]
        atomic_write(extensions, (("\n".join(filtered) + "\n") if filtered else "").encode())


def extract_diagnostics(output: str) -> list[str]:
    diagnostics: list[str] = []
    for raw_line in output.splitlines():
        line = ANSI.sub("", raw_line).strip()
        if any(marker in line for marker in DIAGNOSTIC_MARKERS):
            diagnostics.append(line)
    return diagnostics


def run_godot(
    godot: Path,
    project: Path,
    root: Path,
    phase: str,
    arguments: Sequence[str],
    timeout: float,
    log_dir: Path,
) -> RunResult:
    name = project_name(project, root)
    command = [str(godot), "--headless", "--path", str(project), *arguments]
    started = time.monotonic()
    timed_out = False
    returncode: int | None
    try:
        environment = os.environ.copy()
        environment_root = log_dir / ".godot-environment"
        for variable, child in (
            ("XDG_CACHE_HOME", "cache"),
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_DATA_HOME", "data"),
        ):
            location = environment_root / child
            location.mkdir(parents=True, exist_ok=True)
            environment[variable] = str(location)
        process = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=timeout,
            check=False,
            env=environment,
        )
        output = process.stdout
        returncode = process.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        returncode = None
        captured = error.stdout or ""
        output = captured.decode(errors="replace") if isinstance(captured, bytes) else captured
        output += f"\nHARNESS TIMEOUT after {timeout:.1f}s\n"
    seconds = time.monotonic() - started
    log_path = log_dir / name / f"{phase}.log"
    atomic_write(log_path, output.encode(errors="replace"))
    return RunResult(
        project=name,
        phase=phase,
        command=command,
        returncode=returncode,
        timed_out=timed_out,
        seconds=seconds,
        diagnostics=extract_diagnostics(output),
        log=str(log_path),
    )


def combine_imports(first: RunResult, second: RunResult) -> RunResult:
    """Score final-import diagnostics; retain bootstrap logs and process failures."""
    return RunResult(
        project=second.project,
        phase="import",
        command=second.command,
        returncode=first.returncode if first.returncode not in (0, None) else second.returncode,
        timed_out=first.timed_out or second.timed_out,
        seconds=first.seconds + second.seconds,
        diagnostics=second.diagnostics,
        log=f"{first.log}; {second.log}",
    )


# Match comments and complete strings first so a path-shaped fragment in a
# comment or a multiline message cannot be mistaken for a resource literal.
SCRIPT_TOKEN = re.compile(rb"\#[^\r\n]*|\"\"\"[\s\S]*?\"\"\"|\x27\x27\x27[\s\S]*?\x27\x27\x27|\"(?:\\.|[^\"\\])*\"|\x27(?:\\.|[^\x27\\])*\x27")


def converted_script(project: Path, script: Path, scripts: Sequence[Path]) -> bytes:
    known = {path.resolve() for path in scripts}

    def convert(match: re.Match[bytes]) -> bytes:
        token = match.group()
        if token[:1] == b"#" or token.startswith((b'"""', b"\x27\x27\x27")):
            return token
        path_bytes = token[1:-1]
        if not path_bytes.endswith(b".gd") or b"\\" in path_bytes:
            return token
        path = path_bytes.decode("utf-8")
        if path.startswith("res://"):
            target = project / path[6:]
        elif "://" in path or Path(path).is_absolute():
            return token
        else:
            target = script.parent / path
        if target.resolve() not in known:
            return token
        return token[:1] + path_bytes[:-3] + SCRIPT_SUFFIX.encode() + token[-1:]

    return SCRIPT_TOKEN.sub(convert, script.read_bytes())


def internal_gd_references(project: Path, scripts: Sequence[Path]) -> list[str]:
    references: list[str] = []
    for script in scripts:
        data = converted_script(project, script, scripts)
        if QUOTED_GD_PATH.search(data):
            references.append(script.relative_to(project).as_posix())
    return references


def to_safe_mode(
    project: Path,
    root: Path,
    godot: Path,
    library: Path,
    timeout: float,
    log_dir: Path,
) -> RunResult:
    existing = load_state(project)
    if existing:
        if existing.get("mode") != SCRIPT_MODE:
            raise HarnessError(f"{project}: incomplete Safe mode state; toggle back to gd first")
        return run_godot(godot, project, root, "import", ("--import",), timeout, log_dir)

    scripts = source_files(project)
    relatives = [path.relative_to(project).as_posix() for path in scripts]
    collisions = [path.with_suffix(SCRIPT_SUFFIX) for path in scripts if path.with_suffix(SCRIPT_SUFFIX).exists()]
    if collisions:
        raise HarnessError(f"{project}: refusing to replace existing {collisions[0]}")
    if (project / ADDON_REL).exists():
        raise HarnessError(f"{project}: generated addon path already exists")

    state: dict[str, object] = {
        "version": 2,
        "mode": "preparing",
        "scripts": relatives,
        "rewritten": [],
        "generated_sha256": {},
        "original_sha256": {str(path.relative_to(project)): sha256(path.read_bytes()) for path in scripts},
        "internal_gd_references": internal_gd_references(project, scripts),
    }
    save_state(project, state)
    try:
        for gd in scripts:
            converted = converted_script(project, gd, scripts)
            state["generated_sha256"][gd.relative_to(project).as_posix()] = sha256(converted)
        save_state(project, state)
        for gd in scripts:
            atomic_write(gd.with_suffix(SCRIPT_SUFFIX), converted_script(project, gd, scripts))
        stage_extension(project, library)
        # Only one format may participate in the global class scan. Keep the
        # original bytes in an ignored directory until restoration, otherwise
        # class_name can still resolve to .gd even after all literals change.
        originals = project / ADDON_REL / "originals"
        atomic_write(originals / ".gdignore", b"")
        for gd in scripts:
            backup = originals / gd.relative_to(project)
            backup.parent.mkdir(parents=True, exist_ok=True)
            gd.rename(backup)

        # This pass registers the extension and gives .sgd resources stable UIDs.
        bootstrap = run_godot(
            godot, project, root, "bootstrap-import", ("--import",), timeout, log_dir
        )
        uids = uid_map(project, relatives)
        rewritten: list[dict[str, str]] = []
        for path in reference_files(project):
            original = path.read_bytes()
            converted = GD_SUFFIX.sub(SCRIPT_SUFFIX.encode(), original)
            for old_uid, new_uid in uids.items():
                converted = converted.replace(old_uid, new_uid)
            if converted == original:
                continue
            rewritten.append(
                {
                    "path": path.relative_to(project).as_posix(),
                    "original": base64.b64encode(original).decode("ascii"),
                    "safe_sha256": sha256(converted),
                }
            )
            atomic_write(path, converted)
        state["rewritten"] = rewritten
        state["mode"] = SCRIPT_MODE
        save_state(project, state)
    except Exception:
        # The state file intentionally remains so `toggle gd` can recover.
        raise
    final_import = run_godot(
        godot, project, root, "import", ("--import",), timeout, log_dir
    )
    return combine_imports(bootstrap, final_import)


def to_gd_mode(
    project: Path,
    root: Path,
    godot: Path,
    timeout: float,
    log_dir: Path,
    reimport: bool = True,
) -> RunResult | None:
    state = load_state(project)
    if not state:
        if reimport:
            return run_godot(godot, project, root, "import", ("--import",), timeout, log_dir)
        return None

    for item in state.get("rewritten", []):
        if not isinstance(item, dict):
            raise HarnessError(f"{project}: invalid rewritten-file state")
        path = project / str(item["path"])
        current = path.read_bytes()
        expected = item.get("safe_sha256")
        if expected and sha256(current) != expected:
            raise HarnessError(f"{path} changed in Safe mode; refusing to overwrite it")
    for relative in state.get("scripts", []):
        gd = project / str(relative)
        backup = project / ADDON_REL / "originals" / str(relative)
        original_hash = state.get("original_sha256", {}).get(str(relative))
        if original_hash:
            original = backup if backup.exists() else gd
            if not original.exists() or sha256(original.read_bytes()) != original_hash:
                raise HarnessError(f"{original} changed in converted mode; refusing to overwrite it")
            if backup.exists() and gd.exists():
                raise HarnessError(f"{gd} was created in converted mode; refusing to overwrite it")
        sgd = gd.with_suffix(SCRIPT_SUFFIX)
        expected = state.get("generated_sha256", {}).get(str(relative))
        if expected is None and gd.exists():  # Recover version 1 byte-exact state.
            expected = sha256(gd.read_bytes())
        if sgd.exists() and (expected is None or sha256(sgd.read_bytes()) != expected):
            raise HarnessError(f"{sgd} changed in converted mode; refusing to remove it")

    for item in state.get("rewritten", []):
        path = project / str(item["path"])
        atomic_write(path, base64.b64decode(str(item["original"])))
    for relative in state.get("scripts", []):
        gd = project / str(relative)
        sgd = gd.with_suffix(SCRIPT_SUFFIX)
        uid = sgd.with_name(sgd.name + ".uid")
        if sgd.exists():
            sgd.unlink()
        if uid.exists():
            uid.unlink()
        backup = project / ADDON_REL / "originals" / str(relative)
        if backup.exists():
            backup.rename(gd)
    unstage_extension(project)
    state_path(project).unlink()
    if reimport:
        return run_godot(godot, project, root, "import", ("--import",), timeout, log_dir)
    return None


def mode_for(projects: Sequence[Path]) -> str:
    modes = {SCRIPT_MODE if load_state(project) else "gd" for project in projects}
    if len(modes) != 1:
        raise HarnessError("selected projects are in mixed modes; toggle them to one mode first")
    return modes.pop()


def result_failed(result: RunResult) -> bool:
    return result.timed_out or result.returncode != 0 or bool(result.diagnostics)


def print_result(result: RunResult) -> None:
    status = "FAIL" if result_failed(result) else "PASS"
    detail = "timeout" if result.timed_out else f"rc={result.returncode}"
    print(
        f"{status:4} {result.project:<48} {result.phase:<8} "
        f"{detail}, diagnostics={len(result.diagnostics)}, {result.seconds:.2f}s"
    )


def execute_projects(
    projects: Sequence[Path],
    root: Path,
    godot: Path,
    runtime_arguments: Sequence[str],
    timeout: float,
    jobs: int,
    log_dir: Path,
    import_projects: bool = True,
) -> list[RunResult]:
    def execute(project: Path) -> list[RunResult]:
        current: list[RunResult] = []
        if import_projects:
            current.append(
                run_godot(godot, project, root, "import", ("--import",), timeout, log_dir)
            )
        runtime = run_godot(
            godot,
            project,
            root,
            "run",
            runtime_arguments,
            timeout,
            log_dir,
        )
        current.append(runtime)
        return current

    results: list[RunResult] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {executor.submit(execute, project): project for project in projects}
        for future in concurrent.futures.as_completed(futures):
            try:
                current = future.result()
            except Exception as error:
                name = project_name(futures[future], root)
                print(f"FAIL {name}: {error}", file=sys.stderr)
                continue
            results.extend(current)
            for result in current:
                print_result(result)
    return sorted(results, key=lambda result: (result.project, result.phase))


def normalized_diagnostics(result: RunResult) -> Counter[str]:
    normalized: list[str] = []
    for line in result.diagnostics:
        # The harness stages only the extension, not the packaged editor icon.
        # It has no execution effect and is absent from the final runtime.
        if "res://addons/godot_sandbox/SafeGDScript.svg" in line:
            continue
        # Keeping byte-identical .gd oracles beside .sgd peers can make the
        # editor's initial global-class scan diagnose the duplicate. The final
        # Safe-mode scan resolves the .sgd class, so this is harness noise.
        if re.search(r'Parse Error: Class ".+" hides a global script class\.$', line):
            continue
        if SCRIPT_SUFFIX not in line and re.search(
            r'Failed to load script "res://.+\.gd" with error "Parse error"\.$', line
        ):
            continue
        line = line.replace(SCRIPT_SUFFIX, ".gd")
        line = re.sub(r"\b0x[0-9a-fA-F]+\b", "0xADDR", line)
        for form in NULL_CALL_FORMS:
            match = form.match(line)
            if match:
                line = f"NULL CALL: '{match.group(1)}'"
                break
        normalized.append(line)
    return Counter(normalized)


def diagnostic_shapes(result: RunResult) -> tuple[Counter[str], Counter[str]]:
    """Split diagnostics into message shapes and the object counts they report."""
    shapes: Counter[str] = Counter()
    counts: Counter[str] = Counter()
    for line, repeats in normalized_diagnostics(result).items():
        match = COUNTED_DIAGNOSTIC.match(line)
        if not match:
            shapes[line] += repeats
            continue
        shape = line[: match.start(1)] + "<N>" + line[match.end(1) :]
        shape = re.sub(r"\bRIDs\b", "RID", shape)
        shape = re.sub(r"\b(instance|resource|allocation)s\b", r"\1", shape)
        shape = re.sub(r"\bwere\b", "was", shape)
        shapes[shape] += repeats
        counts[shape] += int(match.group(1)) * repeats
    return shapes, counts


def new_diagnostics_for(baseline: RunResult | None, safe: RunResult) -> Counter[str]:
    """Diagnostics Safe mode adds, ignoring leak counts that barely moved."""
    safe_shapes, safe_counts = diagnostic_shapes(safe)
    if baseline is None:
        return safe_shapes
    base_shapes, base_counts = diagnostic_shapes(baseline)
    added = safe_shapes - base_shapes
    for shape, count in safe_counts.items():
        if shape in added or shape not in base_counts:
            continue
        before = base_counts[shape]
        if count > before * COUNT_TOLERANCE + COUNT_SLACK:
            added[shape.replace("<N>", f"{before} -> {count}")] += 1
    return added


def write_summary(path: Path, mode: str, results: Sequence[RunResult]) -> None:
    payload = {
        "mode": mode,
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "results": [result.as_dict() for result in results],
    }
    atomic_write(path, (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode())


def compare_results(
    baseline: Sequence[RunResult],
    safe: Sequence[RunResult],
    output: Path,
    mixed_references: dict[str, list[str]] | None = None,
) -> tuple[int, list[str]]:
    baseline_by_key = {(result.project, result.phase): result for result in baseline}
    lines = [
        f"# {LANGUAGE_NAME} demo compatibility",
        "",
        "| Project | Phase | Result | New diagnostics |",
        "|---|---:|---:|---:|",
    ]
    regressions = 0
    for result in safe:
        key = (result.project, result.phase)
        before = baseline_by_key.get(key)
        new_diagnostics = new_diagnostics_for(before, result)
        regressed = bool(new_diagnostics) or result.timed_out or result.returncode != 0
        if before and before.timed_out == result.timed_out and before.returncode == result.returncode:
            regressed = bool(new_diagnostics)
        label = "REGRESSION" if regressed else "PASS"
        regressions += int(regressed)
        lines.append(f"| `{result.project}` | {result.phase} | {label} | {sum(new_diagnostics.values())} |")
        if new_diagnostics:
            lines.append("")
            for diagnostic, count in new_diagnostics.items():
                suffix = f" (x{count})" if count > 1 else ""
                lines.append(f"  - `{diagnostic}`{suffix}")
            lines.append("")
    lines.extend(("", f"Regressed phases: **{regressions}**", ""))
    if mixed_references:
        lines.extend(
            (
                "## Unresolved script path literals",
                "",
                "These files retain `.gd` literals that could not be resolved to a local script. "
                "Dynamic script paths need manual review:",
                "",
            )
        )
        for project, scripts in sorted(mixed_references.items()):
            lines.append(f"- `{project}`: " + ", ".join(f"`{script}`" for script in scripts))
        lines.append("")
    atomic_write(output, "\n".join(lines).encode())
    return regressions, lines


def new_results_dir(base: Path, label: str) -> Path:
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = base / f"{stamp}-{label}"
    path.mkdir(parents=True, exist_ok=False)
    return path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=DEFAULT_DEMOS_ROOT, help="godot-demo-projects checkout")
    parser.add_argument("--godot", help="Godot editor executable (or set GODOT)")
    parser.add_argument("--extension", help=f"built {LANGUAGE_NAME} GDExtension library")
    parser.add_argument("--timeout", type=float, default=60.0, help="seconds allowed per Godot invocation")
    parser.add_argument(
        "--results",
        type=Path,
        default=REPO_ROOT / ".demo-compat-results",
        help="directory for logs and summaries",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    def add_selection_options(command: argparse.ArgumentParser) -> None:
        command.add_argument(
            "--exclude",
            action="append",
            metavar="PATTERN",
            help="skip projects matching this glob or substring (repeatable)",
        )
        command.add_argument(
            "--no-default-excludes",
            action="store_true",
            help="also run the demos this environment cannot judge",
        )

    listing = subparsers.add_parser("list", help="list selected demo projects")
    listing.add_argument("projects", nargs="*", help="relative path glob or substring")
    add_selection_options(listing)

    toggle = subparsers.add_parser("toggle", help="switch selected projects and re-import")
    toggle.add_argument("mode", choices=("gd", SCRIPT_MODE))
    toggle.add_argument("projects", nargs="*", help="relative path glob or substring")
    add_selection_options(toggle)

    def add_runtime_options(command: argparse.ArgumentParser) -> None:
        duration = command.add_mutually_exclusive_group()
        duration.add_argument(
            "--frames",
            type=int,
            help="runtime frames before Godot quits",
        )
        duration.add_argument(
            "--seconds",
            type=float,
            help=(
                f"simulated runtime seconds at a fixed {SIMULATED_FPS} FPS "
                f"(default: {DEFAULT_RUNTIME_SECONDS:g})"
            ),
        )
        command.add_argument("--jobs", type=int, default=1, help="projects to run concurrently")

    test = subparsers.add_parser("test", help="import and briefly run projects in their current mode")
    test.add_argument("projects", nargs="*", help="relative path glob or substring")
    add_runtime_options(test)
    add_selection_options(test)

    matrix = subparsers.add_parser("matrix", help=f"compare GDScript and {LANGUAGE_NAME}, then restore GDScript")
    matrix.add_argument("projects", nargs="*", help="relative path glob or substring")
    add_runtime_options(matrix)
    add_selection_options(matrix)
    return parser


def runtime_arguments(frames: int | None, seconds: float | None) -> tuple[str, ...]:
    if frames is not None and frames < 1:
        raise HarnessError("--frames must be at least 1")
    if frames is not None:
        return ("--quit-after", str(frames))
    seconds = DEFAULT_RUNTIME_SECONDS if seconds is None else seconds
    if not math.isfinite(seconds) or seconds <= 0:
        raise HarnessError("--seconds must be a finite number greater than 0")
    simulated_frames = math.ceil(seconds * SIMULATED_FPS)
    return (
        "--fixed-fps",
        str(SIMULATED_FPS),
        "--quit-after",
        str(simulated_frames),
    )


def validate_jobs(jobs: int) -> None:
    if jobs < 1:
        raise HarnessError("--jobs must be at least 1")


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    root = args.root.expanduser().resolve()
    excludes = selected_excludes(args)
    projects = discover_projects(root, args.projects, excludes)
    if args.command == "list":
        for project in projects:
            mode = SCRIPT_MODE if load_state(project) else "gd"
            print(f"{project_name(project, root)}\t{mode}")
        return 0

    godot = find_godot(args.godot)
    args.results = args.results.expanduser().resolve()
    args.results.mkdir(parents=True, exist_ok=True)
    if args.command == "toggle":
        log_dir = new_results_dir(args.results, f"toggle-{args.mode}")
        library = find_extension(args.extension) if args.mode == SCRIPT_MODE else None
        failures = 0
        for project in projects:
            try:
                if args.mode == SCRIPT_MODE:
                    assert library is not None
                    result = to_safe_mode(project, root, godot, library, args.timeout, log_dir)
                else:
                    result = to_gd_mode(project, root, godot, args.timeout, log_dir)
                if result:
                    print_result(result)
                    failures += int(result_failed(result))
                state = load_state(project)
                if state and state.get("internal_gd_references"):
                    count = len(state["internal_gd_references"])  # type: ignore[arg-type]
                    print(f"NOTE {project_name(project, root)}: {count} scripts retain unresolved .gd path literals")
            except Exception as error:
                failures += 1
                print(f"FAIL {project_name(project, root)}: {error}", file=sys.stderr)
        print(f"logs: {log_dir}")
        return 1 if failures else 0

    validate_jobs(args.jobs)
    run_arguments = runtime_arguments(args.frames, args.seconds)
    if args.command == "test":
        mode = mode_for(projects)
        log_dir = new_results_dir(args.results, mode)
        results = execute_projects(
            projects, root, godot, run_arguments, args.timeout, args.jobs, log_dir
        )
        write_summary(log_dir / "summary.json", mode, results)
        failures = sum(result_failed(result) for result in results)
        print(f"{failures}/{len(results)} phases reported diagnostics or failed; results: {log_dir}")
        return 1 if failures else 0


    # Matrix always restores the checkout to GDScript, including after an error.
    library = find_extension(args.extension)
    matrix_dir = new_results_dir(args.results, "matrix")
    baseline_dir = matrix_dir / "gd"
    safe_dir = matrix_dir / SCRIPT_MODE
    baseline_dir.mkdir()
    safe_dir.mkdir()
    baseline: list[RunResult] = []
    safe: list[RunResult] = []
    mixed_references: dict[str, list[str]] = {}
    try:
        for project in projects:
            to_gd_mode(project, root, godot, args.timeout, baseline_dir, reimport=False)
        baseline = execute_projects(
            projects, root, godot, run_arguments, args.timeout, args.jobs, baseline_dir
        )
        write_summary(baseline_dir / "summary.json", "gd", baseline)
        safe_imports: list[RunResult] = []
        for project in projects:
            result = to_safe_mode(project, root, godot, library, args.timeout, safe_dir)
            safe_imports.append(result)
            print_result(result)
            state = load_state(project)
            references = state.get("internal_gd_references", []) if state else []
            if references:
                mixed_references[project_name(project, root)] = [str(path) for path in references]
        safe_runs = execute_projects(
            projects,
            root,
            godot,
            run_arguments,
            args.timeout,
            args.jobs,
            safe_dir,
            import_projects=False,
        )
        safe = safe_imports + safe_runs
        write_summary(safe_dir / "summary.json", SCRIPT_MODE, safe)
    finally:
        for project in projects:
            try:
                to_gd_mode(project, root, godot, args.timeout, matrix_dir, reimport=False)
            except Exception as error:
                print(f"FAIL restoring {project_name(project, root)}: {error}", file=sys.stderr)
    regressions, _ = compare_results(
        baseline,
        safe,
        matrix_dir / "comparison.md",
        mixed_references=mixed_references,
    )
    print(f"{regressions} regressed phases; results: {matrix_dir}")
    return 1 if regressions else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except HarnessError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
