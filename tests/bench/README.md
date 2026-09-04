# Benchmarks

```
./run_benchmarks.sh                  # JIT and interpreter, then the tables
./run_benchmarks.sh --no-jit         # the interpreter run only
./run_benchmarks.sh --jit            # the JIT run only
./run_benchmarks.sh --repeat 7       # runs per mode (default 3)
./run_benchmarks.sh --report         # re-print the tables from saved results
./run_benchmarks.sh -gselect=cpu     # one bench file
./run_benchmarks.sh --save-baseline  # keep each run's result to compare against
```

The suite measures three things against each other: GDScript in the engine and
SafeGDScript running under asmjit (**JIT**) or the interpreter (**Intrp**). The
interpreter is not an aside — the web export has no JIT to enable, so it is what
a mod gets unless the project embeds a translation ahead of time.

An ahead-of-time C99 translation is faster than either, and this addon loads one
when it finds it, but nothing here produces one: the loader half is covered by
`tests/tests/test_bintr.gd`, which takes a baked fixture from
`SGD_TEST_BINTR_ELF` and `SGD_TEST_BINTR_DIR` and is pending without one. A
suite that measures that column has to bake first, and so lives with whatever
tool does the baking.

The guest's execution mode is a property of a whole process: libriscv caches a
translated execute segment per binary, so a program first loaded by one backend
keeps that backend for the rest of the process. JIT and Intrp therefore need
separate Godot processes.

`bench_report.py` joins all available modes afterwards and writes
`results/report.md`; reports still build when only one mode was requested.

## Reading the output

The report has five parts. The summary is one row per benchmark:

```
| benchmark | unit | GDScript | JIT | Intrp | JIT vs GDScript | Intrp vs GDScript |
| int loop | iteration | 33.0 | 3.52 | 21.2 | 9.39x | 1.51x |
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

## Baselines

`--save-baseline` keeps each run's result as `bench/baseline.json` or
`bench/baseline-nojit.json`, which later runs show as the `vs base` column of
the per-run table. Baselines and results are git-ignored: a baseline from
another CPU, or from a run whose guest executed in a different mode, compares
two different things, and the harness checks both and says so.
