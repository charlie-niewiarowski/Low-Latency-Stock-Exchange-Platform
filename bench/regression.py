#!/usr/bin/env python3
"""
regression.py — fixed-seed performance/functional gate against a stored baseline.

Runs one deterministic-seed pass at fixed client count and load, then compares
throughput against a committed baseline and checks functional invariants
(err == 0, match > 0, ack > 0). Exits non-zero on regression, so it works as a CI
gate. Baselines are keyed by runner type (native vs docker) because the absolute
numbers differ across environments.

    python3 -m bench.regression --update              # write/refresh the baseline
    python3 -m bench.regression                       # check against baseline

Process manipulation: none permanent — closed-loop by default (or --load sets and
restores EXPECTED_THROUGHPUT). Deterministic --seed keeps runs comparable.
"""

from __future__ import annotations

import argparse
import json
import sys
from contextlib import nullcontext

from bench import benchlib as bl


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    bl.add_common_args(p)
    p.add_argument("--clients", type=int, default=8)
    p.add_argument("--load", type=int, default=0,
                   help="EXPECTED_THROUGHPUT (0 = closed-loop, default)")
    p.add_argument("--samples", type=int, default=3, help="runs to median")
    p.add_argument("--tolerance-frac", type=float, default=0.15,
                   help="allowed throughput drop vs baseline (default: 0.15)")
    p.add_argument("--name", default="default", help="baseline name")
    p.add_argument("--update", action="store_true",
                   help="write the measured result as the new baseline")
    args = p.parse_args()

    runner = bl.get_runner(args.runner)
    bl.emulation_banner(runner)

    cur = bl.read_macro(bl.CLIENT_CFG, "EXPECTED_THROUGHPUT")
    change = str(args.load) != cur
    guard = bl.config_guard([bl.CLIENT_CFG]) if change else nullcontext()

    reqs, acks, matches, errs = [], [], [], []
    with guard:
        if change:
            bl.set_macro(bl.CLIENT_CFG, "EXPECTED_THROUGHPUT", args.load)
        bl.ensure_built(runner, ["exchange-release", "client-release"], args.no_build)

        for s in range(1, args.samples + 1):
            res = runner.run_pair(args.clients, args.seed, args.duration)
            res.save_log(bl.LOGS_DIR / f"regression_s{s}.log")
            st = res.stats
            if not st:
                print(f"  run {s}: [no stats]")
                continue
            reqs.append(st["requests_s"])
            acks.append(st["ack"]); matches.append(st["match"]); errs.append(st["err"])
            print(f"  run {s}: {st['requests_s']:,.0f} req/s  "
                  f"ack={st['ack']} match={st['match']} err={st['err']}")

    if not reqs:
        print("[FAIL] no successful runs.")
        return 1

    measured = dict(req_s=bl.median(reqs), ack=int(bl.median(acks)),
                    match=int(bl.median(matches)), err=max(errs),
                    clients=args.clients, load=args.load, seed=args.seed,
                    duration=args.duration, emulated=runner.emulated)

    bpath = bl.BASELINES_DIR / f"{args.name}.{runner.name}.json"
    if args.update:
        bl.BASELINES_DIR.mkdir(parents=True, exist_ok=True)
        bpath.write_text(json.dumps(measured, indent=2))
        print(f"\n[UPDATED] baseline {bpath.relative_to(bl.REPO_ROOT)}\n"
              f"  {measured['req_s']:,.0f} req/s")
        return 0

    return _check(measured, bpath, args.tolerance_frac)


def _check(measured: dict, bpath, tol: float) -> int:
    fails = []
    if measured["err"] != 0:
        fails.append(f"err={measured['err']} (want 0)")
    if measured["match"] <= 0:
        fails.append("match==0 (want > 0)")
    if measured["ack"] <= 0:
        fails.append("ack==0 (want > 0)")

    if not bpath.exists():
        print(f"\n[WARN] no baseline at {bpath.relative_to(bl.REPO_ROOT)} — run with "
              f"--update first. Functional checks only.")
    else:
        base = json.loads(bpath.read_text())
        floor = base["req_s"] * (1 - tol)
        print(f"\n  baseline {base['req_s']:,.0f} req/s   measured "
              f"{measured['req_s']:,.0f} req/s   floor {floor:,.0f} "
              f"(-{tol:.0%})")
        if measured["req_s"] < floor:
            fails.append(f"throughput {measured['req_s']:,.0f} < floor {floor:,.0f}")

    if fails:
        print("\n[FAIL] regression:\n    " + "\n    ".join(fails) + "\n")
        return 1
    print("\n[PASS] within tolerance and invariants hold.\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
