# Benchmark suite

Python benchmarks that build, launch, and drive the exchange/client pair (or,
for the isolated microbenchmarks, a single standalone C++ executable) and
report throughput, latency, or hardware counter rates. Run every script as a
module **from the repo root**:

```bash
python3 -m bench.scripts.<script> [flags]
```

## Layout

```
bench/
  scripts/       python drivers — the "tools": how to run and measure something
  executables/   thin C++ mains — the "objects under test": what gets measured
  harness/       C++ code shared by executables/ (CLI parsing, etc.)
```

This split exists so that adding a benchmark is additive, not multiplicative.
A "tool" (e.g. `perf_on_executable.py`'s generic `perf stat` runner, or the
RDTSC-histogram logic `ring_buffer.py` drives) doesn't know or care which
object it's measuring; an "executable" (e.g. `orderbook_bench.cpp`) doesn't
know or care which tool is driving it, beyond a plain CLI-flags-in /
stdout-or-counters-out contract. With N objects under test and M measurement
tools, that keeps the work at each new pairing near O(1) instead of forcing
either side to be rewritten per combination — e.g. `orderbook_perf.py` is ~15
lines because all the `perf stat` invocation/parsing/repeat-averaging already
exists in `perf_on_executable.py`; the next isolated C++ benchmark gets that
for free too. `harness/` is the equivalent de-duplication on the executables/
side (e.g. `args.hpp`'s `--flag value` parsing, previously copy-pasted per
executable).

## Backend auto-detection (exchange/client scripts only)

`benchlib.get_runner()` picks how the binaries run:

- **native** — used when on Linux x86-64 with the release binaries present. Runs
  `build/exchange/exchange-release` and `build/client/client-release` directly.
  This is the only backend that yields **real latency** numbers.
- **docker** — the fallback (e.g. macOS). Drives both binaries inside one
  `docker compose run --rm dev` shell over loopback. On non-x86 hosts (Apple
  Silicon) the run is x86-64 **emulated**: throughput is still meaningful but
  latency figures are not authoritative, and every latency-bearing script prints a
  banner saying so.

Force a backend with `--runner {auto,native,docker}`. The isolated microbenchmarks
(`ring_buffer`, `orderbook_perf`) don't use a runner at all — they build and
run a single native Linux x86-64 executable directly.

## What each script measures & how it drives the processes

| Script | Measures | Process manipulation |
|---|---|---|
| `saturation` | Peak sustained req/s and where throughput plateaus | Closed-loop (`EXPECTED_THROUGHPUT=0`); sweeps `num_clients` (runtime arg). No rebuild unless the cap wasn't already 0. |
| `latency_curve` | End-to-end latency percentiles vs offered load | Sets `EXPECTED_THROUGHPUT` per level and rebuilds `client-release`; keeps only in-band samples; averages the exchange's end-to-end histogram. |
| `stage_breakdown` | All 8 pipeline-stage histograms + each stage's share of end-to-end | Sets `DIAGNOSTICS=1` in **both** configs, rebuilds **both** targets, runs, then restores and rebuilds back. |
| `regression` | Throughput vs a stored baseline + invariants (`err==0`, `match>0`) | Fixed seed; closed-loop by default. `--update` writes the baseline; a check exits non-zero on regression. |
| `ring_buffer` | `RingBuffer<T>` push/pop latency + throughput, in isolation | Doesn't touch the exchange/client at all — builds and runs `bench/executables/ring-buffer-bench`, a standalone RDTSC-based C++ harness that links `ring_buffer.hpp` directly. Native Linux x86-64 only. |
| `orderbook_perf` | Cache-miss / branch-miss rate for `Orderbook::process()`, in isolation | Doesn't touch Engine/Server — builds and runs `bench/executables/orderbook-bench` under `perf stat` (via `perf_on_executable`), driving a seeded NEW/CANCEL/MODIFY workload directly against a real `Orderbook`. Native Linux x86-64 only (needs the hardware PMU). |
| `perf_on_executable` | Generic `perf stat` wrapper: any executable, any event groups | Not target-specific — pass `-i <path> -e ev1,ev2 -- <args>`; used directly for ad hoc profiling, or as a library by scripts like `orderbook_perf.py`. |

