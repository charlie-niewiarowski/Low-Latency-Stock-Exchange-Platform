#!/usr/bin/env python3
"""
ring_buffer.py — RingBuffer<T> push/pop latency + throughput microbenchmark.

Unlike the rest of bench/, this does not drive the TCP exchange/client pair.
It builds and runs bench/cpp/ring-buffer-bench, a small standalone C++ harness
(bench/cpp/ring_buffer_bench.cpp) that links exchange/include/ring_buffer.hpp
directly and times push()/pop() with RDTSC — the same primitive
exchange/server/include/latency.hpp uses on the real hot path.

Three modes:

  op    same-thread push-then-pop. The intrinsic per-call cost of push() and
        pop(), with no cross-core effects — the cleanest number available.
  spsc  producer and consumer pinned to separate physical cores, mirroring
        how the exchange actually uses the ring (inbound thread -> engine,
        engine -> outbound thread). Reports push -> pop transit latency,
        corrected for measured inter-core TSC clock skew (a ping-pong
        handshake run before the timed loop — see calib_producer_side /
        calib_consumer_side in the C++ source) so that skew doesn't leak
        into the numbers as spurious latency (or, worse, small negative
        values).
  sat   both threads spinning flat out (no pacing). Reports sustained ops/s
        and the fraction of push() calls rejected because the ring was full.

Measurement hygiene, and why you should trust (or distrust) a given run:
  - RDTSC reads are bracketed with LFENCE/RDTSCP (see tsc_begin/tsc_end in
    the C++), so the timed window can't leak into neighboring iterations.
  - All bookkeeping (ns conversion, histogram recording) happens after the
    timed loop against a pre-sized buffer — nothing but the RDTSC reads and
    the ring op itself sits inside the timed bracket.
  - Real-time (SCHED_FIFO) scheduling requires CAP_SYS_NICE; without it the
    OS can preempt the pinned threads for scheduler-latency-scale stretches,
    which shows up as spurious high-latency samples that have nothing to do
    with the ring buffer. This script checks the load average before each
    run and warns loudly when the machine is busy enough that this is a
    real risk — treat elevated p99.9+/max on a loaded box with suspicion.

Native Linux x86-64 only (raw RDTSC + core affinity); does not use
benchlib's Docker runner.

    python3 -m bench.ring_buffer op
    python3 -m bench.ring_buffer spsc --capacity 524288 --iters 2000000
    python3 -m bench.ring_buffer sat --iters 5000000
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import subprocess
import sys

from bench import benchlib as bl

BENCH_BIN = bl.BUILD_DIR / "bench" / "cpp" / "ring-buffer-bench"

X86 = {"x86_64", "amd64", "AMD64"}

_CALIB_FIELDS = {
    "ticks_per_ns":              r"ticks_per_ns:\s*([\d.]+)",
    "tsc_offset_ns":              r"tsc_offset_ns:\s*(-?[\d.]+)",
    "negative_after_correction":  r"negative_after_correction:\s*(\d+)",
    "min_ns":                     r"min_ns:\s*([\d.]+)",
    "stall_suspect_count":        r"stall_suspect_count:\s*(\d+)",
}
_SAT_FIELDS = {
    "ops_s":            r"ops_s:\s*([\d.]+)",
    "push_fail_ratio":  r"push_fail_ratio:\s*([\d.]+)",
}


class _LocalRunner:
    """Minimal stand-in so we can reuse benchlib's result_meta()/write_*()."""
    name = "native-cpp"
    emulated = False


def _require_native() -> None:
    if platform.system() != "Linux" or platform.machine() not in X86:
        sys.exit("[ERROR] ring_buffer bench requires Linux x86-64 "
                 "(raw RDTSC + sched_setaffinity core pinning).")


def _load_warning() -> None:
    try:
        load1, _, _ = os.getloadavg()
    except OSError:
        return
    ncpu = os.cpu_count() or 1
    if load1 > 0.5 * ncpu:
        print(f"  [WARN] load average {load1:.1f} on {ncpu} CPUs — the benchmark "
              f"threads run at normal (non-realtime) priority, so contention from "
              f"other work on this box can preempt them for scheduler-latency-scale "
              f"stretches and show up as spurious high-latency samples. Treat "
              f"elevated p99.9+/max with suspicion; p50 is more robust.")


