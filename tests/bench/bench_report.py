#!/usr/bin/env python3
"""Build the benchmark suite's final tables from the results of both modes.

The guest's execution mode is a property of a whole process -- libriscv caches a
translated execute segment per binary, so a program once loaded with the JIT off
stays interpreted for the rest of the run -- which means JIT and interpreter
numbers can only come from separate runs of the suite. This script reads every
run of both and joins them, so that the comparison the suite exists to make is
in one table:

    GDScript, SafeGDScript with the JIT, SafeGDScript interpreted.

The third column is the one that matters for the web export, where Godot has no
JIT to offer and the interpreter is what a mod actually runs on.

Samples are pooled across the runs of a mode rather than taken from one of them.
Sampling inside a process is stable to about a percent; where the loader and the
allocator put things moves some kernels by ten times that, so a P90 from a
single process reports a precision the number does not have.

Output is Markdown, printed and written to results/report.md, because the tables
are meant for the docs site.
"""

import argparse
import glob
import json
import os
import re
import sys

REFERENCE_DEFAULT = "GDScript (engine)"
GUEST = "SafeGDScript (sandbox)"

MODES = (("jit", "JIT", "latest.json"), ("nojit", "no JIT", "latest-nojit.json"))

# A row whose P90 sits this far above its P50 is reported but flagged rather
# than quoted.
SPREAD_WARN = 0.15

# How far apart the runs of one mode may put the same case, measured as the
# spread of their medians. This is layout and placement, not the code.
RUN_SPREAD_WARN = 0.08


def percentile(values, p):
    """Linear interpolation between ranks, matching the harness."""
    if not values:
        return None
    if len(values) == 1:
        return values[0]
    idx = p * (len(values) - 1)
    lo, hi = int(idx), min(int(idx) + 1, len(values) - 1)
    t = idx - lo
    return values[lo] * (1.0 - t) + values[hi] * t


def load(path):
    if not os.path.exists(path):
        return None
    with open(path) as f:
        try:
            return json.load(f)
        except json.JSONDecodeError as exc:
            sys.stderr.write("bench_report: %s is not valid JSON (%s)\n" % (path, exc))
            return None


def load_mode(results_dir, mode, fallback_name):
    """Every run of one mode. The per-run archive if the runner wrote one,
    otherwise the single latest file, so a hand-run suite still reports."""
    paths = sorted(
        glob.glob(os.path.join(results_dir, "runs", "%s-*.json" % mode)),
        key=lambda p: int(re.search(r"-(\d+)\.json$", p).group(1)))
    runs = [r for r in (load(p) for p in paths) if r]
    if runs:
        return runs
    single = load(os.path.join(results_dir, fallback_name))
    return [single] if single else []


class Mode:
    """One execution mode's runs, pooled."""

    def __init__(self, name, runs):
        self.name = name
        self.runs = runs
        self.pooled = {}
        for run in runs:
            for group, entry in run.get("groups", {}).items():
                g = self.pooled.setdefault(group, {
                    "unit": entry.get("unit", "op"),
                    "reference": entry.get("reference", REFERENCE_DEFAULT),
                    "notes": entry.get("notes", {}),
                    "order": [],
                    "cases": {},
                })
                for label in entry.get("order", list(entry.get("cases", {}))):
                    stats = entry.get("cases", {}).get(label)
                    if not isinstance(stats, dict):
                        continue
                    if label not in g["order"]:
                        g["order"].append(label)
                    case = g["cases"].setdefault(label, {"samples": [], "medians": []})
                    samples = stats.get("samples_ns")
                    if samples:
                        case["samples"] += [float(s) for s in samples]
                        case["medians"].append(percentile(sorted(float(s) for s in samples), 0.5))
                    elif isinstance(stats.get("p50"), (int, float)):
                        # A results file from before the samples were kept.
                        case["samples"].append(float(stats["p50"]))
                        case["medians"].append(float(stats["p50"]))
        for g in self.pooled.values():
            for case in g["cases"].values():
                case["samples"].sort()

    def __bool__(self):
        return bool(self.runs)

    def env(self, key, default=None):
        return self.runs[0].get("_environment", {}).get(key, default) if self.runs else default

    def case(self, group, label):
        return self.pooled.get(group, {}).get("cases", {}).get(label)

    def stat(self, group, label, key="p50"):
        case = self.case(group, label)
        if not case or not case["samples"]:
            return None
        s = case["samples"]
        if key == "min":
            return s[0]
        if key == "p50":
            return percentile(s, 0.5)
        if key == "p90":
            return percentile(s, 0.9)
        if key == "spread":
            p50, p90 = percentile(s, 0.5), percentile(s, 0.9)
            return (p90 - p50) / p50 if p50 else None
        if key == "run_spread":
            # How far apart the runs put this case: the range of their medians.
            m = case["medians"]
            return (max(m) - min(m)) / min(m) if len(m) > 1 and min(m) else None
        return None

    def meta(self, group, key, default=None):
        return self.pooled.get(group, {}).get(key, default)


