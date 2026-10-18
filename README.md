# Client-Exchange Simulation

## Project Status
In development. The exchange and the client both build and are tested on their own, but
they can't connect to each other yet: the exchange's DPDK gateway has no TCP stack (see
[Limitations](#limitations)).

## Overview
A price-time-priority order-matching engine written in C++23, built around a low-latency
matching core plus a load-generating client.

- **Exchange** (`src/exchange`): a DPDK kernel-bypass gateway takes order entry as
  OUCH 5.0 over SoupBinTCP 3.00, validates it, and hands the matching engine compact
  requests over an SPSC ring; engine events come back over another ring and go out as
  OUCH. Order handling follows Nasdaq's rules: incoming orders trade before resting,
  Replace loses time priority, Cancel/Replace quantities include shares already executed,
  and IOC/market remainders are canceled. Every book change is also published as ITCH 5.0
  onto a market-data ring.
- **Client** (`src/client`): a single-threaded epoll load generator. Each connection logs
  in over SoupBinTCP on kernel TCP and streams OUCH Enter/Cancel/Replace orders, keeping
  `PIPELINE_DEPTH` Enters in flight.

Linux only because the gateway needs DPDK's EAL, the client uses `epoll`, and both pin threads
with `pthread_setaffinity_np`.

## Limitations
- **The client can't reach the exchange.** The client speaks kernel TCP; the exchange
  reads raw packets off a DPDK port and doesn't terminate TCP. Connecting them needs TCP
  handling in the DPDK wrapper library plus a path the kernel can reach (e.g. a tap device).
- **The gateway's SoupBinTCP layer is a stub** until then: each TCP segment must hold whole
  SoupBinTCP packets, a session is the sender's (ip, port), and there's no retransmission,
  re-login replay, or client-heartbeat timeout. Login/logout, Sequenced/Unsequenced Data
  and server heartbeats work.
- **Nothing consumes market data yet.** The engine publishes ITCH to a ring that nothing
  reads; it drops (and counts) messages once that ring is full.
- **One book.** Every order trades in the `BOOK_SYMBOL` book whatever symbol it names.
- **Order types:** limit and market orders; Day and IOC only; no short sales, pegs,
  reserves, minimum quantities or other OUCH optional behaviour (sent at anything but its
  default, those are rejected).

## Prerequisites (Linux only)
- Linux, x86-64 with an invariant TSC (`constant_tsc` and `nonstop_tsc` in
  `/proc/cpuinfo`): latency stamps and OUCH/ITCH timestamps both come from `rdtsc`.
- GCC or Clang with C++23 support (tested with GCC 12).
- CMake 3.25 or newer.
- **DPDK development headers, discoverable via `pkg-config --modversion libdpdk`** (tested
  with 22.11). On Debian/Ubuntu: `sudo apt install dpdk dpdk-dev libdpdk-dev`. The exchange
  and the DPDK-backed tests link `dpdk_lib`, a small wrapper library CMake fetches from
  `github.com/charlie-niewiarowski/dpdk-wrapper-library`, whose own `CMakeLists.txt`
  requires `libdpdk` through `pkg_check_modules`.
- To *run* the exchange binary: hugepages it can open (root, or permissions on
  `/dev/hugepages`) and a NIC bound to a DPDK driver as port 0. Without them it aborts in
  `rte_eal_init`. The tests don't need either; they bring up a software-only EAL.
- Python 3.9 or newer (only for the `bench/` suite), and `perf` for `orderbook_perf`.
- An internet connection on first configure — CMake's `FetchContent` pulls down
  HdrHistogram_c, the DPDK wrapper library, Abseil, and (for `tests/`) GoogleTest.

## Build
```bash
git clone <repo-url>
cd ClientExchangeSim

cmake -S . -B build
cmake --build build -j$(nproc)
```

This builds everything: `build/exchange/exchange-{release,debug}`,
`build/client/client-{release,debug}`, the test suites under `build/tests/`, and the
benchmark executables under `build/bench/executables/`.

```bash
(cd build && ctest -j$(nproc))              # every test suite -- see Tests
./build/bench/executables/orderbook-bench   # matching-core workload generator
./build/bench/executables/ring-buffer-bench
```

`exchange-debug` builds with AddressSanitizer and UBSan, minus UBSan's `null`,
`nonnull-attribute` and `returns-nonnull-attribute` checks, which make GCC 12 reject
Abseil's hash map.

## Benchmark suite (`bench/`)

The `bench/` directory holds a suite of Python benchmarks that build, launch, and drive
things for you, split into two families:

| Script | Measures | Currently Usable |
|---|---|------------------|
| `saturation` | Peak sustained req/s, sweeping client count (closed-loop) | No               |
| `latency_curve` | End-to-end latency percentiles vs offered load | No               |
| `stage_breakdown` | Per-stage latency histograms (`DIAGNOSTICS=1`) | No               |
| `regression` | Fixed-seed run checked against a stored baseline | No               |
| `ring_buffer` | `RingBuffer<T>` push/pop latency + throughput, in isolation | Yes              |
| `orderbook_perf` | Cache/branch-miss rate for `Orderbook::process()`, in isolation | Yes              |
| `perf_on_executable` | Generic `perf stat` wrapper for any executable | Yes              |

```bash
python3 -m bench.scripts.ring_buffer op
python3 -m bench.scripts.ring_buffer spsc --capacity 524288 --iters 2000000
python3 -m bench.scripts.orderbook_perf --ops 10000000 --reps 20 --core 6
```

See `bench/README.md` for the full script reference, flags, and output layout.

## Configuration

All compile-time configuration lives in header files. A rebuild is required after changes.

### Exchange: `src/exchange/config/config.hpp`

| Macro | Default | Description |
|---|---|---|
| `DIAGNOSTICS` | `1` | Print thread ids at startup, and per-stage latency histograms at shutdown (the end-to-end histogram is always printed) |
| `TESTING` | `0` | Expose `Engine`/`Orderbook` inspection accessors and the server's test queues (overridden to `1` for `engine-tests`, `server-tests` and `orderbook-bench`) |
| `PREFETCH` | `1` | Software prefetch hints on the matching hot path; set `0` to compile them out |
| `MIN_PRICE` | `1` | Lowest price the book holds, in cents (its tick); OUCH prices outside the band, or sub-penny, are rejected |
| `MAX_PRICE` | `100000` | Highest price the book holds; $1000.00 = 100000 cents |
| `BOOK_SYMBOL` | `"SIM"` | Symbol the single book publishes ITCH under |
| `BOOK_STOCK_LOCATE` | `1` | ITCH stock locate for that book |
| `MATCHING_CORE` | `1` | CPU core the matching thread is pinned to |
| `INBOUND_CORE` | `2` | CPU core the inbound gateway thread is pinned to |
| `OUTBOUND_CORE` | `3` | CPU core the outbound gateway thread is pinned to |
| `SOUP_SESSION` | `"SIM"` | SoupBinTCP session clients log into (any username/password is accepted) |
| `MAX_CLIENTS` | `64` | Maximum sessions; new ones beyond this are dropped |
| `COMMUNICATION_RING_COUNT` | `524288` | Size of each SPSC ring: engine in/out, gateway notes, market data |
| `PREALLOCATION_COUNT` | `1000000` | `OrderPool` arena capacity / `OrderMap` reserve |
| `RINGBUF_SIZE` | `512` | Engine ring capacity used by `server-tests` |
| `LATENCY_SAMPLE_DROP` | `5` | Answered requests skipped as warm-up before latency histograms start recording |

### Client: `src/client/config/config.hpp`

| Macro | Default | Description |
|---|---|---|
| `LOGGING` | `0` | Log connects, logins, reconnects, Accepted/Executed/Rejected messages and protocol warnings to stderr |
| `DIAGNOSTICS` | `1` | Print the thread id in the startup banner |
| `MIN_PRICE` / `MAX_PRICE` | `1` / `100000` | Price band, in cents, the client generates within; must match the exchange's, which rejects anything outside it |
| `CLIENT_SYMBOL` | `"SIM"` | Symbol every order is entered under |
| `EXCHANGE_HOST` | `"127.0.0.1"` | Exchange address |
| `EXCHANGE_PORT` | `4000` | Exchange port |
| `CLIENT_CORE` | `4` | CPU core the client event loop is pinned to |
| `SOUP_USERNAME` / `SOUP_PASSWORD` | `"LOADGN"` / `"LOADGEN"` | SoupBinTCP login credentials |
| `EXPECTED_THROUGHPUT` | `0` | Aggregate orders/s rate cap across all clients; `0` = unlimited |
| `MID_PRICE_VOL` | `0.002` | GBM volatility per step controlling mid-price drift and limit spread |
| `MID_PRICE_INITIAL` | `10000` | Starting mid-price in cents ($100.00) |
| `MID_PRICE_UPDATE_N` | `16` | How many messages between GBM mid-price updates (power of two) |
| `MEAN_QTY` | `100.0` | Log-normal order quantity mean |
| `QTY_VOL` | `0.8` | Log-normal order quantity volatility |
| `MAX_ACTIVE_ORDERS` | `32` | Per-connection ring of live orders' UserRefNums kept as Cancel/Replace targets |
| `PIPELINE_DEPTH` | `32` | Enter orders in flight per connection; Cancels/Replaces don't take a slot, since they can legitimately go unanswered |
| `MAX_BATCH_MESSAGES` | `2 * PIPELINE_DEPTH` | Cap on messages per write batch |
| `CLIENT_EPOLL_BATCH` | `512` | Max epoll events drained per loop iteration |
| `RECONNECT_DELAY_MS` | `1000` | Reconnect backoff ceiling (linear, reset by a successful login) |

## Tests

`ctest` runs every suite. Tests that bring up a DPDK EAL, and the client tests (which all
bind the same port), are serialized with resource locks, so `ctest -j` is safe.

| Target | Covers | Needs |
|---|---|---|
| `proto-tests` | OUCH, ITCH and SoupBinTCP codecs, byte for byte against the specs' offset tables | — |
| `clock-tests` | `TscClock`: TSC-derived nanoseconds since midnight | invariant TSC |
| `gateway-tests` | `OrderEntry` (validation, OUCH → engine requests, OrderId assignment) and `OuchResponder` (engine events → OUCH) | — |
| `engine-tests` | Matching through the events and ITCH it emits: aggressor-first matching, time priority, IOC/market orders, Cancel/Replace rules | — |
| `packet-factory-tests` | Ethernet/IPv4/TCP/UDP packet building and parsing | software-only EAL |
| `server-tests` | The real `Server` end to end over a `net_ring0` vdev, with the test playing both the SoupBinTCP client and the engine | software-only EAL |
| `client-tests` | The real `LoadGenerator` against an in-test SoupBinTCP/OUCH exchange on loopback | `EXCHANGE_PORT` free |