def build(no_build: bool) -> None:
    if no_build:
        return
    bl._run(["cmake", "-S", str(bl.REPO_ROOT), "-B", str(bl.BUILD_DIR)])
    bl._run(["cmake", "--build", str(bl.BUILD_DIR), "--target", "ring-buffer-bench",
             f"-j{os.cpu_count() or 4}"])


def run_once(mode: str, args) -> tuple[str, str]:
    cmd = [str(BENCH_BIN), mode,
           "--capacity", str(args.capacity),
           "--iters", str(args.iters),
           "--warmup", str(args.warmup),
           "--producer-cpu", str(args.producer_cpu),
           "--consumer-cpu", str(args.consumer_cpu)]
    if mode == "spsc":
        cmd += ["--rate", str(args.rate)]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
    out, err = r.stdout.decode(errors="replace"), r.stderr.decode(errors="replace")
    if r.returncode != 0:
        sys.exit(f"[ERROR] {' '.join(cmd)} exited {r.returncode}:\n{err}")
    if err.strip():
        print(err.strip())
    return out, err


def parse_fields(out: str, fields: dict[str, str]) -> dict[str, float]:
    result = {}
    for key, pat in fields.items():
        m = re.search(pat, out)
        if m:
            result[key] = float(m.group(1))
    return result


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["op", "spsc", "sat"])
    p.add_argument("--capacity", type=int, default=524_288,
                   help="ring capacity, power of two (default: 524288, matches "
                        "exchange COMMUNICATION_RING_COUNT)")
    p.add_argument("--iters", type=int, default=None,
                   help="recorded samples (default: mode-dependent, see the C++ harness)")
    p.add_argument("--warmup", type=int, default=50_000,
                   help="unrecorded warm-up ops before timing starts (default: 50000)")
    p.add_argument("--producer-cpu", type=int, default=4,
                   help="core to pin the (sole, for op) producer thread to (default: 4)")
    p.add_argument("--consumer-cpu", type=int, default=5,
                   help="core to pin the consumer thread to; spsc/sat only (default: 5)")
    p.add_argument("--rate", type=int, default=0,
                   help="spsc only: paced pushes/s (0 = unpaced/saturated, default)")
    p.add_argument("--samples", type=int, default=3,
                   help="repeat runs to average over (default: 3)")
    p.add_argument("--no-build", action="store_true",
                   help="skip configure/build; assume the binary is current")
    args = p.parse_args()

    _require_native()
    if not BENCH_BIN.exists() and args.no_build:
        sys.exit(f"[ERROR] {BENCH_BIN} missing and --no-build given.")

    print("  building ring-buffer-bench ...", flush=True)
    build(args.no_build)
    _load_warning()

    if args.mode == "sat":
        _run_sat(args)
    else:
        _run_latency(args)


def _cli_iters(args) -> int:
    if args.iters is not None:
        return args.iters
    return {"op": 2_000_000, "spsc": 1_000_000, "sat": 5_000_000}[args.mode]


