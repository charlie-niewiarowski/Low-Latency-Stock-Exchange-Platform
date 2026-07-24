# Client-Exchange Simulation

A low-latency order exchange simulation written in C++23. It consists of two separate programs: an exchange server that runs a price-time-priority matching engine behind a TCP gateway, and a load-generator client that drives it with synthetic order traffic. The primary goal is to measure and minimize end-to-end order processing latency.

---

## Disclaimer

DISCLAIMER, the latency benchmarks may not be accurate at the moment as I do not have access to a Linux system. The last tested commit should be 7db090536f36ae9112bc3f369f89384e58b3bdb0

---

## Overview

The exchange processes three order operations: NEW, CANCEL, and MODIFY. It supports both LIMIT and MARKET order types. On shutdown the exchange prints HDR histogram latency data covering the full round-trip as well as each internal segment (recv syscall, ring transit, engine processing, send syscall, and so on).

The client opens N concurrent TCP connections and keeps each one pipelined with up to 32 in-flight requests at a time. Orders are generated synthetically using a geometric Brownian motion mid-price model.

---

## Repository layout

```
Client-Exchange-Simulation/
  exchange/
    app/          Exchange entry point and top-level Exchange class
    engine/       Matching engine (order book, matching logic, tests)
    server/       TCP gateway (epoll inbound/outbound threads, tests)
    include/      Shared lock-free ring buffer
    config/       exchange/config/config.hpp  -- all compile-time knobs
  client/
    app/          Client entry point
    src/          LoadGenerator (epoll event loop)
    include/      ClientState, LoadGenerator, OrderFactory
    config/       client/config/config.hpp  -- all compile-time knobs
  infra/          Wire types used by both programs
    protocol.hpp    Frame sizes, status prefixes, error strings
    communication_types.hpp  InboundMessage, OutboundMessage structs
    order_types.hpp  Primitive type aliases (Price, Quantity, OrderId, etc.)
    buffer.hpp      Header-only fixed-capacity byte buffer
  bench/          Python benchmark suite (throughput, latency, regression)
  CMakeLists.txt  Root build file
```

---

## Prerequisites

**Native Linux build:**
- Linux (the server uses `epoll` and thread pinning via `pthread_setaffinity_np`)
- GCC or Clang with C++23 support
- CMake 3.25 or newer
- Python 3.9 or newer (only needed for the `bench/` suite)
- An internet connection on first build (CMake fetches HdrHistogram_c via FetchContent)\

**macOS / Windows (Docker):**
- Docker Desktop
- Apple Silicon Macs must enable Rosetta in Docker Desktop settings

The project has been developed and tested on x86-64. The `__rdtsc` intrinsic and `-march=native` are used throughout, so it will not build correctly on non-x86 targets without modification. Under Docker on Apple Silicon the simulation runs via x86-64 emulation. It's functional but not suitable for latency benchmarking.

## Linux Build

```bash
# Clone and enter the repo
git clone <repo-url>
cd Client-Exchange-Simulation

# Configure (downloads hdr_histogram on first run)
cmake -S . -B build

# Build all targets
cmake --build build -j$(nproc)
```

This produces four binaries:

| Binary | Path | Notes |
|---|---|---|
| `exchange-release` | `build/exchange/exchange-release` | `-O3 -march=native` |
| `exchange-debug`   | `build/exchange/exchange-debug`   | AddressSanitizer + UBSanitizer |
| `client-release`   | `build/client/client-release`     | `-O3 -march=native` |
| `client-debug`     | `build/client/client-debug`       | `-g -march=native` |

Individual targets can be built in isolation:

```bash
cmake --build build --target exchange-release
cmake --build build --target client-release
```

---

## Linux Run

Start the exchange first, then start the client in a separate terminal:

```bash
# Terminal 1
./build/exchange/exchange-release

# Terminal 2  (10 clients, random seed)
./build/client/client-release 10

# Terminal 2  (10 clients, fixed seed for reproducibility)
./build/client/client-release 10 42
```

