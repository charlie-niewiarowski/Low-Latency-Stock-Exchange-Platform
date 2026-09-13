# Client-Exchange Simulation

A price-time-priority order-matching engine written in C++23, built around a low-latency
matching core (order pool arena, intrusive price-level queues, SPSC ring buffers). The
project is in the middle of migrating its network layer from a TCP/epoll gateway to a
DPDK kernel-bypass gateway speaking real exchange wire protocols (OUCH for order entry,
ITCH for market data). Linux only — the matching core and the DPDK gateway both depend on
Linux-specific APIs (`epoll`, `pthread_setaffinity_np`, hugepages, DPDK's EAL).

---

## Current status

This is mid-rewrite and **the exchange and client binaries do not currently build**:

- `exchange-release` / `exchange-debug` — `Exchange`'s constructor (`src/exchange/app/exchange.cpp`)
  still calls the old 3-argument `Server(InboundRing&, OutboundRing&, std::atomic<bool>&)`
  constructor, but `Server` (`src/exchange/server/include/server.hpp`) has already been
  rewritten to the new DPDK/OUCH shape: `Server(shared_ptr<RingBuffer<OUCH>>, shared_ptr<RingBuffer<OUCH>>)`.
  These two don't match.
- `client-release` / `client-debug` — `ClientState`/`LoadGenerator`
  (`src/client/include/client_state.hpp`, `src/client/src/load_generator.cpp`) still reference
  `INBOUND_BSIZE`/`OUTBOUND_BSIZE`, wire-frame-size constants that no longer exist now that
  `protocol.hpp` has been rewritten around OUCH/ITCH message structs instead of the old
  fixed "EXCHANGE\n" + `InboundMessage` frame.

What **does** build and run today (native Linux x86-64, verified against this branch):

- `engine-tests` — links cleanly against the matching engine (`Engine`/`Orderbook`), though
  `tests/engine_test.cpp` currently has no test cases in it (0 tests run).
- `server-tests` — links cleanly against the new DPDK-based `Server`; its one test
  (`ServerTest.test_receives_packets`) brings up a real DPDK EAL instance, so it needs
  working hugepages and permissions (`/dev/hugepages`) to actually run, not just build.
- `orderbook-bench` / `ring-buffer-bench` (`bench/executables/`) — isolated microbenchmarks
  against `Orderbook` and `RingBuffer<T>` directly; neither touches `Server`, `Engine`,
  or the client, so they're unaffected by the gateway rewrite.

For a last known-working, fully end-to-end TCP/epoll version of the simulation (exchange
+ client, old repo layout without the `src/` prefix), see the `main` branch.

---

## Overview

The matching engine processes three order operations — NEW, CANCEL, MODIFY — over LIMIT
and MARKET order types, with price-time priority. On shutdown the exchange prints HDR
histogram latency data covering the full round-trip as well as each internal pipeline
segment (recv/parse, ring transit, engine processing, send, etc. — see
[Latency measurement](#latency-measurement)).

The intended end state (in progress) is a DPDK-based gateway: inbound OUCH order-entry
packets are received off a DPDK rx queue, deserialized, validated, and handed to the
matching engine; engine output is serialized back out as OUCH responses over a DPDK tx
queue, and a separate `market_feed` component republishes fills as an ITCH-style UDP
multicast feed. None of that gateway wiring exists yet — see
[Current status](#current-status) and [In-progress DPDK/OUCH/ITCH gateway](#in-progress-dpdkouchitch-gateway) below for exactly what's there today.

---

## Repository layout

```
ClientExchangeSim/
  src/
    exchange/
      app/          Exchange entry point and top-level Exchange class
      engine/       Matching engine (order pool, price-level queues, order book)
        include/    engine.hpp, orderbook.hpp, order.hpp, order_pool.hpp,
                     price_level_queue.hpp, order_request.hpp, trade.hpp
      server/       In-progress DPDK-based gateway (see status above)
        include/    server.hpp, latency.hpp (HDR histogram + TSC latency handler)
      market_feed/  Stub ITCH multicast publisher — not wired into the build
      config/       src/exchange/config/config.hpp -- all compile-time knobs
    client/
      app/          Client entry point
      src/          LoadGenerator (epoll event loop) -- currently broken, see status above
      include/      ClientState, LoadGenerator, OrderFactory
      config/       src/client/config/config.hpp -- all compile-time knobs
    infra/          Wire types + hot-path primitives shared by both programs
                    (dependency-free by design, so it can be lifted into a
                    separate repo for future trading systems without touching
                    exchange/ or client/)
      protocol.hpp    TCPHeader/UDPHeader + real OUCH 5.0 / ITCH 5.0 message structs
      communication_types.hpp  InboundMessage/OutboundMessage (the older
                      internal engine<->gateway message shape; still what
                      Engine/Orderbook consume today)
      order_types.hpp  Primitive type aliases (Price, Quantity, OrderId, etc.)
      packet_factory.hpp  Builds/parses DPDK packets carrying OUCH or ITCH payloads
      buffer.hpp      Header-only fixed-capacity byte buffer (TCP read/write path)
      ring_buffer.hpp Lock-free SPSC ring buffer
      validation.hpp  Stub -- OUCH/ITCH validation not yet implemented
      perf.hpp        Software prefetch hints + thread/core pinning helpers
  tests/          engine-tests, server-tests (GoogleTest; see status above)
  bench/          Python benchmark suite + isolated C++ microbenchmarks
  CMakeLists.txt  Root build file
```

---

## Prerequisites (Linux only)

- Linux, x86-64. The matching engine uses `__rdtsc`/`-march=native`; the server links
  DPDK, which needs a Linux kernel (hugepages, UIO/VFIO) regardless of what it ends up
  driving traffic through.
- GCC or Clang with C++23 support.
- CMake 3.25 or newer.
- **DPDK development headers, discoverable via `pkg-config --modversion libdpdk`.** On
  Debian/Ubuntu: `sudo apt install dpdk dpdk-dev libdpdk-dev`. This is a hard build
  dependency today: `exchange-release`/`exchange-debug`/`server-tests` all link
  `dpdk_lib`, a small wrapper library CMake fetches from
  `github.com/charlie-niewiarowski/dpdk-wrapper-library`, and that library's own
  `CMakeLists.txt` does `pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)`.
- Working hugepages if you intend to actually *run* anything that brings up the DPDK EAL
  (`server-tests`, and eventually the exchange binary) — not required just to build.
- Python 3.9 or newer (only needed for the `bench/` suite).
- An internet connection on first configure — CMake's `FetchContent` pulls down
  HdrHistogram_c, the DPDK wrapper library, and (for `tests/`) GoogleTest.

---

## Build

```bash
git clone <repo-url>
cd ClientExchangeSim

cmake -S . -B build
```

Building everything (`cmake --build build -j$(nproc)`) will currently fail partway
through on the `exchange-*` and `client-*` targets (see
[Current status](#current-status)). Build the targets that actually work individually:

```bash
cmake --build build --target engine-tests -j$(nproc)
cmake --build build --target server-tests -j$(nproc)     # needs libdpdk to build
cmake --build build --target orderbook-bench ring-buffer-bench -j$(nproc)
```

```bash
./build/tests/engine-tests
./build/tests/server-tests      # needs hugepages/permissions to actually run
./build/bench/executables/orderbook-bench
./build/bench/executables/ring-buffer-bench
```

---

## Benchmark suite (`bench/`)

The `bench/` directory holds a suite of Python benchmarks that build, launch, and drive
things for you, split into two families:

| Script | Measures | Depends on the broken exchange/client? |
|---|---|---|
| `saturation` | Peak sustained req/s, sweeping client count (closed-loop) | Yes |
| `latency_curve` | End-to-end latency percentiles vs offered load | Yes |
| `stage_breakdown` | Per-stage latency histograms (`DIAGNOSTICS=1`) | Yes |
| `regression` | Fixed-seed run checked against a stored baseline | Yes |
| `ring_buffer` | `RingBuffer<T>` push/pop latency + throughput, in isolation | No |
| `orderbook_perf` | Cache/branch-miss rate for `Orderbook::process()`, in isolation | No |
| `perf_on_executable` | Generic `perf stat` wrapper for any executable | No |

The first four drive `build/exchange/exchange-release` and `build/client/client-release`
directly and will not run until those targets build again. `ring_buffer` and
`orderbook_perf` build and run their own standalone executables
(`bench/executables/ring_buffer_bench.cpp`, `orderbook_bench.cpp`) and work today:

```bash
python3 -m bench.scripts.ring_buffer op
python3 -m bench.scripts.ring_buffer spsc --capacity 524288 --iters 2000000
python3 -m bench.scripts.orderbook_perf --ops 10000000 --reps 20 --core 6
```

See `bench/README.md` for the full script reference, flags, and output layout.

---

## Configuration

All compile-time configuration lives in header files. A rebuild is required after changes.

### Exchange: `src/exchange/config/config.hpp`

| Macro | Default | Description |
|---|---|---|
| `LOGGING` | `0` | Print matched trades to stdout |
| `DIAGNOSTICS` | `1` | Collect per-segment TSC timestamps; enables detailed latency histograms |
| `TESTING` | `0` | Expose `Engine`/`Orderbook` inspection accessors for unit tests (overridden to `1` by `tests/CMakeLists.txt` for `engine-tests`) |
| `PREFETCH` | `1` | Software prefetch hints on the matching hot path; set `0` to compile them out |
| `MIN_PRICE` | `1` | Minimum valid limit price (integer ticks) |
| `MAX_PRICE` | `100000` | Maximum valid limit price; $1000.00 = 100000 ticks |
| `MATCHING_CORE` | `1` | CPU core the matching thread is pinned to |
| `INBOUND_CORE` | `2` | CPU core the inbound gateway thread is pinned to |
| `OUTBOUND_CORE` | `3` | CPU core the outbound gateway thread is pinned to |
| `PORT` | `"4000"` | TCP port (unused by the current DPDK `Server`; left over from the TCP gateway) |
| `MAX_CLIENTS` | `64` | Maximum simultaneous connections |
| `PIPELINE_DEPTH` | `32` | Per-connection outbound staging ring capacity |
| `COMMUNICATION_RING_COUNT` | `524288` | Inbound and outbound SPSC ring sizes |
| `PREALLOCATION_COUNT` | `1000000` | `OrderPool` arena capacity / `OrderMap` reserve |
| `RINGBUF_SIZE` | `512` | Per-client `RingBuffer<OUCH>` capacity used by `server-tests` |
| `LATENCY_SAMPLE_DROP` | `5` | Cold-start samples discarded before recording |
| `LATENCY_SAMPLE_COUNT` | `2000 + LATENCY_SAMPLE_DROP` | How many samples to collect before stopping |

### Client: `src/client/config/config.hpp`

| Macro | Default | Description |
|---|---|---|
| `LOGGING` | `0` | Log each ACK, MATCH, ERR, and reconnect event to stderr |
| `DIAGNOSTICS` | `1` | Print client-side diagnostic info |
| `MIN_PRICE` / `MAX_PRICE` | `1` / `100000` | Must match the exchange's config -- the exchange indexes its price ladder directly by `(price - MIN_PRICE)` |
| `EXCHANGE_HOST` | `"127.0.0.1"` | Exchange address |
| `EXCHANGE_PORT` | `4000` | Exchange port |
| `CLIENT_CORE` | `4` | CPU core the client event loop is pinned to |
| `EXPECTED_THROUGHPUT` | `0` | Aggregate orders/s rate cap across all clients; `0` = unlimited |
| `MID_PRICE_VOL` | `0.002` | GBM volatility per step controlling mid-price drift and limit spread |
| `MID_PRICE_INITIAL` | `10000` | Starting mid-price in ticks ($100.00) |
| `MID_PRICE_UPDATE_N` | `16` | How many frames between GBM mid-price updates |
| `MEAN_QTY` | `100.0` | Log-normal order quantity mean |
| `QTY_VOL` | `0.8` | Log-normal order quantity volatility |
| `MAX_ACTIVE_ORDERS` | `32` | Per-connection ring of order ids kept as CANCEL/MODIFY targets |
| `PIPELINE_DEPTH` | `32` | In-flight requests per connection |
| `CLIENT_EPOLL_BATCH` | `512` | Max epoll events drained per loop iteration |
| `RECONNECT_DELAY_MS` | `1000` | Reconnect backoff ceiling (linear) |

---

## How it works

### The matching engine (working, tested via benchmarks today)

`Engine` is a thin driver: it owns the matching thread, assigns `OrderId`s to NEW orders,
translates each `InboundMessage` into an `OrderRequest`, and hands it to `Orderbook::process()`.

`Orderbook` owns all book state and mutation logic:

- **`OrderPool`** is the single owner of every `Order` object -- a fixed-capacity arena
  (`std::vector<Order>`, sized by `PREALLOCATION_COUNT`) with a free-list stack for O(1)
  allocate/deallocate. Nothing else in the book allocates or frees an `Order`; the
  `OrderMap` and `PriceLevelQueue`s only ever hold non-owning pointers into this pool.
- **`PriceLevelQueue`** is an intrusive doubly-linked list of `Order` nodes: push at the
  back, pop from the front (time priority), and O(1) removal of an arbitrary order
  (cancellation) via its own `prev_`/`next_` pointers.
- Bids and asks are each a fixed-size `std::array<PriceLevelQueue, MAX_PRICE - MIN_PRICE + 1>`
  indexed directly by `price - MIN_PRICE`, making best-bid/best-ask lookup O(1) in the
  common case.
- `process()` runs matching after every add/modify: it walks from the best bid down and
  the best ask up, filling contra-side orders until no crossing remains. MARKET orders
  bypass the book and fill against the current best available price. Every request gets
  exactly one direct ack/error via `ProcessResult`, regardless of how many MATCH fills it
  also produced (those get pushed to the outbound ring separately, one per affected
  counterparty).
- A one-ahead software-prefetch pipeline (`Engine::handleMatching`) pops the *next*
  inbound message and prefetches the price-level/`orders_` slot it will touch while the
  *current* request is still being processed by `Orderbook::process()`, hiding that cache
  miss behind real work (`PREFETCH` config macro; see `src/infra/perf.hpp`).

### Latency measurement

`LatencyHandler` (`src/exchange/server/include/latency.hpp`) records up to eight TSC
timestamps per request (t0..t7, spanning read → inbound ring → engine → outbound ring →
write) and, at shutdown, converts tick deltas to nanoseconds using a calibrated
ticks-per-nanosecond ratio, reporting p50/p90/p99/p99.9/p99.99/max per segment via
HdrHistogram. This piece is gateway-agnostic and will keep working whichever transport
ends up feeding it.

### In-progress DPDK/OUCH/ITCH gateway

`src/infra/protocol.hpp` defines real exchange wire formats: **OUCH 5.0** (order entry,
carried over a `TCPHeader`) and **ITCH 5.0** (market data, one-way multicast over a
`UDPHeader`) -- field names/sizes/order follow Nasdaq's public specs, with a common
`message_type` at byte 0 of every variant so a receiver can dispatch before knowing which
member of the `OUCH`/`ITCH` union is live. `packet_factory<transport, payload>`
(`src/infra/packet_factory.hpp`) builds and parses whole DPDK packets (Ethernet + IPv4 +
TCP/UDP + OUCH/ITCH payload) on top of a separately-fetched DPDK wrapper library
(`dpdk::runtime`, `dpdk::port`, `dpdk::packet_pool`, ...).

Today, `Server` (`src/exchange/server/src/server.cpp`) brings up a `dpdk::runtime` and one
port, and spawns inbound/outbound threads -- but `inbound_()` only logs each received
packet's length, `outbound_()` is an empty loop, and neither touches `packet_factory`,
OUCH parsing, or the matching engine yet. `market_feed` (an ITCH multicast publisher
meant to consume fills off a queue the engine publishes to) is a header/stub with no
implementation. `src/infra/validation.hpp` is a placeholder for OUCH/ITCH validation that
hasn't been written.

The old TCP/epoll wire format is still what `OrderFactory` (`src/client/include/order_factory.hpp`)
builds and what `Engine`/`Orderbook` consume (`InboundMessage`/`OutboundMessage` in
`src/infra/communication_types.hpp`): a 9-byte `"EXCHANGE\n"` header, a 56-byte
`InboundMessage`, and a trailing newline + padding. That framing has no home in the new
gateway yet -- `Server` doesn't speak it, and the client-side buffering that used to wrap
it around a TCP socket references buffer-size constants (`INBOUND_BSIZE`/`OUTBOUND_BSIZE`)
that were removed when `protocol.hpp` was rewritten around OUCH/ITCH (see
[Current status](#current-status)).

### Client load generator (currently doesn't build -- see status)

`LoadGenerator` is a single-threaded `epoll` event loop meant to manage N pipelined,
non-blocking TCP connections (`PIPELINE_DEPTH` in-flight requests each), generating orders
via `OrderFactory` -- a singleton sharing one RNG and one GBM mid-price model across all
connections (roughly 74% NEW / 14% CANCEL / 8% MODIFY, market/limit chosen with equal
probability). `EXPECTED_THROUGHPUT` (0 = unlimited) controls an open-loop per-connection
inter-arrival rate cap. This is the piece most directly broken by the protocol rewrite;
see [Current status](#current-status).

---

## Tests

`tests/` builds two GoogleTest targets:

- **`engine-tests`** -- builds `engine.cpp`/`orderbook.cpp` with `TESTING=1` (exposing
  `Orderbook`'s inspection accessors) and links only `infra`. `tests/engine_test.cpp` is
  currently empty (0 test cases); this target exists as ready-to-use scaffolding for
  whoever writes the next matching-engine test.
- **`server-tests`** -- builds against the DPDK-based `Server` and links `dpdk_lib`
  alongside `infra`. Its one test (`ServerTest.test_receives_packets`) opens a plain TCP
  socket, which nothing in `Server` currently listens on, and constructing `Server` itself
  brings up a real DPDK EAL instance -- so it needs working hugepages/permissions
  (`/dev/hugepages`) just to get past the fixture's constructor, and will need updating
  once `Server` actually parses OUCH traffic.