def ns(value):
    if value is None:
        return "-"
    if value >= 100:
        return "%.0f" % value
    if value >= 10:
        return "%.1f" % value
    if value >= 1:
        return "%.2f" % value
    return "%.3f" % value


def ratio(reference, value):
    """How many times faster `value` is than `reference`. Below 1.00x is slower."""
    if not reference or not value:
        return "-"
    return "%.2fx" % (reference / value)


def pct(value):
    return "-" if value is None else "%.1f%%" % (value * 100)


def table(rows, header, align=None):
    widths = [len(h) for h in header]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(str(cell)))
    align = align or ["left"] * len(header)
    sep = []
    for i, a in enumerate(align):
        sep.append(("-" * (widths[i] + 1) + ":") if a == "right" else ("-" * (widths[i] + 2)))

    def line(cells):
        out = []
        for i, cell in enumerate(cells):
            text = str(cell)
            out.append(text.rjust(widths[i]) if align[i] == "right" else text.ljust(widths[i]))
        return "| " + " | ".join(out) + " |"

    return "\n".join([line(header), "|" + "|".join(sep) + "|"] + [line(r) for r in rows])


def ordered_groups(modes):
    order = []
    for mode in modes:
        for name in mode.pooled:
            if name not in order:
                order.append(name)
    return order


def case_order(modes, group):
    order = []
    for mode in modes:
        for label in mode.meta(group, "order", []):
            if label not in order:
                order.append(label)
    return order


def meta(modes, group, key, default=None):
    for mode in modes:
        value = mode.meta(group, key)
        if value not in (None, {}, []):
            return value
    return default


def summary_section(modes, names):
    jit, nojit = modes
    out = [
        "## Summary",
        "",
        "Median nanoseconds per work unit, pooled over every run of a mode. Each",
        "speedup divides two numbers measured in the same process, which is what makes",
        "the two columns comparable: a process's layout shifts its absolute numbers by",
        "a few percent either way, and dividing within a run cancels it. Below 1.00x is",
        "slower than GDScript.",
        "",
    ]
    rows = []
    for group in names:
        reference = meta(modes, group, "reference", REFERENCE_DEFAULT)
        unit = meta(modes, group, "unit", "op")
        rows.append([
            group, unit,
            ns(jit.stat(group, reference) or nojit.stat(group, reference)),
            ns(jit.stat(group, GUEST)),
            ns(nojit.stat(group, GUEST)),
            ratio(jit.stat(group, reference), jit.stat(group, GUEST)),
            ratio(nojit.stat(group, reference), nojit.stat(group, GUEST)),
        ])
    out.append(table(
        rows,
        ["benchmark", "unit", "GDScript", "JIT", "no JIT", "JIT vs GDScript", "no JIT vs GDScript"],
        ["left", "left", "right", "right", "right", "right", "right"]))
    out.append("")
    out.append("The `GDScript` column is the JIT runs' measurement of the engine; the")
    out.append("interpreter runs measure it again, and how far apart the two land is under")
    out.append("measurement quality below.")
    out.append("")
    return out


