#!/usr/bin/env python3
"""Build the benchmark suite's final tables from every available guest mode.

The guest's execution mode is a property of a whole process -- libriscv caches a
translated execute segment per binary, so Full, JIT and interpreter numbers can
only come from separate runs of the suite. Full is also baked in its own process.
This script reads every run and joins them into one table:

    GDScript, SafeGDScript Full, with the JIT, and interpreted.

The interpreter column matters for the web export, where Godot has no JIT to
offer and the interpreter is what a mod runs without an embedded translation.

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

MODES = (("full", "Full", "latest-full.json"),
         ("jit", "JIT", "latest.json"),
         ("nojit", "Intrp", "latest-nojit.json"))

# A row whose P90 sits this far above its P50 is reported but flagged rather
# than quoted.
SPREAD_WARN = 0.15

# How far apart the runs of one mode may put the same case, measured as the
# spread of their medians. This is layout and placement, not the code.
RUN_SPREAD_WARN = 0.08

# The grouped overview is meant to be pasted whole, so it is kept small enough
# to survive a message box: over this, it drops to one line per group.
OVERVIEW_MAX_CHARS = 2000

# Which benchmarks are one thing. Unlisted names land in `other`.
CATEGORIES = (
    ("loops and math", ("int loop", "float loop",
                        "untyped float math", "untyped float compare")),
    ("calls", ("call overhead", "recursion")),
    ("containers", ("array append + index", "dictionary set + get",
                    "container size")),
    ("strings", ("string build", "string iterate")),
    ("guest dispatch", ("logic CPU dispatch", "single-instruction step")),
)


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

    def __init__(self, key, name, runs):
        self.key = key
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


def ratio_value(reference, value):
    """How many times faster `value` is than `reference`. Below 1.0 is slower."""
    if not reference or not value:
        return None
    return reference / value


def fmt_ratio(value):
    return "-" if value is None else "%.2fx" % value


def ratio(reference, value):
    return fmt_ratio(ratio_value(reference, value))


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


def present_modes(modes):
    return [mode for mode in modes if mode]


def modes_by_key(modes):
    return {mode.key: mode for mode in modes}


def first_stat(modes, group, label):
    for mode in present_modes(modes):
        value = mode.stat(group, label)
        if value is not None:
            return value
    return None


def ascii_table(rows, header, align=None):
    """+--+ borders rather than the Markdown pipes above: this one is quoted into
    a terminal, a commit message or a chat window as often as into the docs."""
    widths = [len(h) for h in header]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(str(cell)))
    align = align or ["left"] * len(header)
    rule = "+" + "+".join("-" * (w + 2) for w in widths) + "+"

    def line(cells):
        out = []
        for i, cell in enumerate(cells):
            text = str(cell)
            out.append(text.rjust(widths[i]) if align[i] == "right" else text.ljust(widths[i]))
        return "| " + " | ".join(out) + " |"

    return "\n".join([rule, line(header), rule] + [line(r) for r in rows] + [rule])


def geomean(values):
    """Speedups are ratios, so they average multiplicatively: an arithmetic mean
    of 10x and 0.5x claims a win where there is none."""
    values = [v for v in values if v and v > 0]
    if not values:
        return None
    product = 1.0
    for v in values:
        product *= v
    return product ** (1.0 / len(values))


def categorise(names):
    """Group the benchmarks so the overview stays short. A benchmark not listed
    here still appears, under `other` -- a new group must show up unbidden."""
    seen = set()
    out = []
    for title, members in CATEGORIES:
        rows = [n for n in names if n in members]
        seen.update(rows)
        if rows:
            out.append((title, rows))
    rest = [n for n in names if n not in seen]
    if rest:
        out.append(("other", rest))
    return out


def overview_section(modes, names):
    """The last thing the suite prints: every benchmark in one ASCII table,
    grouped, with a geometric mean per group. Held under OVERVIEW_MAX_CHARS by
    dropping the per-benchmark rows -- the group lines are the summary, and the
    Markdown tables above are where the detail already lives."""
    by_key = modes_by_key(modes)
    full = by_key["full"]
    jit = by_key["jit"]

    def speedups(group):
        reference = meta(modes, group, "reference", REFERENCE_DEFAULT)
        return {
            mode.key: ratio_value(mode.stat(group, reference), mode.stat(group, GUEST))
            for mode in modes
        }

    def full_vs_jit(group):
        return ratio_value(jit.stat(group, GUEST), full.stat(group, GUEST))

    def render(detail):
        # Without the per-benchmark rows there is nothing left to put in the ns
        # columns: a group spans units, and 34 ns/iteration next to 646 ns per
        # emulated instruction is not a number anyone should average.
        header = ["benchmark"]
        if detail:
            header += ["GDScript"] + [mode.name for mode in modes]
        header += [mode.name + " x" for mode in modes] + ["Full vs JIT"]
        rows = []
        for title, members in categorise(names):
            group_speedups = [speedups(g) for g in members]
            rows.append(
                [title]
                + ([""] * (len(modes) + 1) if detail else [])
                + [fmt_ratio(geomean([values[mode.key] for values in group_speedups]))
                   for mode in modes]
                + [fmt_ratio(geomean([full_vs_jit(group) for group in members]))]
            )
            if not detail:
                continue
            for group in members:
                reference = meta(modes, group, "reference", REFERENCE_DEFAULT)
                values = speedups(group)
                rows.append([
                    "  " + group,
                    ns(first_stat(modes, group, reference)),
                ] + [ns(mode.stat(group, GUEST)) for mode in modes]
                  + [fmt_ratio(values[mode.key]) for mode in modes]
                  + [fmt_ratio(full_vs_jit(group))])
        return ascii_table(rows, header, ["left"] + ["right"] * (len(header) - 1))

    def section(detail):
        return ["## Overview", "",
                "ns per work unit; `x` is SafeGDScript against GDScript, above 1.00x",
                "faster. Group lines are the geometric mean of their benchmarks.",
                "", "```", render(detail), "```", ""]

    # The limit is on the whole section, prose included: what gets pasted is the
    # block, not the table alone.
    out = section(True)
    if len("\n".join(out)) > OVERVIEW_MAX_CHARS:
        out = section(False)
    return out


def summary_section(modes, names):
    out = [
        "## Summary",
        "",
        "Median nanoseconds per work unit, pooled over every run of a mode. Each",
        "speedup divides two numbers measured in the same process, which is what makes",
        "the speedup columns comparable: a process's layout shifts its absolute numbers by",
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
            ns(first_stat(modes, group, reference)),
        ] + [ns(mode.stat(group, GUEST)) for mode in modes]
          + [ratio(mode.stat(group, reference), mode.stat(group, GUEST)) for mode in modes])
    out.append(table(
        rows,
        ["benchmark", "unit", "GDScript"]
        + [mode.name for mode in modes]
        + [mode.name + " vs GDScript" for mode in modes],
        ["left", "left"] + ["right"] * (1 + len(modes) * 2)))
    out.append("")
    out.append("The `GDScript` column comes from the first available mode. Every other mode")
    out.append("measures the engine again; their pairwise gaps are under measurement quality.")
    out.append("")
    return out


def detail_sections(modes, names):
    out = ["## Per-benchmark detail", ""]
    active = present_modes(modes)
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
            for mode in active:
                row += [
                    ns(mode.stat(group, label, "min")),
                    ns(mode.stat(group, label)),
                    ns(mode.stat(group, label, "p90")),
                    ratio(mode.stat(group, reference), mode.stat(group, label)),
                ]
            rows.append(row)
        headers = ["case"]
        for mode in active:
            headers += [mode.name + " min", mode.name + " P50",
                        mode.name + " P90", mode.name + " vs ref"]
        out.append(table(rows, headers, ["left"] + ["right"] * (len(headers) - 1)))
        out.append("")
        out.append("ns/%s. `vs ref` compares against `%s` measured in the same runs."
                   % (unit, reference))
        out.append("")
    return out


def quality_section(modes, names):
    """What the tables above are worth: how far apart the runs of one mode put
    the same case, and whether the modes agree about GDScript."""
    active = present_modes(modes)
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
            for mode in active:
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
    if active:
        base = active[0]
        for group in names:
            reference = meta(modes, group, "reference", REFERENCE_DEFAULT)
            a = base.stat(group, reference)
            if not a:
                continue
            for mode in active[1:]:
                b = mode.stat(group, reference)
                if b:
                    deltas.append((abs(a - b) / a, group, base.name, mode.name))
    if deltas:
        deltas.sort(reverse=True)
        out.append("Across modes: GDScript cannot be affected by the sandbox's execution mode,")
        out.append("so every mode is checked against the first present one. Median gap **%s**, worst"
                   % pct(deltas[len(deltas) // 2][0]))
        out.append("**%s** on `%s` (%s against %s). No GDScript speedup above divides across"
                   % (pct(deltas[0][0]), deltas[0][1], deltas[0][2], deltas[0][3]))
        out.append("this boundary; it bounds how precisely absolute figures may be compared.")
        out.append("")

    noisy = []
    for group in names:
        for label in case_order(modes, group):
            for mode in active:
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


def environment_section(modes, manifest):
    out = ["## Environment", ""]
    active = present_modes(modes)
    env_mode = active[0]
    rows = [
        ("CPU", env_mode.env("cpu", "?")),
        ("Godot", env_mode.env("godot", "?")),
        ("Cores", env_mode.env("cpus") or "not pinned"),
        ("Rounds per case per run", env_mode.env("samples", "?")),
        ("Minimum sample", "%d ms" % (env_mode.env("min_sample_usec", 0) / 1000)),
        ("Baked hashes", len(manifest)),
    ]
    expected_modes = {"full": "binary translated", "jit": "JIT", "nojit": "interpreter"}
    for mode in modes:
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
    for mode in modes:
        if not mode:
            continue
        expected = expected_modes[mode.key]
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
    return build_with_manifest(modes, {})


def build_with_manifest(modes, manifest):
    names = ordered_groups(modes)
    out = ["# SafeGDScript benchmarks", ""]
    commands = {"full": "--full", "jit": "--jit", "nojit": "--no-jit"}
    for mode in modes:
        if not mode:
            out.append("> No %s run; run `./run_benchmarks.sh %s`.\n"
                       % (mode.name, commands[mode.key]))
    if not names:
        out.append("No results.")
        return "\n".join(out) + "\n"
    out += summary_section(modes, names)
    out += detail_sections(modes, names)
    out += quality_section(modes, names)
    out += environment_section(modes, manifest)
    out += overview_section(modes, names)
    return "\n".join(out).rstrip() + "\n"


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results-dir", default=os.path.join(here, "results"))
    ap.add_argument("--out", default=None, help="default: <results-dir>/report.md")
    ap.add_argument("--quiet", action="store_true", help="write the file without printing it")
    args = ap.parse_args()

    modes = tuple(Mode(mode, label, load_mode(args.results_dir, mode, fallback))
                  for mode, label, fallback in MODES)
    if not any(modes):
        sys.stderr.write("bench_report: no results in %s -- run ./run_benchmarks.sh first\n"
                         % args.results_dir)
        return 1

    manifest = load(os.path.join(args.results_dir, "bintr", "manifest.json")) or {}
    report = build_with_manifest(modes, manifest)
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
