# Benchmarks

```
./run_benchmarks.sh                  # Full, JIT and interpreter, then the tables
./run_benchmarks.sh --full           # baked C99 translation only
./run_benchmarks.sh --no-full        # JIT and interpreter only
./run_benchmarks.sh --no-jit         # the interpreter run only
./run_benchmarks.sh --jit            # the JIT run only
./run_benchmarks.sh --repeat 7       # runs per mode (default 3)
./run_benchmarks.sh --report         # re-print the tables from saved results
./run_benchmarks.sh -gselect=cpu     # one bench file
./run_benchmarks.sh --save-baseline  # keep each run's result to compare against
```

The suite measures four things against each other: GDScript in the engine and
SafeGDScript using a fully baked C99 translation (**Full**), asmjit (**JIT**),
or the interpreter (**Intrp**). The interpreter is not an aside — the web
export has no JIT to enable, so it is what a mod gets unless the project embeds
a translation ahead of time.

The guest's execution mode is a property of a whole process: libriscv caches a
translated execute segment per binary, so a program first loaded by one backend
keeps that backend for the rest of the process. Full, JIT and Intrp therefore
need separate Godot processes. Full also has a fourth process before them: it
loads and bakes every benchmark ELF into `results/bintr/`, then exits before the
measuring process starts. Baking inside the measuring process would first claim
the segment with asmjit and report JIT numbers under the Full heading.

The bake cache is keyed by the ELF execute bytes, all translation-affecting
Sandbox options, and the addon build. A stale object after rebuilding the addon
silently misses at the loader level; the benchmark harness turns that miss into
a failed assertion instead of accepting mislabeled numbers. The runner resets
the cache before every Full run and records the expected hashes in
`results/bintr/manifest.json`. Full is skipped, with a reason, when the addon has
no C99 translator or its configured C compiler is unavailable.

`bench_report.py` joins all available modes afterwards and writes
`results/report.md`; reports still build when Full was skipped or only one mode
was requested.

## Reading the output

The report has five parts. The summary is one row per benchmark:

```
| benchmark | unit | GDScript | Full | JIT | Intrp | Full vs GDScript | JIT vs GDScript | Intrp vs GDScript |
| int loop | iteration | 33.0 | 3.10 | 3.52 | 21.2 | 10.65x | 9.39x | 1.51x |
```

- The `ns` columns are the pooled median. Below 1.00x is slower than GDScript.
- Each speedup divides two numbers measured in the same process. That matters:
  a process's layout moves its absolute numbers by a few percent and some
  kernels by much more, so figures from different modes are only comparable
  to within the run-to-run range the report prints.

The per-benchmark tables add min, P50 and P90 per mode, and every case the
group measured rather than only the sandbox — the `.sgd` script path, the guest
loop, `vmcallable()`.

The overview at the end is the whole suite as one ASCII table, benchmarks
grouped, with a geometric mean per group -- small enough to paste whole. Groups
are `CATEGORIES` in `bench_report.py`; a benchmark not listed there shows up
under `other`. Past 2000 characters it drops to one line per group, so adding
benchmarks shortens the table rather than overflowing it.

Measurement quality is the part to read before quoting anything. It reports how
far apart the runs of one mode put the same case, whether all modes agree
about GDScript (they must: the engine cannot see the sandbox's execution mode),
and any row whose P90 sits well above its P50.

## What the numbers are

- A sample is repeated until it lasts at least 30 ms. Below a few tens of
  milliseconds the reading is the timer and the scheduler rather than the code.
- Samples are taken in interleaved rounds — every case once, then every case
  again — so drift over a group lands on all of its cases equally.
- Samples are pooled across the runs of a mode. Sampling inside one process is
  stable to about a percent; where the loader and the allocator put things is
  worth ten times that on some kernels, and no number of samples inside a single
  process can see it.
- Runs are pinned to the cores sharing one L3 with cpu0, which on a multi-die
  part is one die. `--no-pin` turns that off, `--cpu <list>` chooses.
- Binary translation is synchronous, not backgrounded: compiling on another
  thread live-patches the decoder cache under a running guest, which mixes
  interpreted and translated code into the early samples.

## Baselines

`--save-baseline` keeps each run's result as `bench/baseline-full.json`,
`bench/baseline.json`, or `bench/baseline-nojit.json`, which later runs show as
the `vs base` column of the per-run table. Baselines and results are git-ignored:
a baseline from another CPU, or from a run whose guest executed in a different
mode, compares two different things, and the harness checks both and says so.