Stop both with Ctrl-C. When the exchange stops it prints the latency histograms to stdout. The client prints aggregate throughput stats to stderr.

---

## Docker (macOS / Windows)

The Docker image is a pure Linux build environment. Your source tree is mounted into the container, and the build directory lives in a dedicated named volume — kept separate from any host `./build` so a macOS/Homebrew CMake cache can never collide with the Linux one. Builds persist across runs and you interact with the project exactly as you would on a native Linux machine.

```bash
# Build the image once (or after changing the Dockerfile)
docker compose build
```

Then, whenever you open a terminal, run this one command to enter a Linux shell:

```bash
docker compose run --rm dev
```

You are now inside the container at `/app` (your project root). Everything from here is identical to working on Linux:

```bash
cmake -S . -B build                          # configure (once)
cmake --build build -j$(nproc)              # build everything
./build/exchange/exchange-release            # run the exchange
./build/client/client-release 10            # run the client
```

**Dedicated exchange / client services:**  
The Compose file also defines `exchange` and `client` services so you can build and run the two programs with separate commands instead of a shared shell. First build the release binaries once (they persist in the shared `build` volume). Wrap any command using `$(nproc)` in `sh -c '...'` so it expands inside the container rather than your host shell:

```bash
docker compose run --rm dev cmake -S . -B build                          # configure (once)
docker compose run --rm dev cmake --build build --target exchange-release
docker compose run --rm dev cmake --build build --target client-release
```

Then run each side with its own command:

```bash
# Terminal 1 — exchange
docker compose run --rm exchange

# Terminal 2 — client (auto-starts the exchange if it isn't already up)
docker compose run --rm client          # 10 clients (default)
docker compose run --rm client 20 42    # 20 clients, fixed seed 42
```

The client connects to `127.0.0.1`, so its service joins the exchange's network namespace (`network_mode: "service:exchange"`) — no source changes needed. The client's reconnect backoff covers the brief window before the exchange is listening. Stop each with Ctrl-C.

If you prefer a single interactive shell instead, use a named container so a second terminal can attach to it:

```bash
# Terminal 1 — start a named container
docker compose run --rm --name sim dev
# inside: ./build/exchange/exchange-release

# Terminal 2 — attach to the same container
docker exec -it sim bash
# inside: ./build/client/client-release 10
```

---

## Example Client Output
```
=== Stats ===
  elapsed         : 15.003 s
  requests sent   : 15243872
  ACK responses   : 15243872
  MATCH responses : 1884231
  ERR responses   : 0
  orders in flight: 0
  --- throughput ---
  requests/s      : 1016005
  ACK/s           : 1016005
  MATCH/s         : 125587
  ERR/s           : 0
  total resp/s    : 1016005
```

---

## Configuration

All compile-time configuration lives in header files. A rebuild is required after changes.

### Exchange: `exchange/config/config.hpp`

| Macro | Default | Description |
|---|---|---|
| `LOGGING` | `0` | Print matched trades to stdout |
| `DIAGNOSTICS` | `0` | Collect per-segment TSC timestamps; enables detailed latency histograms |
| `TESTING` | `0` | Expose `Engine::step()` and inspection accessors for unit tests |
| `MIN_PRICE` | `1` | Minimum valid limit price (integer ticks) |
| `MAX_PRICE` | `100000` | Maximum valid limit price; $1000.00 = 100000 ticks |
| `MATCHING_CORE` | `1` | CPU core the matching thread is pinned to |
| `INBOUND_CORE` | `2` | CPU core the inbound TCP thread is pinned to |
| `OUTBOUND_CORE` | `3` | CPU core the outbound TCP thread is pinned to |
| `PORT` | `"4000"` | TCP port the exchange listens on |
| `MAX_CLIENTS` | `64` | Maximum simultaneous connections |
| `PIPELINE_DEPTH` | `32` | Per-connection outbound staging ring capacity |
| `COMMUNICATION_RING_COUNT` | `524288` | Inbound and outbound SPSC ring sizes |
| `LATENCY_SAMPLE_COUNT` | `100000000` | How many samples to collect before stopping |
| `LATENCY_SAMPLE_DROP` | `100000` | Cold-start samples to discard before recording |