`ring_buffer` and `orderbook_perf` are a different kind of benchmark from the
first four: they measure a data structure or component directly rather than
driving the TCP exchange/client pair.

`ring_buffer` exists because push/pop are nanosecond-scale operations that
only a native harness with RDTSC and core pinning can time accurately (see
`bench/executables/ring_buffer_bench.cpp` for the measurement methodology —
RDTSC fencing, deferred histogram recording, cross-core TSC offset
calibration). Three modes: `op` (same-thread push-then-pop — the cleanest
number), `spsc` (cross-core producer/consumer, mirrors how the exchange
actually uses the ring), `sat` (saturation throughput). Run via:

```bash
python3 -m bench.scripts.ring_buffer op
python3 -m bench.scripts.ring_buffer spsc --capacity 524288 --iters 2000000
python3 -m bench.scripts.ring_buffer sat --iters 5000000
```

Without `CAP_SYS_NICE` the benchmark threads can't get `SCHED_FIFO` priority,
so on a busy host `spsc` numbers can be dominated by scheduler-preemption
backlog rather than real ring-transit latency — the script detects this
(`stall_suspect_count`) and prints `min_ns` (best-case single-sample latency)
alongside the percentiles, with a warning when the tail looks contaminated.

`orderbook_perf` exists because cache/branch-miss rates need hardware PMU
counters, not RDTSC — `perf stat` provides those directly, including
mean/stddev across repeated runs (`-r`) so the reported rate isn't a
cherry-picked best trial. Run via:

```bash
python3 -m bench.scripts.orderbook_perf
python3 -m bench.scripts.orderbook_perf --ops 10000000 --reps 20 --core 6
```

Or profile anything else the same way:

```bash
python3 -m bench.scripts.perf_on_executable \
    -i build/bench/executables/orderbook-bench -r 20 \
    -e cache-references,cache-misses -e branches,branch-misses \
    -- --ops 10000000 --seed 7 --core 6
```

All config edits are transient: `benchlib.config_guard` snapshots the full text of
every touched config header and restores it on normal exit, exception, **or**
SIGINT/SIGTERM — the source tree is always left byte-for-byte unchanged.

## Common flags

- `--runner {auto,native,docker}` — backend (default: auto); exchange/client scripts only
- `--duration SECONDS` — run window per sample (default: 15); exchange/client scripts only
- `--seed N` — RNG seed for reproducibility (default varies by script)
- `--no-build` — skip configure/build; assume the binaries are current
- `BENCH_BUILD_DIR=<path>` (env) — override the build directory (default: `build/`)

## Output

- Console tables (throughput / percentiles / event rates).
- `bench/results/<script>_<timestamp>.{json,csv}` — structured + flat results.
- `bench/logs/<script>_*.log` — raw client/exchange stdout+stderr per run.
- `bench/baselines/<name>.<runner>.json` — regression baselines (committed;
  keyed by backend since absolute numbers differ between native and docker).

`results/` and `logs/` are gitignored; `baselines/` is committed.

## Caveats

- **Latency warm-up**: the exchange discards the first `LATENCY_SAMPLE_DROP`
  (100k) samples before recording. A run that never exceeds that (short and/or
  emulated) reports 0-ns percentiles; the scripts warn when the exchange collected
  no samples at all. Use native Linux with enough load/duration for real numbers.
- **Baselines are environment-specific**: generate `regression` baselines on the
  machine you intend to gate on (a docker/emulated baseline is not comparable to a
  native one).
- **PMU counter multiplexing**: requesting many hardware events in one `perf
  stat` call spreads them across too few physical counters, and perf scales up
  the sampled fraction rather than measuring continuously — a noisier estimate.
  `perf_on_executable` keeps event groups small (2-3) and runs each group as a
  separate invocation to avoid this.