def _run_latency(args) -> None:
    args.iters = _cli_iters(args)
    hists, calibs = [], []
    for s in range(1, args.samples + 1):
        out, _ = run_once(args.mode, args)
        h = bl.parse_histograms(out)
        c = parse_fields(out, _CALIB_FIELDS)
        if not h:
            print(f"  run {s}: [no histogram parsed — see output above]")
            continue
        hists.append(h)
        calibs.append(c)
        print(f"  run {s}: ok ({', '.join(f'{k}={v:g}' for k, v in c.items())})")

    if not hists:
        sys.exit("[ERROR] no successful runs.")

    avg = bl.average_samples(hists)
    title = (f"ring_buffer {args.mode}  (capacity={args.capacity:,}, "
             f"iters={args.iters:,}"
             + (f", rate={'unpaced' if args.rate == 0 else f'{args.rate:,}/s'}"
                if args.mode == "spsc" else "") + ")")
    bl.print_histograms(title, avg)

    if args.mode == "spsc":
        avg_calib = {k: sum(c[k] for c in calibs) / len(calibs)
                    for k in _CALIB_FIELDS if all(k in c for c in calibs)}
        if avg_calib:
            print(f"  avg ticks_per_ns={avg_calib.get('ticks_per_ns', 0):.4f}  "
                  f"avg tsc_offset_ns={avg_calib.get('tsc_offset_ns', 0):.2f}  "
                  f"avg negative_after_correction={avg_calib.get('negative_after_correction', 0):.1f}")
            min_ns = avg_calib.get("min_ns", 0)
            stall_pct = 100 * avg_calib.get("stall_suspect_count", 0) / args.iters
            print(f"  min_ns={min_ns:.1f}  (best-case single-sample transit latency — "
                  f"a more robust indicator than the percentiles above on a busy host)")
            if stall_pct > 0.5:
                trust = ("even p50 is contaminated — trust min_ns alone" if stall_pct >= 50
                         else "p50 is still reasonably robust; treat p90+ with suspicion")
                print(f"  [WARN] {stall_pct:.2f}% of samples exceeded 5us — consistent with "
                      f"scheduler preemption of the pinned threads (SCHED_FIFO needs "
                      f"CAP_SYS_NICE, which this run didn't have), not real ring-transit "
                      f"latency. The percentile table above is likely dominated by backlog "
                      f"drain from those stalls; {trust}.")

    meta = bl.result_meta(_LocalRunner(), dict(
        mode=args.mode, capacity=args.capacity, iters=args.iters, warmup=args.warmup,
        producer_cpu=args.producer_cpu, consumer_cpu=args.consumer_cpu,
        rate=args.rate, samples=args.samples))
    rows = [dict(section=sec, **{p: round(d.get(p, 0)) for p in bl.PCTS})
            for sec, d in avg.items()]
    jp = bl.write_json("ring_buffer", {**meta, "sections": avg, "calibration": calibs})
    cp = bl.write_csv("ring_buffer", rows, ["section", *bl.PCTS])
    print(f"  wrote {jp.relative_to(bl.REPO_ROOT)}")
    print(f"  wrote {cp.relative_to(bl.REPO_ROOT)}")


def _run_sat(args) -> None:
    args.iters = _cli_iters(args)
    runs = []
    for s in range(1, args.samples + 1):
        out, _ = run_once(args.mode, args)
        fields = parse_fields(out, _SAT_FIELDS)
        if "ops_s" not in fields:
            print(f"  run {s}: [no throughput parsed — see output above]")
            continue
        runs.append(fields)
        print(f"  run {s}:  {fields['ops_s']:>14,.0f} ops/s   "
              f"push_fail_ratio={fields.get('push_fail_ratio', 0):.4%}")

    if not runs:
        sys.exit("[ERROR] no successful runs.")

    avg_ops = bl.median([r["ops_s"] for r in runs])
    avg_fail = bl.median([r.get("push_fail_ratio", 0) for r in runs])
    print(f"\n  ring_buffer sat  (capacity={args.capacity:,}, iters={args.iters:,})")
    print(f"    median ops/s        : {avg_ops:,.0f}")
    print(f"    median push_fail_ratio: {avg_fail:.4%}")

    meta = bl.result_meta(_LocalRunner(), dict(
        mode=args.mode, capacity=args.capacity, iters=args.iters,
        producer_cpu=args.producer_cpu, consumer_cpu=args.consumer_cpu,
        samples=args.samples))
    jp = bl.write_json("ring_buffer", {**meta, "runs": runs,
                                       "median_ops_s": avg_ops,
                                       "median_push_fail_ratio": avg_fail})
    cp = bl.write_csv("ring_buffer", runs, ["ops_s", "push_fail_ratio"])
    print(f"  wrote {jp.relative_to(bl.REPO_ROOT)}")
    print(f"  wrote {cp.relative_to(bl.REPO_ROOT)}")


if __name__ == "__main__":
    main()