def detail_sections(modes, names):
    out = ["## Per-benchmark detail", ""]
    for group in names:
        unit = meta(modes, group, "unit", "op")
        reference = meta(modes, group, "reference", REFERENCE_DEFAULT)
        notes = meta(modes, group, "notes", {}) or {}
        out.append("### %s" % group)
        out.append("")
        if notes:
            out.append("`" + ", ".join("%s = %s" % (k, v) for k, v in sorted(notes.items())) + "`")
            out.append("")
        rows = []
        for label in case_order(modes, group):
            row = [label]
            for mode in modes:
                row += [
                    ns(mode.stat(group, label, "min")),
                    ns(mode.stat(group, label)),
                    ns(mode.stat(group, label, "p90")),
                    ratio(mode.stat(group, reference), mode.stat(group, label)),
                ]
            rows.append(row)
        out.append(table(
            rows,
            ["case",
             "JIT min", "JIT P50", "JIT P90", "JIT vs ref",
             "no JIT min", "no JIT P50", "no JIT P90", "no JIT vs ref"],
            ["left"] + ["right"] * 8))
        out.append("")
        out.append("ns/%s. `vs ref` compares against `%s` measured in the same runs."
                   % (unit, reference))
        out.append("")
    return out


def quality_section(modes, names):
    """What the tables above are worth: how far apart the runs of one mode put
    the same case, and whether the two modes agree about GDScript."""
    jit, nojit = modes
    out = ["## Measurement quality", ""]

    runs = " and ".join("%d %s run%s" % (len(m.runs), m.name, "" if len(m.runs) == 1 else "s")
                        for m in modes if m)
    out.append("Pooled from %s." % (runs or "nothing"))
    out.append("")
    single = [m.name for m in modes if m and len(m.runs) < 2]
    if single:
        out.append("> Only one run of %s. A P90 from a single process describes sampling inside"
                   % " and ".join(single))
        out.append("> it and not where it happened to land, which is the larger of the two;")
        out.append("> run with `--repeat` before quoting that column.")
        out.append("")

    spreads = []
    for group in names:
        for label in case_order(modes, group):
            for mode in modes:
                value = mode.stat(group, label, "run_spread")
                if value is not None:
                    spreads.append((value, group, label, mode.name))
    if spreads:
        spreads.sort(reverse=True)
        median = spreads[len(spreads) // 2][0]
        out.append("Run-to-run: the same case measured by different processes of the same mode,")
        out.append("as the range of their medians. This is layout and placement, not the code.")
        out.append("")
        out.append("- median across all cases: **%s**" % pct(median))
        out.append("- worst: **%s**, `%s` / %s in %s" % (
            pct(spreads[0][0]), spreads[0][1], spreads[0][2], spreads[0][3]))
        out.append("")
        over = [[g, l, m, pct(v)] for v, g, l, m in spreads if v > RUN_SPREAD_WARN]
        if over:
            out.append("Cases the runs disagreed about by more than %s:" % pct(RUN_SPREAD_WARN))
            out.append("")
            out.append(table(over, ["benchmark", "case", "mode", "run-to-run range"],
                             ["left", "left", "left", "right"]))
            out.append("")

    deltas = []
    for group in names:
        reference = meta(modes, group, "reference", REFERENCE_DEFAULT)
        a, b = jit.stat(group, reference), nojit.stat(group, reference)
        if a and b:
            deltas.append((abs(a - b) / a, group))
    if deltas:
        deltas.sort(reverse=True)
        out.append("Across modes: GDScript is measured by both and cannot be affected by the")
        out.append("sandbox's execution mode, so the two should agree. Median gap **%s**, worst"
                   % pct(deltas[len(deltas) // 2][0]))
        out.append("**%s** on `%s`. No speedup above divides across this boundary; it bounds"
                   % (pct(deltas[0][0]), deltas[0][1]))
        out.append("how precisely a JIT `ns` figure and a no-JIT one may be compared directly.")
        out.append("")

    noisy = []
    for group in names:
        for label in case_order(modes, group):
            for mode in modes:
                spread = mode.stat(group, label, "spread")
                if spread is not None and spread > SPREAD_WARN:
                    noisy.append([group, label, mode.name, pct(spread)])
    if noisy:
        out.append("Rows whose pooled P90 sits more than %s above their P50:" % pct(SPREAD_WARN))
        out.append("")
        out.append(table(noisy, ["benchmark", "case", "mode", "P90 over P50"],
                         ["left", "left", "left", "right"]))
        out.append("")
    else:
        out.append("Every row's pooled P90 is within %s of its P50." % pct(SPREAD_WARN))
        out.append("")
    return out


def environment_section(modes):
    jit, nojit = modes
    out = ["## Environment", ""]
    env_mode = jit if jit else nojit
    rows = [
        ("CPU", env_mode.env("cpu", "?")),
        ("Godot", env_mode.env("godot", "?")),
        ("Cores", env_mode.env("cpus") or "not pinned"),
        ("Rounds per case per run", env_mode.env("samples", "?")),
        ("Minimum sample", "%d ms" % (env_mode.env("min_sample_usec", 0) / 1000)),
    ]
    for mode, expected in ((jit, "JIT"), (nojit, "interpreter")):
        if not mode:
            rows.append((mode.name + " runs", "missing"))
            continue
        observed = sorted({m.get("_environment", {}).get("mode", "?") for m in mode.runs})
        rows.append(("%s runs" % mode.name,
                     "%d, guest mode: %s" % (len(mode.runs), ", ".join(observed))))
    out.append(table([[k, v] for k, v in rows], ["", ""]))
    out.append("")

    # The runs must have measured what they claim to, or the tables compare a
    # mode against itself.
    for mode, expected in ((jit, "JIT"), (nojit, "interpreter")):
        if not mode:
            continue
        wrong = sorted({m.get("_environment", {}).get("mode", "?") for m in mode.runs}
                       - {expected})
        if wrong:
            out.append("> The %s runs report the guest ran in %s. Those columns are not %s"
                       % (mode.name, " and ".join("'%s'" % w for w in wrong), expected))
            out.append("> numbers.")
            out.append("")
    cpus = {m.env("cpu") for m in modes if m}
    if len(cpus) > 1:
        out.append("> The runs were measured on different machines (%s); they do not compare."
                   % ", ".join("'%s'" % c for c in sorted(cpus)))
        out.append("")
    return out


def build(modes):
    jit, nojit = modes
    names = ordered_groups(modes)
    out = ["# SafeGDScript benchmarks", ""]
    if not jit:
        out.append("> No JIT run; run `./run_benchmarks.sh`.\n")
    if not nojit:
        out.append("> No interpreter run; run `./run_benchmarks.sh --no-jit`. Without it there")
        out.append("> is nothing to say about the web export, which has no JIT.\n")
    if not names:
        out.append("No results.")
        return "\n".join(out) + "\n"
    out += summary_section(modes, names)
    out += detail_sections(modes, names)
    out += quality_section(modes, names)
    out += environment_section(modes)
    return "\n".join(out).rstrip() + "\n"


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results-dir", default=os.path.join(here, "results"))
    ap.add_argument("--out", default=None, help="default: <results-dir>/report.md")
    ap.add_argument("--quiet", action="store_true", help="write the file without printing it")
    args = ap.parse_args()

    modes = tuple(Mode(label, load_mode(args.results_dir, mode, fallback))
                  for mode, label, fallback in MODES)
    if not any(modes):
        sys.stderr.write("bench_report: no results in %s -- run ./run_benchmarks.sh first\n"
                         % args.results_dir)
        return 1

    report = build(modes)
    out_path = args.out or os.path.join(args.results_dir, "report.md")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w") as f:
        f.write(report)
    if not args.quiet:
        print(report)
    sys.stderr.write("bench_report: wrote %s\n" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
