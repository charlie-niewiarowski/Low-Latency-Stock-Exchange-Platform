#!/usr/bin/env python3
"""
orderbook_perf.py — cache-miss / branch-miss rate for Orderbook::process(),
measured in isolation (no Engine/Server) via `perf stat`.

This is the "N" side for the order book: it builds bench/executables/
orderbook-bench and hands it to perf_on_executable.run_groups() — all the
`perf stat` invocation/parsing/repeat-averaging logic lives there, shared with
every other perf-measured benchmark. Mirrors what ring_buffer.py is to
ring-buffer-bench, just measuring hardware counters instead of RDTSC latency.

    python3 -m bench.scripts.orderbook_perf
    python3 -m bench.scripts.orderbook_perf --ops 10000000 --reps 20 --core 6
"""

from __future__ import annotations

import argparse
import os
import sys

from bench.scripts import benchlib as bl
from bench.scripts import perf_on_executable as pe

BENCH_BIN = bl.BUILD_DIR / "bench" / "executables" / "orderbook-bench"

EVENT_GROUPS = ["cache-references,cache-misses", "branches,branch-misses"]


class _LocalRunner:
    """Minimal stand-in so we can reuse benchlib's result_meta()/write_json()."""
    name = "perf-stat"
    emulated = False


def build(no_build: bool) -> None:
    if no_build:
        return
    bl._run(["cmake", "-S", str(bl.REPO_ROOT), "-B", str(bl.BUILD_DIR)])
    bl._run(["cmake", "--build", str(bl.BUILD_DIR), "--target", "orderbook-bench",
             f"-j{os.cpu_count() or 4}"])


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ops", type=int, default=10_000_000,
                   help="operations per perf run (default: 10000000; stay "
                        "well under the 1M order-pool cap's steady-state "
                        "working set — see orderbook_bench.cpp)")
    p.add_argument("--seed", type=int, default=7, help="workload RNG seed (default: 7)")
    p.add_argument("--core", type=int, default=6, help="core to pin to (default: 6)")
    p.add_argument("--depth-max", type=int, default=50,
                   help="max resting-order depth in ticks from touch (default: 50)")
    p.add_argument("--reps", type=int, default=20,
                   help="perf stat -r: repeats per event group, reported as "
                        "mean +- relative stddev, never a single best trial "
                        "(default: 20)")
    p.add_argument("--no-build", action="store_true",
                   help="skip configure/build; assume the binary is current")
    args = p.parse_args()

    pe.require_perf()
    if not BENCH_BIN.exists() and args.no_build:
        sys.exit(f"[ERROR] {BENCH_BIN} missing and --no-build given.")

    print("  building orderbook-bench ...", flush=True)
    build(args.no_build)

    exe_args = ["--ops", str(args.ops), "--seed", str(args.seed),
                "--core", str(args.core), "--depth-max", str(args.depth_max)]
    print(f"  orderbook-bench --ops {args.ops:,} --seed {args.seed} "
          f"--core {args.core} --depth-max {args.depth_max}  (reps={args.reps})\n")

    results = pe.run_groups(BENCH_BIN, exe_args, EVENT_GROUPS, reps=args.reps)
    for group in EVENT_GROUPS:
        for name in group.split(","):
            ev = results.get(name)
            print(f"  {pe.format_event(ev) if ev else name + ': <no data>'}")

    meta = bl.result_meta(_LocalRunner(), dict(
        ops=args.ops, seed=args.seed, core=args.core, depth_max=args.depth_max, reps=args.reps))
    payload = {**meta, "events": {k: vars(v) for k, v in results.items()}}
    jp = bl.write_json("orderbook_perf", payload)
    print(f"\n  wrote {jp.relative_to(bl.REPO_ROOT)}")


if __name__ == "__main__":
    main()
