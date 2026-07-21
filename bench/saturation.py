#!/usr/bin/env python3
"""
saturation.py — throughput saturation vs client count.

Closed-loop (EXPECTED_THROUGHPUT = 0): each client fires as fast as its pipeline
allows. We sweep the number of concurrent clients and measure sustained
requests/s, revealing the peak and the point where adding connections stops
helping (or starts hurting).

Process manipulation: runtime argument only (client count). The client is rebuilt
once only if EXPECTED_THROUGHPUT is not already 0 (then restored). No exchange
rebuild.

    python3 -m bench.saturation --clients 1,2,4,8,16,32 --duration 10 --samples 3
"""

from __future__ import annotations

import argparse
from contextlib import nullcontext

from bench import benchlib as bl


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    bl.add_common_args(p)
    p.add_argument("--clients", type=bl.int_list, default=[1, 2, 4, 8, 16, 32],
                   help="comma-separated client counts (default: 1,2,4,8,16,32)")
    p.add_argument("--samples", type=int, default=3, help="runs per client count")
    args = p.parse_args()

    runner = bl.get_runner(args.runner)
    bl.emulation_banner(runner)

    cur = bl.read_macro(bl.CLIENT_CFG, "EXPECTED_THROUGHPUT")
    need_zero = cur != "0"
    guard = bl.config_guard([bl.CLIENT_CFG]) if need_zero else nullcontext()

    rows: list[dict] = []
    summary: list[dict] = []

    with guard:
        if need_zero:
            print(f"  EXPECTED_THROUGHPUT was {cur}; setting 0 for closed-loop.")
            bl.set_macro(bl.CLIENT_CFG, "EXPECTED_THROUGHPUT", 0)
        bl.ensure_built(runner, ["exchange-release", "client-release"], args.no_build)

        for n in args.clients:
            reqs, tots, matches = [], [], []
            print(f"\n  clients={n:>3}  ", end="", flush=True)
            for s in range(1, args.samples + 1):
                res = runner.run_pair(n, args.seed, args.duration)
                res.save_log(bl.LOGS_DIR / f"saturation_c{n}_s{s}.log")
                st = res.stats
                if not st:
                    print("[skip:no-stats] ", end="", flush=True)
                    continue
                reqs.append(st["requests_s"])
                tots.append(st["total_s"])
                matches.append(st["match_s"])
                rows.append(dict(clients=n, sample=s, **st))
                print(f"{st['requests_s']:,.0f} ", end="", flush=True)
            if reqs:
                summary.append(dict(clients=n,
                                    req_s_median=bl.median(reqs),
                                    total_s_median=bl.median(tots),
                                    match_s_median=bl.median(matches),
                                    samples=len(reqs)))
        print()

    _report(summary)
    _write(runner, args, rows, summary)


def _report(summary: list[dict]) -> None:
    if not summary:
        print("\n[WARN] no successful samples collected.")
        return
    peak = max(summary, key=lambda r: r["req_s_median"])
    print(f"\n{'=' * 64}\n  Throughput saturation (closed-loop)\n{'=' * 64}")
    print(f"  {'clients':>8}{'req/s':>16}{'total resp/s':>16}{'match/s':>14}")
    print("  " + "-" * 54)
    for r in summary:
        mark = "  <- peak" if r is peak else ""
        print(f"  {r['clients']:>8}{r['req_s_median']:>16,.0f}"
              f"{r['total_s_median']:>16,.0f}{r['match_s_median']:>14,.0f}{mark}")
    print(f"\n  Peak sustained: {peak['req_s_median']:,.0f} req/s "
          f"at {peak['clients']} clients.\n")


def _write(runner, args, rows, summary) -> None:
    meta = bl.result_meta(runner, dict(clients=args.clients, samples=args.samples,
                                       duration=args.duration, seed=args.seed,
                                       mode="closed-loop"))
    jp = bl.write_json("saturation", {**meta, "summary": summary, "runs": rows})
    cp = bl.write_csv("saturation", rows,
                      ["clients", "sample", "requests_s", "total_s", "match_s",
                       "ack", "match", "err", "requests_sent", "elapsed"])
    print(f"  wrote {jp.relative_to(bl.REPO_ROOT)}")
    print(f"  wrote {cp.relative_to(bl.REPO_ROOT)}")


if __name__ == "__main__":
    main()
