#!/usr/bin/env python3
"""Exercise godot-demo-projects with byte-identical GDScript/SafeGDScript.

The harness deliberately never edits script contents.  Safe mode creates an
``.sgd`` peer for every ``.gd`` file and switches references in Godot resource
files.  A small state file in each project makes the operation reversible and
guards against overwriting edits made while Safe mode is active.
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


def discover_projects(root: Path, patterns: Sequence[str]) -> list[Path]:
    if not root.is_dir():
        raise HarnessError(f"demo root does not exist: {root}")
    projects = sorted(path.parent for path in root.rglob("project.godot"))
    if patterns:
        selected: list[Path] = []
        for project in projects:
            relative = project.relative_to(root).as_posix()
            if any(
                fnmatch.fnmatch(relative, pattern)
                or fnmatch.fnmatch(relative, f"*{pattern}*")
                for pattern in patterns
            ):
                selected.append(project)
        projects = selected
    if not projects:
        detail = f" matching {', '.join(patterns)}" if patterns else ""
        raise HarnessError(f"no projects found under {root}{detail}")
    return projects


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
        sgd = gd.with_suffix(".sgd")
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
    link = addon / "libgodot-riscv.so"
    link.symlink_to(library)
    descriptor = b"""[configuration]

entry_symbol = "riscv_library_init"
compatibility_minimum = "4.4"

[libraries]

