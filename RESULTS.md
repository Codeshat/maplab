# Results — machine, method, and how to reproduce them

Every number in [EXPERIMENTS.md](EXPERIMENTS.md) and the README came from the machine and
procedure described here. Read this first: the conclusions are portable, several of the
absolute nanosecond figures are not.

---

## Reference machine

| | |
|---|---|
| CPU | Intel Core i5-10210U (Comet Lake-U), 4 cores / 8 threads, 1.60 GHz base, 4.20 GHz boost |
| L1d | 32 KiB per core |
| L2 | 256 KiB per core |
| L3 | 6 MiB, shared |
| RAM | 8 GiB |
| OS | Ubuntu 24.04.4 LTS, Linux 7.0.0-28-generic |
| Compiler | GCC 13.3.0, `-O3 -DNDEBUG -std=c++23` (`--preset release`) |
| Also builds under | Clang 19.1.1 |
| cpufreq governor | **powersave** |

**This is a thermally constrained ultrabook, not a benchmarking machine.** It is the wrong
machine for absolute numbers and a perfectly good one for ablations, because an ablation
compares two configurations under the same conditions. Where the two disagree, this repo
sides with the ablation.

---

## Method

**Timing.** Each measurement is a warm-up pass followed by 7 timed passes over a fixed
operation count; the reported figure is the **minimum**. Timing noise here is one-sided —
an interrupt, a frequency drop, a background process — so the fastest observed pass is the
best available estimate of the code's own cost, while the median drifts with whatever else
the machine was doing.

Every experiment prints the worst `(max - min) / min` spread it saw, so the noise is
visible rather than implied. On a quiet run this machine reports 10–40%; that is a property
of the hardware, not of the estimator.

**Counting.** Timing and counting are **separate runs**. A `probe_stats` update is several
stores plus a histogram increment per lookup, against a 400-byte counter block competing
for the same L1 the table wants — timing an instrumented build measures the instrument. It
inflated this project's first Experiment 1 run by roughly 8x at 64K elements. Each
experiment therefore times a stats-free table and collects counters from its stats-enabled
twin: same configuration in every other respect, so both place the same keys in the same
slots. Experiment 1 asserts the two agree on groups-probed before reporting anything.

**Keys.** Generated from fixed seeds by `support/include/maplab_workloads/`, so any figure
can be reproduced exactly. Lookups walk a **shuffled copy** of the key array sequentially,
rather than indexing through a random permutation. The permutation array costs a second
random memory access per lookup, and at 8M+ elements it and the key array are both larger
than L3 — two thirds of the measured cache misses belonged to the harness. Shuffling the
keys keeps the *table* access uniformly random while making the *key* access prefetchable.

**Miss benchmarks use keys guaranteed absent by construction.** A "miss" benchmark that
accidentally hits even 1% of the time is measuring something else.

**Pinning.** `scripts/run_experiments.sh` pins to one core with `taskset`. Without it the
scheduler migrates the process between cores with cold caches and different thermal
headroom, and the load-factor sweep came out visibly non-monotonic purely from that.

**Sanitizers are never linked into anything that reports a time.** The benchmark and
experiment targets deliberately do not link `maplab::sanitizers`, and CMake emits a warning
if you configure them with `MAPLAB_SANITIZE` set.

---

## Reproducing

```bash
# build
cmake --preset release
cmake --build --preset release

# correctness first: nothing below is meaningful if this is red
ctest --preset release
./build/release/bin/maplab_tests "[.soak]"      # 2M-operation differential soak

# the experiments (writes results/*.csv and results/*.txt)
./scripts/run_experiments.sh

# the Google Benchmark suite (writes results/bench.json)
./scripts/run_bench.sh

# the figures
python3 bench/plots/plot.py --results results --out docs/img
```

For a quieter machine, before measuring:

```bash
sudo cpupower frequency-set -g performance
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space   # optional
```

Environment knobs: `MAPLAB_RESULTS_DIR` (output directory), `CPU` (which core to pin to),
`REPS` and `MIN_TIME` for `run_bench.sh`, `FILTER` to run a subset.

`plot.py` draws the cache boundaries at the sizes above; on another machine pass
`--l1/--l2/--l3` in bytes.

---

## Known limitations of these measurements

1. **Absolute latencies are pessimistic.** A powersave-governed 1.6 GHz ultrabook under
   thermal pressure produces numbers well off what a desktop or server part would give.
   Ratios between configurations are far more trustworthy than any single figure.
2. **The size axis stops at 8M elements** (~140 MB for maplab, ~4x that for
   `std::unordered_map`), because the machine has 8 GB. That is still ~23x the 6 MB L3, so
   the DRAM plateau is fully visible; there is simply no data beyond it.
3. **No hardware performance counters.** `perf_event_paranoid` is 4 on this machine, so
   cache-miss and instruction counts per operation are absent. maplab's own software
   counters — groups probed, key comparisons, fingerprint false positives — cover the same
   analytical ground and are what the experiments actually rely on. Adding `perf stat`
   output on a machine that permits it is on the roadmap.
4. **Single run per configuration.** No confidence intervals; the reported spread is the
   only dispersion statistic.
5. **Baselines are compiled as their authors ship them** and not tuned. `absl` is off by
   default (`-DMAPLAB_WITH_ABSEIL=ON` to include it); `ankerl::unordered_dense` v4.5.0 is
   vendored so the baseline cannot silently change version between runs.