### Client: `client/config/config.hpp`

| Macro | Default | Description |
|---|---|---|
| `LOGGING` | `0` | Log each ACK, MATCH, ERR, and reconnect event to stderr |
| `DIAGNOSTICS` | `0` | Print client-side diagnostic info |
| `EXCHANGE_HOST` | `"127.0.0.1"` | Exchange address |
| `EXCHANGE_PORT` | `4000` | Exchange port |
| `CLIENT_CORE` | `4` | CPU core the client event loop is pinned to |
| `EXPECTED_THROUGHPUT` | `0` | Aggregate orders/s rate cap across all clients; `0` = unlimited |
| `MID_PRICE_VOL` | `0.002` | GBM volatility per step controlling mid-price drift and limit spread |
| `MID_PRICE_INITIAL` | `10000` | Starting mid-price in ticks ($100.00) |
| `MID_PRICE_UPDATE_N` | `16` | How many frames between GBM mid-price updates |
| `MEAN_QTY` | `100.0` | Log-normal order quantity mean |
| `QTY_VOL` | `0.8` | Log-normal order quantity volatility |
| `PIPELINE_DEPTH` | `32` | In-flight requests per connection |

---

## Benchmark suite (`bench/`)

The `bench/` directory holds a small suite of Python benchmarks that build,
launch, and drive the exchange/client pair for you. A shared harness
(`bench/benchlib.py`) auto-detects the backend — it runs the **native** release
binaries on Linux x86-64 (real latency) and otherwise falls back to **Docker**
(functional throughput; latency is emulated and clearly flagged). Any compile-time
knob a benchmark toggles (`EXPECTED_THROUGHPUT`, `DIAGNOSTICS`) is edited and then
restored, guaranteed even on Ctrl-C. Results are printed as tables and written to
`bench/results/` as CSV + JSON; raw per-run logs land in `bench/logs/`.

| Script | Measures |
|---|---|
| `saturation` | Peak sustained req/s and the plateau, sweeping client count (closed-loop) |
| `latency_curve` | End-to-end latency percentiles vs offered load (supersedes the old `bench.py`) |
| `stage_breakdown` | Per-stage latency histograms (`DIAGNOSTICS=1`) to locate the bottleneck |
| `regression` | Fixed-seed run checked against a stored baseline — a CI gate |

Run any of them as a module from the repo root:

```bash
python3 -m bench.saturation --clients 1,2,4,8,16,32 --duration 10
python3 -m bench.latency_curve --levels 250000,1000000,3500000 --clients 10
python3 -m bench.stage_breakdown --clients 16 --duration 20
python3 -m bench.regression --update      # record a baseline, then re-run to check
```

Common flags: `--runner {auto,native,docker}`, `--duration`, `--seed`, `--no-build`.
See `bench/README.md` for details. **Reliable latency numbers require native Linux
x86-64** — under Docker on Apple Silicon the pipeline runs but latency figures are
emulated (throughput remains meaningful).

---

## How it works

### Wire protocol

Every frame sent from client to exchange is 72 bytes:
- 9-byte ASCII header `"EXCHANGE\n"`
- 56-byte `InboundMessage` struct (containing timestamps, order fields, and message type)
- 1-byte newline + 6 bytes padding

Every frame sent from exchange to client is 32 bytes:
- ACK: `"EXCHANGE\nOK\n"` (12 B) + 4-byte ClientId + 8-byte OrderId + zero padding
- Fill notification: `"EXCHANGE\nMATCH\n"` (15 B) + 4-byte ClientId + 8-byte OrderId + zero padding
- Error: `"EXCHANGE\nERROR\n"` + ASCII error string + newline + zero padding

