# Benchmark suite

Python benchmarks that build, launch, and drive the exchange/client pair and
report throughput and latency. Run every script as a module **from the repo
root**:

```bash
python3 -m bench.<script> [flags]
```

## Backend auto-detection

`benchlib.get_runner()` picks how the binaries run:

- **native** — used when on Linux x86-64 with the release binaries present. Runs
  `build/exchange/exchange-release` and `build/client/client-release` directly.
  This is the only backend that yields **real latency** numbers.
- **docker** — the fallback (e.g. macOS). Drives both binaries inside one
  `docker compose run --rm dev` shell over loopback. On non-x86 hosts (Apple
  Silicon) the run is x86-64 **emulated**: throughput is still meaningful but
  latency figures are not authoritative, and every latency-bearing script prints a
  banner saying so.

Force a backend with `--runner {auto,native,docker}`.

## What each script measures & how it drives the processes

| Script | Measures | Process manipulation |
|---|---|---|
| `saturation` | Peak sustained req/s and where throughput plateaus | Closed-loop (`EXPECTED_THROUGHPUT=0`); sweeps `num_clients` (runtime arg). No rebuild unless the cap wasn't already 0. |
| `latency_curve` | End-to-end latency percentiles vs offered load | Sets `EXPECTED_THROUGHPUT` per level and rebuilds `client-release`; keeps only in-band samples; averages the exchange's end-to-end histogram. |
| `stage_breakdown` | All 8 pipeline-stage histograms + each stage's share of end-to-end | Sets `DIAGNOSTICS=1` in **both** configs, rebuilds **both** targets, runs, then restores and rebuilds back. |
| `regression` | Throughput vs a stored baseline + invariants (`err==0`, `match>0`) | Fixed seed; closed-loop by default. `--update` writes the baseline; a check exits non-zero on regression. |
| `ring_buffer` | `RingBuffer<T>` push/pop latency + throughput, in isolation | Doesn't touch the exchange/client at all — builds and runs `bench/cpp/ring-buffer-bench`, a standalone RDTSC-based C++ harness that links `ring_buffer.hpp` directly. Native Linux x86-64 only. |

`ring_buffer` is a different kind of benchmark from the other four: it measures
the data structure directly rather than driving the TCP exchange/client pair,
because push/pop are nanosecond-scale operations that only a native harness
with RDTSC and core pinning can time accurately (see
`bench/cpp/ring_buffer_bench.cpp` for the measurement methodology — RDTSC
fencing, deferred histogram recording, cross-core TSC offset calibration).
Three modes: `op` (same-thread push-then-pop — the cleanest number), `spsc`
(cross-core producer/consumer, mirrors how the exchange actually uses the
ring), `sat` (saturation throughput). Run via:

```bash
python3 -m bench.ring_buffer op
python3 -m bench.ring_buffer spsc --capacity 524288 --iters 2000000
python3 -m bench.ring_buffer sat --iters 5000000
```

Without `CAP_SYS_NICE` the benchmark threads can't get `SCHED_FIFO` priority,
so on a busy host `spsc` numbers can be dominated by scheduler-preemption
backlog rather than real ring-transit latency — the script detects this
(`stall_suspect_count`) and prints `min_ns` (best-case single-sample latency)
alongside the percentiles, with a warning when the tail looks contaminated.

All config edits are transient: `benchlib.config_guard` snapshots the full text of
every touched config header and restores it on normal exit, exception, **or**
SIGINT/SIGTERM — the source tree is always left byte-for-byte unchanged.

## Common flags

- `--runner {auto,native,docker}` — backend (default: auto)
- `--duration SECONDS` — run window per sample (default: 15)
- `--seed N` — client RNG seed for reproducibility (default: 42)
- `--no-build` — skip configure/build; assume the binaries are current
- `BENCH_BUILD_DIR=<path>` (env) — override the build directory (default: `build/`)

## Output

- Console tables (throughput / percentiles).
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