linux.debug.x86_64 = "./libgodot-riscv.so"
linux.release.x86_64 = "./libgodot-riscv.so"
"""
    atomic_write(addon / "safegdscript.gdextension", descriptor)

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
    """Keep diagnostics seen in either import without double-counting repeats."""
    counts: Counter[str] = Counter(first.diagnostics)
    for diagnostic, count in Counter(second.diagnostics).items():
        counts[diagnostic] = max(counts[diagnostic], count)
    diagnostics = [diagnostic for diagnostic, count in counts.items() for _ in range(count)]
    return RunResult(
        project=second.project,
        phase="import",
        command=second.command,
        returncode=first.returncode if first.returncode not in (0, None) else second.returncode,
        timed_out=first.timed_out or second.timed_out,
        seconds=first.seconds + second.seconds,
        diagnostics=diagnostics,
        log=f"{first.log}; {second.log}",
    )


def internal_gd_references(project: Path, scripts: Sequence[Path]) -> list[str]:
    references: list[str] = []
    for script in scripts:
        data = script.read_bytes()
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
        if existing.get("mode") != "sgd":
            raise HarnessError(f"{project}: incomplete Safe mode state; toggle back to gd first")
        return run_godot(godot, project, root, "import", ("--import",), timeout, log_dir)

    scripts = source_files(project)
    relatives = [path.relative_to(project).as_posix() for path in scripts]
    collisions = [path.with_suffix(".sgd") for path in scripts if path.with_suffix(".sgd").exists()]
    if collisions:
        raise HarnessError(f"{project}: refusing to replace existing {collisions[0]}")
    if (project / ADDON_REL).exists():
        raise HarnessError(f"{project}: generated addon path already exists")

    state: dict[str, object] = {
        "version": 1,
        "mode": "preparing",
        "scripts": relatives,
        "rewritten": [],
        "internal_gd_references": internal_gd_references(project, scripts),
    }
    save_state(project, state)
    try:
        for gd in scripts:
            shutil.copy2(gd, gd.with_suffix(".sgd"))
        stage_extension(project, library)

        # This pass registers the extension and gives .sgd resources stable UIDs.
        bootstrap = run_godot(
            godot, project, root, "bootstrap-import", ("--import",), timeout, log_dir
        )
        uids = uid_map(project, relatives)
        rewritten: list[dict[str, str]] = []
        for path in reference_files(project):
            original = path.read_bytes()
            converted = GD_SUFFIX.sub(b".sgd", original)
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
        state["mode"] = "sgd"
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
        sgd = gd.with_suffix(".sgd")
        if sgd.exists() and gd.exists() and sha256(sgd.read_bytes()) != sha256(gd.read_bytes()):
            raise HarnessError(f"{sgd} is no longer byte-identical to {gd}; refusing to remove it")

    for item in state.get("rewritten", []):
        path = project / str(item["path"])
        atomic_write(path, base64.b64decode(str(item["original"])))
    for relative in state.get("scripts", []):
        gd = project / str(relative)
        sgd = gd.with_suffix(".sgd")
        uid = sgd.with_name(sgd.name + ".uid")
        if sgd.exists():
            sgd.unlink()
        if uid.exists():
            uid.unlink()
    unstage_extension(project)
    state_path(project).unlink()
    if reimport:
        return run_godot(godot, project, root, "import", ("--import",), timeout, log_dir)
    return None


def mode_for(projects: Sequence[Path]) -> str:
    modes = {"sgd" if load_state(project) else "gd" for project in projects}
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
    frames: int,
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
            ("--quit-after", str(frames)),
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
        # Keeping byte-identical .gd oracles beside .sgd peers can make the
        # editor's initial global-class scan diagnose the duplicate. The final
        # Safe-mode scan resolves the .sgd class, so this is harness noise.
        if re.search(r'Parse Error: Class ".+" hides a global script class\.$', line):
            continue
        if ".sgd" not in line and re.search(
            r'Failed to load script "res://.+\.gd" with error "Parse error"\.$', line
        ):
            continue
        line = line.replace(".sgd", ".gd")
        line = re.sub(r"\b0x[0-9a-fA-F]+\b", "0xADDR", line)
        normalized.append(line)
    return Counter(normalized)


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
        "# SafeGDScript demo compatibility",
        "",
        "| Project | Phase | Result | New diagnostics |",
        "|---|---:|---:|---:|",
    ]
    regressions = 0
    for result in safe:
        key = (result.project, result.phase)
        before = baseline_by_key.get(key)
        new_diagnostics = normalized_diagnostics(result)
        if before:
            new_diagnostics -= normalized_diagnostics(before)
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
                "## Byte-exact coverage notes",
                "",
                "These SafeGDScript files still load a `.gd` path because rewriting the "
                "literal would change the oracle bytes:",
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
    parser.add_argument("--extension", help="built SafeGDScript GDExtension library")
    parser.add_argument("--timeout", type=float, default=60.0, help="seconds allowed per Godot invocation")
    parser.add_argument(
        "--results",
        type=Path,
        default=REPO_ROOT / ".demo-compat-results",
        help="directory for logs and summaries",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    listing = subparsers.add_parser("list", help="list selected demo projects")
    listing.add_argument("projects", nargs="*", help="relative path glob or substring")

    toggle = subparsers.add_parser("toggle", help="switch selected projects and re-import")
    toggle.add_argument("mode", choices=("gd", "sgd"))
    toggle.add_argument("projects", nargs="*", help="relative path glob or substring")

    test = subparsers.add_parser("test", help="import and briefly run projects in their current mode")
    test.add_argument("projects", nargs="*", help="relative path glob or substring")
    test.add_argument("--frames", type=int, default=120, help="runtime frames before Godot quits")
    test.add_argument("--jobs", type=int, default=1, help="projects to run concurrently")

    matrix = subparsers.add_parser("matrix", help="compare GDScript and SafeGDScript, then restore GDScript")
    matrix.add_argument("projects", nargs="*", help="relative path glob or substring")
    matrix.add_argument("--frames", type=int, default=120, help="runtime frames before Godot quits")
    matrix.add_argument("--jobs", type=int, default=1, help="projects to run concurrently")
    return parser


def validate_run_options(frames: int, jobs: int) -> None:
    if frames < 1:
        raise HarnessError("--frames must be at least 1")
    if jobs < 1:
        raise HarnessError("--jobs must be at least 1")


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    root = args.root.expanduser().resolve()
    projects = discover_projects(root, args.projects)
    if args.command == "list":
        for project in projects:
            mode = "sgd" if load_state(project) else "gd"
            print(f"{project_name(project, root)}\t{mode}")
        return 0

    godot = find_godot(args.godot)
    args.results.mkdir(parents=True, exist_ok=True)
    if args.command == "toggle":
        log_dir = new_results_dir(args.results, f"toggle-{args.mode}")
        library = find_extension(args.extension) if args.mode == "sgd" else None
        failures = 0
        for project in projects:
            try:
                if args.mode == "sgd":
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
                    print(f"NOTE {project_name(project, root)}: {count} scripts retain byte-exact .gd path literals")
            except Exception as error:
                failures += 1
                print(f"FAIL {project_name(project, root)}: {error}", file=sys.stderr)
        print(f"logs: {log_dir}")
        return 1 if failures else 0

    validate_run_options(args.frames, args.jobs)
    if args.command == "test":
        mode = mode_for(projects)
        log_dir = new_results_dir(args.results, mode)
        results = execute_projects(projects, root, godot, args.frames, args.timeout, args.jobs, log_dir)
        write_summary(log_dir / "summary.json", mode, results)
        failures = sum(result_failed(result) for result in results)
        print(f"{failures}/{len(results)} phases reported diagnostics or failed; results: {log_dir}")
        return 1 if failures else 0


    # Matrix always restores the checkout to GDScript, including after an error.
    library = find_extension(args.extension)
    matrix_dir = new_results_dir(args.results, "matrix")
    baseline_dir = matrix_dir / "gd"
    safe_dir = matrix_dir / "sgd"
    baseline_dir.mkdir()
    safe_dir.mkdir()
    baseline: list[RunResult] = []
    safe: list[RunResult] = []
    mixed_references: dict[str, list[str]] = {}
    try:
        for project in projects:
            to_gd_mode(project, root, godot, args.timeout, baseline_dir, reimport=False)
        baseline = execute_projects(projects, root, godot, args.frames, args.timeout, args.jobs, baseline_dir)
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
            args.frames,
            args.timeout,
            args.jobs,
            safe_dir,
            import_projects=False,
        )
        safe = safe_imports + safe_runs
        write_summary(safe_dir / "summary.json", "sgd", safe)
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