Fixed frame sizes allow both sides to parse byte streams without a length prefix or delimiter search: each side advances by exactly one frame size per message.

### Exchange threads

The exchange runs three threads, each pinned to a dedicated CPU core:

**Inbound thread** (`INBOUND_CORE`): accepts TCP connections via `accept()`, reads incoming bytes into per-connection read buffers using `epoll`, validates each frame, stamps an initial TSC value, and pushes `InboundMessage` structs onto the inbound SPSC ring. Validation errors are forwarded to the outbound thread via a separate error channel rather than being handled inline.

**Matching thread** (`MATCHING_CORE`): spins on the inbound ring, popping one message at a time and dispatching it to the order book. After processing it pushes an `OutboundMessage` onto the outbound ring. No synchronization is needed beyond the ring's acquire/release memory ordering.

**Outbound thread** (`OUTBOUND_CORE`): drains the outbound ring and routes each message to the correct client file descriptor using a `ClientId -> fd` map. It serializes responses into per-connection write buffers and drains those to the kernel via `send()`. Back-pressure is handled by arming `EPOLLOUT` on the rare occasion that `send()` returns `EAGAIN`.

### Order book

The book stores bids and asks as fixed-size arrays of price-level queues indexed by price tick (`bids_[price - MIN_PRICE]`). This makes best-bid/best-ask lookup O(1) in the common case. Each price-level queue is an intrusive doubly-linked list of `Order` objects stored by value in an `unordered_map<OrderId, Order>`, which keeps pointer stability across insertions.

Matching runs after every `addOrder` or `modifyOrder` call. It walks from the best bid price down and the best ask price up, filling contra-side orders until no crossing remains. MARKET orders bypass the book entirely and fill against the current best available price.

### Connection lifecycle

Disconnections are handled with a two-phase close protocol to avoid race conditions between the two server threads. When the inbound thread detects a hangup it sets `condemned_inbound_[fd]`. The outbound thread scans for this flag, cleans up its own per-fd state, then sets `condemned_outbound_[fd]`. The inbound thread scans for that flag and only then calls `close()` on the file descriptor. This ordering guarantees no thread accesses freed state.

### Latency measurement

Eight TSC timestamps are taken per request when `DIAGNOSTICS` is enabled:
- t0: before the `recv` syscall
- t1: after the `recv` syscall
- t2: after pushing onto the inbound ring
- t3: after the engine pops from the inbound ring
- t4: after the engine pushes onto the outbound ring
- t5: after the server pops from the outbound ring
- t6: before the `send` syscall
- t7: after the `send` syscall

At shutdown, `LatencyHandler` converts tick differences to nanoseconds using a calibrated ticks-per-nanosecond ratio and reports p50/p90/p99/p99.9/p99.99/max for each segment using HdrHistogram.

Without `DIAGNOSTICS`, only t0 and t7 are recorded (end-to-end latency only).

### Client load generator

`LoadGenerator` runs a single-threaded `epoll` event loop managing N non-blocking TCP connections simultaneously. Each connection maintains a pipeline of up to `PIPELINE_DEPTH` in-flight requests. When a response arrives the pipeline slot is freed and a new request is immediately queued.

Order types are generated by `OrderFactory`, a singleton that shares one RNG and one GBM mid-price state across all connections. The default probability mix is approximately 79% NEW, 12% CANCEL, and 9% MODIFY. CANCEL and MODIFY target order IDs returned by previous NEW acknowledgements. Market and limit orders are chosen with equal probability.

Rate limiting is controlled by `EXPECTED_THROUGHPUT`: when set to a non-zero value the client computes a per-connection inter-arrival time and holds each connection to one request per interval. When set to `0` the client runs as fast as possible.

---

## Tests

Test files exist under `exchange/engine/tests/` and `exchange/server/tests/` but are not actively maintained and may be out of date. They are guarded by `#define TESTING 1` in `exchange/config/config.hpp`.
