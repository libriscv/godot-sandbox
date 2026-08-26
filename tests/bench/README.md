# Benchmarks

```
./run_benchmarks.sh                  # both modes, then the tables
./run_benchmarks.sh --no-jit         # the interpreter run only
./run_benchmarks.sh --jit            # the JIT run only
./run_benchmarks.sh --repeat 7       # runs per mode (default 3)
./run_benchmarks.sh --report         # re-print the tables from saved results
./run_benchmarks.sh -gselect=cpu     # one bench file
./run_benchmarks.sh --save-baseline  # keep each run's result to compare against
```

The suite measures three things against each other: GDScript in the engine,
SafeGDScript with the JIT, and SafeGDScript interpreted. The third is not an
aside — the web export has no JIT to enable, so the interpreter is what a mod
running in a browser actually gets, and binary translation embedded ahead of
time is a build step most projects will not take on.

The guest's execution mode is a property of a whole process: libriscv caches a
translated execute segment per binary, so a program once loaded with the JIT off
stays interpreted for the rest of the run. The two modes therefore need separate
runs of Godot, which is why the tables are built afterwards, by
`bench_report.py`, out of both runs' results. It writes `results/report.md`.

## Reading the output

The report has three parts. The summary is one row per benchmark:

```
| benchmark  | unit      | GDScript |  JIT | no JIT | JIT vs GDScript | no JIT vs GDScript |
| int loop   | iteration |     33.0 | 3.52 |   21.2 |           9.39x |              1.51x |
```

- The `ns` columns are the pooled median. Below 1.00x is slower than GDScript.
- Each speedup divides two numbers measured in the same process. That matters:
  a process's layout moves its absolute numbers by a few percent and some
  kernels by much more, so a JIT figure and a no-JIT figure are only comparable
  to within the run-to-run range the report prints.

The per-benchmark tables add min, P50 and P90 per mode, and every case the
group measured rather than only the sandbox — the `.sgd` script path, the guest
loop, `vmcallable()`.

Measurement quality is the part to read before quoting anything. It reports how
far apart the runs of one mode put the same case, whether the two modes agree
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

`--save-baseline` keeps a run's result as `bench/baseline.json` (and
`baseline-nojit.json`), which later runs show as the `vs base` column of the
per-run table. Baselines and results are git-ignored: a baseline from another
CPU, or from a run whose guest executed in a different mode, compares two
different things, and the harness checks both and says so.
