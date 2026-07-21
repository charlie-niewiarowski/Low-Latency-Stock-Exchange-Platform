#!/usr/bin/env python3
"""
latency_curve.py — end-to-end latency vs offered load (supersedes bench.py).

For each target load we set the client's compile-time EXPECTED_THROUGHPUT rate
cap, rebuild client-release, run the pair, and keep only samples whose achieved
requests/s land inside the tolerance band. We then average the exchange's
end-to-end HDR percentiles for that level, producing the classic
latency-vs-throughput curve.

Process manipulation: edit-and-restore EXPECTED_THROUGHPUT (client rebuild per
level). The exchange is untouched (its always-on end-to-end histogram needs no
DIAGNOSTICS). Use --diagnostics to additionally capture per-stage histograms
(rebuilds the exchange too — see stage_breakdown.py for a focused version).

    python3 -m bench.latency_curve --levels 250000,1000000,3500000 --clients 10
"""

from __future__ import annotations

import argparse

from bench import benchlib as bl


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    bl.add_common_args(p)
    p.add_argument("--levels", type=bl.int_list,
                   default=[250_000, 1_000_000, 3_500_000],
                   help="comma-separated target loads in req/s")
    p.add_argument("--clients", type=int, default=10)
    p.add_argument("--samples", type=int, default=3, help="valid samples per level")
    p.add_argument("--tolerance-frac", type=float, default=0.2,
                   help="accept band as fraction of target (default: 0.2)")
    p.add_argument("--max-attempts", type=int, default=8, help="runs per level cap")
    p.add_argument("--diagnostics", action="store_true",
                   help="also enable per-stage histograms (rebuilds exchange)")
    args = p.parse_args()

    runner = bl.get_runner(args.runner)
    bl.emulation_banner(runner)
    if runner.emulated:
        print("  (latency below is emulated; use it for shape, not absolute ns)")

    cfgs = [bl.CLIENT_CFG] + ([bl.EXCHANGE_CFG] if args.diagnostics else [])
    targets = ["client-release"] + (["exchange-release"] if args.diagnostics else [])

    per_level: list[dict] = []
    csv_rows: list[dict] = []

    with bl.config_guard(cfgs):
        if args.diagnostics:
            bl.set_macro(bl.EXCHANGE_CFG, "DIAGNOSTICS", 1)

        for target in args.levels:
            lo = target * (1 - args.tolerance_frac)
            hi = target * (1 + args.tolerance_frac)
            print(f"\n{'-' * 72}\n  target {target:,} req/s  "
                  f"(band {lo:,.0f}-{hi:,.0f})\n{'-' * 72}")
            bl.set_macro(bl.CLIENT_CFG, "EXPECTED_THROUGHPUT", target)
            print("  building client ...", flush=True)
            bl.ensure_built(runner, targets, args.no_build)

            rates, hists = _collect(runner, args, target, lo, hi)
            if not rates:
                print(f"  [WARN] no in-band samples for {target:,}.")
                continue

            avg = bl.average_samples(hists)
            e2e = avg.get(bl.END_TO_END, {})
            achieved = bl.median(rates)
            per_level.append(dict(target=target, achieved=achieved,
                                  samples=len(rates), e2e=e2e, sections=avg))
            csv_rows.append(dict(target=target, achieved_rate=round(achieved),
                                 **{p: round(e2e.get(p, 0)) for p in bl.PCTS}))
            print(f"  achieved {achieved:,.0f} req/s over {len(rates)} sample(s)")
            bl.print_histograms(f"target {target:,} req/s  "
                                f"(achieved {achieved:,.0f})", avg)

    _write(runner, args, per_level, csv_rows)


def _collect(runner, args, target, lo, hi):
    rates, hists = [], []
    attempts = 0
    while len(rates) < args.samples and attempts < args.max_attempts:
        attempts += 1
        res = runner.run_pair(args.clients, args.seed, args.duration)
        res.save_log(bl.LOGS_DIR / f"latency_{target}_{attempts:02d}.log")
        st = res.stats
        if not st:
            print(f"  run {attempts:2d} [skip: no stats]")
            continue
        bl.warn_no_samples(res)
        h = res.histograms
        rate = st["requests_s"]
        in_band = lo <= rate <= hi
        tag = "OK " if in_band and h else "-- "
        print(f"  run {attempts:2d}  {rate:>12,.0f} req/s  {tag}"
              f"({len(rates) + (1 if in_band and h else 0)}/{args.samples})")
        if in_band and h:
            rates.append(rate)
            hists.append(h)
    return rates, hists


def _write(runner, args, per_level, csv_rows) -> None:
    meta = bl.result_meta(runner, dict(levels=args.levels, clients=args.clients,
                                       samples=args.samples, duration=args.duration,
                                       seed=args.seed,
                                       tolerance_frac=args.tolerance_frac,
                                       diagnostics=args.diagnostics))
    jp = bl.write_json("latency_curve", {**meta, "levels": per_level})
    cp = bl.write_csv("latency_curve", csv_rows,
                      ["target", "achieved_rate", *bl.PCTS])
    print(f"  wrote {jp.relative_to(bl.REPO_ROOT)}")
    print(f"  wrote {cp.relative_to(bl.REPO_ROOT)}")


if __name__ == "__main__":
    main()
