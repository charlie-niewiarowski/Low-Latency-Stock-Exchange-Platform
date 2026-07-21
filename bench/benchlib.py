#!/usr/bin/env python3
"""
benchlib — shared harness for the exchange/client benchmark suite.

Responsibilities:
  * locate the build tree and config headers,
  * auto-detect whether to run the native binaries (real latency, Linux x86-64)
    or drive them through Docker (functional throughput anywhere; latency is
    emulated on non-x86 hosts and flagged as such),
  * temporarily edit-and-restore compile-time config macros
    (EXPECTED_THROUGHPUT, DIAGNOSTICS, ...) — always restored, even on Ctrl-C,
  * launch an exchange+client pair for a fixed duration and capture their output,
  * parse the client "=== Stats ===" block and the exchange HDR histograms,
  * aggregate and emit results as console tables plus CSV/JSON.

The individual bench/*.py scripts build on these primitives.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import time
from contextlib import contextmanager
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

# ---------------------------------------------------------------------------
# paths
# ---------------------------------------------------------------------------

BENCH_DIR    = Path(__file__).resolve().parent
REPO_ROOT    = BENCH_DIR.parent
BUILD_DIR    = Path(os.environ.get("BENCH_BUILD_DIR", REPO_ROOT / "build"))
EXCHANGE_BIN = BUILD_DIR / "exchange" / "exchange-release"
CLIENT_BIN   = BUILD_DIR / "client"   / "client-release"

EXCHANGE_CFG = REPO_ROOT / "exchange" / "config" / "config.hpp"
CLIENT_CFG   = REPO_ROOT / "client"   / "config" / "config.hpp"

RESULTS_DIR   = BENCH_DIR / "results"
LOGS_DIR      = BENCH_DIR / "logs"
BASELINES_DIR = BENCH_DIR / "baselines"

# ---------------------------------------------------------------------------
# latency-output vocabulary (must match exchange/server/include/latency.hpp)
# ---------------------------------------------------------------------------

PCTS = ["p50", "p90", "p99", "p99.9", "p99.99", "max"]

SECTION_ORDER = [
    "begin read -> end write   (end-to-end)",
    "begin read -> end read    (recv syscall)",
    "end read -> push inbound  (inbound thread)",
    "push inbound -> engine pop (ring transit)",
    "engine pop -> engine push (engine processing)",
    "engine push -> server pop (ring transit)",
    "pop outbound -> begin write (outbound thread)",
    "begin write -> end write  (send syscall)",
]

END_TO_END = SECTION_ORDER[0]

# ---------------------------------------------------------------------------
# config-macro editing (transient; always restored via config_guard)
# ---------------------------------------------------------------------------

def _macro_re(name: str) -> re.Pattern:
    # Captures "#define NAME" in group 1 and the value token in group 2.
    return re.compile(rf"(#define\s+{re.escape(name)})\s+(\S+)")


def read_macro(cfg: Path, name: str) -> str | None:
    m = _macro_re(name).search(cfg.read_text())
    return m.group(2) if m else None


def set_macro(cfg: Path, name: str, value) -> None:
    text = cfg.read_text()
    new_text, n = _macro_re(name).subn(rf"\1 {value}", text)
    if n == 0:
        raise KeyError(f"macro {name!r} not found in {cfg}")
    cfg.write_text(new_text)


@contextmanager
def config_guard(paths):
    """
    Snapshot the full text of each config file, yield, then restore verbatim —
    on normal exit, exception, or SIGINT/SIGTERM. Guarantees the source tree is
    left byte-for-byte unchanged no matter how the run ends.
    """
    paths = [Path(p) for p in paths]
    snapshots = {p: p.read_text() for p in paths}

    def _restore(*_):
        for p, text in snapshots.items():
            try:
                if p.read_text() != text:
                    p.write_text(text)
            except OSError:
                p.write_text(text)

    prev_int  = signal.getsignal(signal.SIGINT)
    prev_term = signal.getsignal(signal.SIGTERM)

    def _on_signal(signum, frame):
        _restore()
        # re-raise default behaviour so the script actually stops
        signal.signal(signum, prev_int if signum == signal.SIGINT else prev_term)
        os.kill(os.getpid(), signum)

    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)
    try:
        yield
    finally:
        _restore()
        signal.signal(signal.SIGINT, prev_int)
        signal.signal(signal.SIGTERM, prev_term)

# ---------------------------------------------------------------------------
# output parsers
# ---------------------------------------------------------------------------

_STATS_FIELDS = {
    "elapsed":          r"elapsed\s*:\s*([\d.]+)\s*s",
    "requests_sent":    r"requests sent\s*:\s*(\d+)",
    "ack":              r"ACK responses\s*:\s*(\d+)",
    "match":            r"MATCH responses\s*:\s*(\d+)",
    "err":              r"ERR responses\s*:\s*(\d+)",
    "orders_in_flight": r"orders in flight\s*:\s*(-?\d+)",
    "requests_s":       r"requests/s\s*:\s*([\d.]+)",
    "ack_s":            r"ACK/s\s*:\s*([\d.]+)",
    "match_s":          r"MATCH/s\s*:\s*([\d.]+)",
    "err_s":            r"ERR/s\s*:\s*([\d.]+)",
    "total_s":          r"total resp/s\s*:\s*([\d.]+)",
}
_INT_FIELDS = {"requests_sent", "ack", "match", "err", "orders_in_flight"}


def parse_client_stats(client_stderr: str) -> dict | None:
    """Parse the client '=== Stats ===' block (stderr). None if absent."""
    out: dict = {}
    for key, pat in _STATS_FIELDS.items():
        m = re.search(pat, client_stderr)
        if m:
            out[key] = int(m.group(1)) if key in _INT_FIELDS else float(m.group(1))
    return out if "requests_s" in out else None


def parse_histograms(exchange_stdout: str) -> dict[str, dict[str, int]]:
    """
    Parse all print_hist() blocks from exchange stdout. Section titles contain
    ' -> '; data rows look like 'p99.9   1234 ns'. Returns {section: {pct: ns}}.
    """
    result: dict[str, dict[str, int]] = {}
    current: str | None = None
    for raw in exchange_stdout.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = re.match(r"^(p[\d.]+|max)\s+(\d+)\s+ns$", line)
        if m and current is not None:
            result.setdefault(current, {})[m.group(1)] = int(m.group(2))
        elif " -> " in line:
            current = line
    return result

# ---------------------------------------------------------------------------
# run harness
# ---------------------------------------------------------------------------

@dataclass
class RunResult:
    client_stderr: str = ""
    exchange_stdout: str = ""
    exchange_stderr: str = ""

    @property
    def stats(self) -> dict | None:
        return parse_client_stats(self.client_stderr)

    @property
    def histograms(self) -> dict[str, dict[str, int]]:
        return parse_histograms(self.exchange_stdout)

    def save_log(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            "=== CLIENT STDERR ===\n" + self.client_stderr
            + "\n=== EXCHANGE STDOUT ===\n" + self.exchange_stdout
            + "\n=== EXCHANGE STDERR ===\n" + self.exchange_stderr
        )


X86 = {"x86_64", "amd64", "AMD64"}


class Runner:
    """Abstract launcher for an exchange+client pair."""
    name = "abstract"
    emulated = False

    def configure(self) -> None: ...
    def build(self, targets: list[str]) -> None: ...
    def run_pair(self, num_clients: int, seed: int | None, duration: float) -> RunResult:
        raise NotImplementedError


class NativeRunner(Runner):
    """Runs the release binaries directly (real latency on Linux x86-64)."""
    name = "native"
    emulated = False

    def configure(self) -> None:
        _run(["cmake", "-S", str(REPO_ROOT), "-B", str(BUILD_DIR)])

    def build(self, targets: list[str]) -> None:
        _run(["cmake", "--build", str(BUILD_DIR), "--target", *targets,
              f"-j{os.cpu_count() or 4}"])

    def run_pair(self, num_clients, seed, duration) -> RunResult:
        xproc = subprocess.Popen([str(EXCHANGE_BIN)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(0.5)
        if xproc.poll() is not None:
            raise RuntimeError("exchange exited immediately — port already in use?")

        cmd = [str(CLIENT_BIN), str(num_clients)]
        if seed is not None:
            cmd.append(str(seed))
        cproc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        time.sleep(duration)

        cproc.send_signal(signal.SIGTERM)
        try:
            _, c_err = cproc.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            cproc.kill()
            _, c_err = cproc.communicate()

        xproc.send_signal(signal.SIGTERM)
        try:
            x_out, x_err = xproc.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            xproc.kill()
            x_out, x_err = xproc.communicate()

        time.sleep(1)  # let the port leave TIME_WAIT
        return RunResult(
            client_stderr=c_err.decode(errors="replace"),
            exchange_stdout=x_out.decode(errors="replace"),
            exchange_stderr=x_err.decode(errors="replace"),
        )


_D_CL_ERR = "@@@CLIENT_STDERR@@@"
_D_EX_OUT = "@@@EXCHANGE_STDOUT@@@"
_D_EX_ERR = "@@@EXCHANGE_STDERR@@@"
_D_END    = "@@@END@@@"


class DockerRunner(Runner):
    """
    Drives both binaries inside one `docker compose run --rm dev` shell, sharing
    loopback so the client's compiled-in 127.0.0.1 reaches the exchange. Each
    process writes to the container-local /tmp (fast, no bind-mount lag); the
    outputs are then streamed back over the compose command's stdout between
    delimiters, so the host never depends on macOS bind-mount propagation.
    """
    name = "docker"

    def __init__(self):
        self.emulated = platform.machine() not in X86

    def _compose(self, shell_cmd: str, timeout: float):
        return _run(
            ["docker", "compose", "run", "--rm", "-T", "dev", "sh", "-c", shell_cmd],
            cwd=REPO_ROOT, timeout=timeout,
        )

    def configure(self) -> None:
        self._compose("cmake -S . -B build", timeout=600)

    def build(self, targets: list[str]) -> None:
        tgt = " ".join(f"--target {t}" for t in targets)
        self._compose(f'cmake --build build {tgt} -j"$(nproc)"', timeout=1800)

    def run_pair(self, num_clients, seed, duration) -> RunResult:
        seed_arg = f" {seed}" if seed is not None else ""
        script = (
            "./build/exchange/exchange-release >/tmp/ex.out 2>/tmp/ex.err & XPID=$!; "
            "sleep 0.7; "
            f"./build/client/client-release {num_clients}{seed_arg} "
            ">/tmp/cl.out 2>/tmp/cl.err & CPID=$!; "
            f"sleep {duration}; "
            "kill -INT $CPID 2>/dev/null; wait $CPID 2>/dev/null; "
            "sleep 0.5; "
            "kill -INT $XPID 2>/dev/null; wait $XPID 2>/dev/null; "
            f"echo '{_D_CL_ERR}'; cat /tmp/cl.err; "
            f"echo '{_D_EX_OUT}'; cat /tmp/ex.out; "
            f"echo '{_D_EX_ERR}'; cat /tmp/ex.err; "
            f"echo '{_D_END}'"
        )
        # Tolerant of a nonzero rc (the kill/wait can propagate 130); we validate
        # via the delimiters instead.
        r = subprocess.run(
            ["docker", "compose", "run", "--rm", "-T", "dev", "sh", "-c", script],
            cwd=REPO_ROOT, timeout=duration + 120,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        out = r.stdout.decode(errors="replace")
        if _D_END not in out:
            raise RuntimeError("docker run produced no delimited output:\n"
                               + r.stderr.decode(errors="replace")[-500:])
        return RunResult(
            client_stderr=_slice(out, _D_CL_ERR, _D_EX_OUT),
            exchange_stdout=_slice(out, _D_EX_OUT, _D_EX_ERR),
            exchange_stderr=_slice(out, _D_EX_ERR, _D_END),
        )


def _slice(text: str, start: str, end: str) -> str:
    a = text.find(start)
    b = text.find(end, a + 1)
    if a < 0 or b < 0:
        return ""
    return text[a + len(start):b].lstrip("\n")


def _run(cmd, cwd=None, timeout=None) -> subprocess.CompletedProcess:
    r = subprocess.run(cmd, cwd=cwd, timeout=timeout,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.exit(f"[ERROR] command failed ({r.returncode}): {' '.join(map(str, cmd))}\n"
                 + r.stderr.decode(errors="replace"))
    return r


def get_runner(force: str = "auto") -> Runner:
    """force ∈ {auto, native, docker}."""
    native_ok = (platform.system() == "Linux"
                 and platform.machine() in X86
                 and EXCHANGE_BIN.exists() and CLIENT_BIN.exists())
    if force == "native":
        if platform.system() != "Linux" or platform.machine() not in X86:
            sys.exit("[ERROR] --runner native requires Linux x86-64.")
        return NativeRunner()
    if force == "docker":
        if not shutil.which("docker"):
            sys.exit("[ERROR] --runner docker requested but docker is not installed.")
        return DockerRunner()
    # auto
    if native_ok:
        return NativeRunner()
    if shutil.which("docker"):
        return DockerRunner()
    sys.exit("[ERROR] no native binaries and docker unavailable — cannot run.")

# ---------------------------------------------------------------------------
# aggregation
# ---------------------------------------------------------------------------

def median(xs: list[float]) -> float:
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return 0.0
    mid = n // 2
    return s[mid] if n % 2 else (s[mid - 1] + s[mid]) / 2


def average_samples(samples: list[dict[str, dict[str, int]]]) -> dict[str, dict[str, float]]:
    """Average each percentile across samples, per histogram section."""
    all_secs = {sec for s in samples for sec in s}
    out: dict[str, dict[str, float]] = {}
    for sec in all_secs:
        out[sec] = {}
        for pct in PCTS:
            vals = [s[sec][pct] for s in samples if sec in s and pct in s[sec]]
            if vals:
                out[sec][pct] = sum(vals) / len(vals)
    return out

# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------

_COL, _PCOL = 50, 10
_BAR = "=" * (_COL + 2 + _PCOL * len(PCTS))


def hist_header() -> str:
    return "  " + f"{'Section':<{_COL}}" + "".join(f"{p:>{_PCOL}}" for p in PCTS)


def hist_row(label: str, d: dict[str, float]) -> str:
    label = label if len(label) <= _COL else label[:_COL - 3] + "..."
    vals = "".join(f"{d.get(p, 0):>{_PCOL},.0f}" for p in PCTS)
    return f"  {label:<{_COL}}{vals}"


def print_histograms(title: str, averaged: dict[str, dict[str, float]]) -> None:
    print(f"\n{_BAR}\n  {title}\n{_BAR}")
    print(hist_header())
    print("  " + "-" * (_COL + _PCOL * len(PCTS)))
    ordered = [s for s in SECTION_ORDER if s in averaged]
    extras  = [s for s in averaged if s not in SECTION_ORDER]
    for sec in ordered + extras:
        print(hist_row(sec, averaged[sec]))
    print()


def emulation_banner(runner: Runner) -> None:
    print(f"[runner] {runner.name}", end="")
    if runner.emulated:
        print("  ***  LATENCY EMULATED — throughput valid, latency numbers "
              "are NOT authoritative (run on native Linux x86-64 for real latency)")
    else:
        print()

# ---------------------------------------------------------------------------
# result output (CSV / JSON)
# ---------------------------------------------------------------------------

def _stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")


def write_json(script: str, payload: dict) -> Path:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    path = RESULTS_DIR / f"{script}_{_stamp()}.json"
    path.write_text(json.dumps(payload, indent=2))
    return path


def write_csv(script: str, rows: list[dict], fieldnames: list[str]) -> Path:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    path = RESULTS_DIR / f"{script}_{_stamp()}.csv"
    with path.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fieldnames)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in fieldnames})
    return path


def result_meta(runner: Runner, params: dict) -> dict:
    return {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "runner": runner.name,
        "emulated": runner.emulated,
        "host": {"system": platform.system(), "machine": platform.machine()},
        "params": params,
    }

# ---------------------------------------------------------------------------
# CLI helpers shared by the scripts
# ---------------------------------------------------------------------------

def add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--runner", choices=["auto", "native", "docker"], default="auto",
                        help="execution backend (default: auto-detect)")
    parser.add_argument("--duration", type=float, default=15.0,
                        help="seconds per run (default: 15)")
    parser.add_argument("--seed", type=int, default=42,
                        help="client RNG seed for reproducibility (default: 42)")
    parser.add_argument("--no-build", action="store_true",
                        help="skip configure/build; assume binaries are current")


def int_list(s: str) -> list[int]:
    return [int(x) for x in s.replace(" ", "").split(",") if x]


def ensure_built(runner: Runner, targets: list[str], skip: bool) -> None:
    if skip:
        return
    runner.configure()
    runner.build(targets)


def warn_no_samples(res: RunResult) -> bool:
    """True (with a warning) if the exchange collected no latency samples."""
    if "no samples collected" in res.exchange_stdout:
        print("  [WARN] exchange collected 0 latency samples — increase --duration "
              "or --clients so traffic exceeds LATENCY_SAMPLE_DROP (100k).")
        return True
    return False
