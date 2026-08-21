# Benchmarks

```
./run_benchmarks.sh                  # everything
./run_benchmarks.sh -gselect=cpu     # one file
./run_benchmarks.sh --no-jit         # guest in the interpreter instead of the JIT
./run_benchmarks.sh --save-baseline  # run, then keep the result to compare against
```

Each run writes `bench/results/latest.json`; `--save-baseline` copies that to
`bench/baseline.json`, which later runs show as the `vs base` column. Both are
git-ignored, because a baseline from another CPU compares two different things —
the harness checks the recorded processor and execution mode and says so when
they differ.

`--no-jit` is a separate run, and results go into `results/latest-nojit.json` and
`baseline-nojit.json`, so interpreter numbers are distinct from JIT ones.

## Reading the output

```
--- int loop ---
case                               ns/iteration     vs ref    vs base
SafeGDScript (sandbox)                      5.2      6.26x      +1.3%
GDScript (engine)                          32.4      1.00x      -6.8%
```

- `ns/<unit>` — the best of five samples, after a warmup call. The minimum, not
  the mean: the mean measures the scheduler as much as the code.
- `vs ref` — how many times faster than GDScript. Below 1.00x is slower.
- `vs base` — against the saved baseline, so **negative is faster**. Run-to-run
  noise on an idle machine is a few percent; treat anything under ~5% as noise.
