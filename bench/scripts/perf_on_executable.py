#!/usr/bin/env python3
"""
perf_on_executable.py — generic `perf stat` runner for ANY benchmark executable.

This is the "tool" half of bench/'s N-executables x M-tools split: it knows
nothing about ring buffers or order books, only how to run `perf stat -r N`
against a given binary+args, parse the report, and hand back structured
results. A target-specific script (e.g. orderbook_perf.py) supplies the N side
(which executable, which workload args) and calls run_perf_stat() directly —
adding a new perf-measured benchmark should never require touching this file.

Repeats the whole run `--reps` times (perf's own `-r`) and reports mean +-
relative stddev, never a single best trial. Requests are split into small
event groups (default: one cache group, one branch group) because most chips
have far fewer physical PMU counters than the hardware events perf exposes;
cramming everything into one run causes counter multiplexing, where perf
scales up sampled-not-measured counts and reports a noisier estimate.

    python3 -m bench.scripts.perf_on_executable \\
        -i build/bench/executables/orderbook-bench -r 20 \\
        -e cache-references,cache-misses -e branches,branch-misses \\
        -- --ops 10000000 --seed 7 --core 6
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

DEFAULT_GROUPS = ["cache-references,cache-misses", "branches,branch-misses"]

# Matches one `perf stat` report line, e.g.:
#   "       141,496,490      cache-references                ( +-  1.09% )"
#   "        49,934,287      cache-misses      #   34.449 % of all cache refs      ( +-  0.17% )"
# When a "# X % of ..." comment is present, the trailing "(+- Y%)" is the
# relative stddev of THAT ratio (not of the raw count) — which is exactly the
# number a miss-rate claim wants, straight from perf.
_LINE_RE = re.compile(
    r'^\s*(?P<value><not counted>|<not supported>|[\d,]+)\s+'
    r'(?P<name>[A-Za-z0-9_.\-]+)'
    r'(?:\s+#\s+(?P<pct>[\d.]+)\s*%\s*of\s*(?P<pct_of>.+?))?'
    r'(?:\s+\(\s*\+-\s*(?P<stddev>[\d.]+)%\s*\))?\s*$'
)


@dataclass
class PerfEvent:
    name: str
    value: float | None                 # raw counter (None if not counted/supported)
    stddev_pct: float | None = None     # relative stddev of the displayed number
    ratio_pct: float | None = None      # the "# XX %" figure, if perf printed one
    ratio_of: str | None = None         # what that ratio is "of" (e.g. "all cache refs")


def parse_perf_stat(stderr_text: str) -> dict[str, PerfEvent]:
    events: dict[str, PerfEvent] = {}
    for raw in stderr_text.splitlines():
        m = _LINE_RE.match(raw)
        if not m:
            continue
        val = m.group("value")
        value = None if val.startswith("<") else float(val.replace(",", ""))
        ev = PerfEvent(
            name=m.group("name"),
            value=value,
            stddev_pct=float(m.group("stddev")) if m.group("stddev") else None,
            ratio_pct=float(m.group("pct")) if m.group("pct") else None,
            ratio_of=m.group("pct_of"),
        )
        events[ev.name] = ev
    return events


def require_perf() -> None:
    if not shutil.which("perf"):
        sys.exit("[ERROR] `perf` not found on PATH — install linux-perf / linux-tools.")


def run_perf_stat(exe, exe_args: list[str], events: list[str],
                   reps: int = 10, timeout: float | None = None) -> dict[str, PerfEvent]:
    """
    Run `perf stat -r <reps> -e <events> <exe> <exe_args>`, return results keyed
    by event name. Keep `events` to 2-3 per call (see module docstring).
    """
    cmd = ["perf", "stat", "-r", str(reps), "-e", ",".join(events), str(exe), *exe_args]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=timeout, text=True)
    if r.returncode != 0 and "Performance counter stats" not in r.stderr:
        raise RuntimeError(f"perf stat failed ({r.returncode}): {' '.join(cmd)}\n{r.stderr}")
    return parse_perf_stat(r.stderr)


def run_groups(exe, exe_args: list[str], groups: list[str],
               reps: int = 10, timeout: float | None = None) -> dict[str, PerfEvent]:
    """Run each event group as a separate `perf stat` invocation, merge results."""
    merged: dict[str, PerfEvent] = {}
    for group in groups:
        merged.update(run_perf_stat(exe, exe_args, group.split(","), reps=reps, timeout=timeout))
    return merged


def format_event(ev: PerfEvent) -> str:
    if ev.value is None:
        return f"{ev.name:<20} <not counted>"
    line = f"{ev.name:<20} {ev.value:>18,.0f}"
    if ev.ratio_pct is not None:
        line += f"   # {ev.ratio_pct:.3f}% of {ev.ratio_of}"
    if ev.stddev_pct is not None:
        line += f"   ( +- {ev.stddev_pct:.2f}% )"
    return line


def _valid_executable(path_str: str) -> str:
    p = Path(path_str)
    if not p.is_file():
        raise argparse.ArgumentTypeError(f"'{path_str}' is not a file")
    if not (p.stat().st_mode & 0o111):
        raise argparse.ArgumentTypeError(f"'{path_str}' is not executable")
    return path_str


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("-i", "--input", type=_valid_executable, required=True,
                   help="path to the executable to profile")
    p.add_argument("-e", "--events", action="append", default=[],
                   help="comma-separated event group; repeat -e per group "
                        "(default: cache-references,cache-misses and "
                        "branches,branch-misses)")
    p.add_argument("-r", "--reps", type=int, default=10,
                   help="perf stat -r: repeat the whole run this many times, "
                        "report mean +- relative stddev (default: 10)")
    p.add_argument("exe_args", nargs=argparse.REMAINDER,
                   help="passthrough args for the executable, after --")
    args = p.parse_args()

    require_perf()
    groups = args.events or DEFAULT_GROUPS
    exe_args = args.exe_args[1:] if args.exe_args[:1] == ["--"] else args.exe_args

    print(f"perf stat: {args.input}  (reps={args.reps}, args={' '.join(exe_args) or '(none)'})")
    for group in groups:
        result = run_groups(args.input, exe_args, [group], reps=args.reps)
        for name in group.split(","):
            ev = result.get(name)
            print(f"  {format_event(ev) if ev else name + ': <no data>'}")


if __name__ == "__main__":
    main()
