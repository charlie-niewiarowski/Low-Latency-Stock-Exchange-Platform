#!/usr/bin/env python3
"""
stage_breakdown.py — per-stage latency breakdown to locate the bottleneck.

Enables DIAGNOSTICS=1 in BOTH config headers (so the exchange timestamps every
pipeline stage t0..t7), rebuilds BOTH release targets, runs one long pass at a
fixed client count and load, and reports each of the 8 stage histograms plus each
stage's share of the end-to-end p50/p99. Configs are restored afterwards, and the
binaries are rebuilt back to DIAGNOSTICS=0 (unless --no-rebuild-after).

Process manipulation: edit-and-restore DIAGNOSTICS (both configs, both rebuilds);
optional EXPECTED_THROUGHPUT via --load (0 = closed-loop, the default, to push
enough traffic past the 100k warm-up drop).

    python3 -m bench.stage_breakdown --clients 16 --duration 20
"""

from __future__ import annotations

import argparse

from bench import benchlib as bl


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    bl.add_common_args(p)
    p.add_argument("--clients", type=int, default=16)
    p.add_argument("--load", type=int, default=0,
                   help="EXPECTED_THROUGHPUT (0 = closed-loop, default)")
    p.add_argument("--samples", type=int, default=3, help="runs to average")
    p.add_argument("--no-rebuild-after", action="store_true",
                   help="leave DIAGNOSTICS=1 binaries in place after the run")
    args = p.parse_args()

    runner = bl.get_runner(args.runner)
    bl.emulation_banner(runner)
    if runner.emulated:
        print("  (stage ns are emulated; use the *proportions* to find the "
              "bottleneck, not the absolute numbers)")

    cfgs = [bl.CLIENT_CFG, bl.EXCHANGE_CFG]
    targets = ["exchange-release", "client-release"]
    orig_load = bl.read_macro(bl.CLIENT_CFG, "EXPECTED_THROUGHPUT")

    hists: list[dict] = []
    with bl.config_guard(cfgs):
        bl.set_macro(bl.EXCHANGE_CFG, "DIAGNOSTICS", 1)
        bl.set_macro(bl.CLIENT_CFG, "DIAGNOSTICS", 1)
        if str(args.load) != orig_load:
            bl.set_macro(bl.CLIENT_CFG, "EXPECTED_THROUGHPUT", args.load)
        print("  building exchange + client with DIAGNOSTICS=1 ...", flush=True)
        bl.ensure_built(runner, targets, args.no_build)

        for s in range(1, args.samples + 1):
            res = runner.run_pair(args.clients, args.seed, args.duration)
            res.save_log(bl.LOGS_DIR / f"stage_s{s}.log")
            if bl.warn_no_samples(res):
                continue
            h = res.histograms
            if h:
                hists.append(h)
                print(f"  run {s}: {len(h)} sections")
            else:
                print(f"  run {s}: [no histograms]")

    # config_guard has now restored the DIAGNOSTICS=0 text on disk; rebuild the
    # binaries back to match unless the caller opted out.
    if not args.no_rebuild_after and not args.no_build:
        print("  rebuilding DIAGNOSTICS=0 binaries ...", flush=True)
        bl.ensure_built(runner, targets, skip=False)

    if not hists:
        print("\n[WARN] no per-stage histograms captured (emulated runs are slow — "
              "raise --duration/--clients).")
        return

    avg = bl.average_samples(hists)
    bl.print_histograms(f"per-stage latency  (clients={args.clients}, "
                        f"load={'closed-loop' if args.load == 0 else args.load})", avg)
    _print_shares(avg)
    _write(runner, args, avg)


def _print_shares(avg: dict[str, dict[str, float]]) -> None:
    e2e = avg.get(bl.END_TO_END)
    if not e2e:
        return
    stages = [s for s in bl.SECTION_ORDER[1:] if s in avg]
    print(f"  {'stage share of end-to-end':<52}{'p50':>10}{'p99':>10}")
    print("  " + "-" * 72)
    for s in stages:
        d = avg[s]
        f50 = 100 * d.get("p50", 0) / e2e["p50"] if e2e.get("p50") else 0
        f99 = 100 * d.get("p99", 0) / e2e["p99"] if e2e.get("p99") else 0
        label = s.split("(")[-1].rstrip(")")
        print(f"  {label:<52}{f50:>9.1f}%{f99:>9.1f}%")
    print()


def _write(runner, args, avg) -> None:
    meta = bl.result_meta(runner, dict(clients=args.clients, load=args.load,
                                       duration=args.duration, samples=args.samples,
                                       seed=args.seed))
    rows = [dict(section=sec, **{p: round(d.get(p, 0)) for p in bl.PCTS})
            for sec, d in avg.items()]
    jp = bl.write_json("stage_breakdown", {**meta, "sections": avg})
    cp = bl.write_csv("stage_breakdown", rows, ["section", *bl.PCTS])
    print(f"  wrote {jp.relative_to(bl.REPO_ROOT)}")
    print(f"  wrote {cp.relative_to(bl.REPO_ROOT)}")


if __name__ == "__main__":
    main()
