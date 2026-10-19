# Client-Exchange Simulation

## Project Status
In development. The exchange and the load-generating client run end to end on one machine:
the client's kernel TCP connections reach the exchange's DPDK port through a tap interface
(see [Run](#run)). [`TODO.md`](TODO.md) has what's next.

## Overview
A price-time-priority order-matching engine written in C++23, built around a low-latency
matching core plus a load-generating client.

- **Exchange** (`src/exchange`): a DPDK kernel-bypass gateway takes order entry as
  OUCH 5.0 over SoupBinTCP 3.00, validates it, and hands the matching engine compact
  requests over an SPSC ring; engine events come back over another ring and go out as
  OUCH. TCP is terminated in user space by the DPDK wrapper library's `tcp_stack`.
  SoupBinTCP sessions are keyed on the login username, and a client that reconnects is
  replayed the sequenced messages it missed. Order handling follows Nasdaq's rules: incoming orders trade before resting,
  Replace loses time priority, Cancel/Replace quantities include shares already executed,
  and IOC/market remainders are canceled. Every book change is also published as ITCH 5.0
  onto a market-data ring.
- **Client** (`src/client`): a single-threaded epoll load generator. Each connection logs
  in over SoupBinTCP on kernel TCP as its own session and streams OUCH
  Enter/Cancel/Replace orders, keeping `PIPELINE_DEPTH` Enters in flight; after a
  reconnect it resumes from the next sequence number it hasn't seen.

Linux only because the gateway needs DPDK's EAL, the client uses `epoll`, and both pin threads
with `pthread_setaffinity_np`.

## Limitations
- **Lossless links only.** The DPDK library's TCP has no retransmission or out-of-order
  reassembly, which is fine over a tap interface on one host but not over a real network.
- **Replay covers short outages.** Each session keeps its last `SOUP_REPLAY_BYTES` of
  Sequenced Data in memory (16 MiB: ~110k orders' worth, a couple of seconds at full
  load). A re-login asking for anything older is rejected; there's no on-disk journal.
- **No cancel-on-disconnect.** A disconnected session's orders keep resting and trading.
- **No authentication.** Any username/password is accepted.
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
- To *run* the exchange: `CAP_NET_ADMIN`, to create its tap interface: root, or a
  throwaway user + network namespace (see [Run](#run)). No hugepages or NIC are needed on
  the tap path. (`EXCHANGE_NETWORK_TAP 0` runs it on a DPDK-bound NIC instead, which needs
  hugepages and a NIC bound to a DPDK driver as port 0.) The tests need none of this
  except `server-tap-tests`, which is skipped without the capability.
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

## Run

The exchange creates a tap interface, `dpdktap0`: the kernel's side is `10.77.0.1/24`,
and the exchange answers as `10.77.0.2:4000`, which is where the client connects. Both
run on the same machine, in the same network namespace.

As root:
```bash
sudo ./build/exchange/exchange-release &
./build/client/client-release 8        # 8 sessions; Ctrl-C prints stats
sudo kill -INT %1                      # the exchange prints its latency histograms
```

Without root, in a throwaway user + network namespace (`SCHED_FIFO` isn't permitted
there, so threads fall back to the normal scheduler):
```bash
unshare -rnm sh -c 'mount -t tmpfs tmpfs /var/run && ip link set lo up &&
    ./build/exchange/exchange-release & sleep 1.5;
    timeout -s INT 10 ./build/client/client-release 8; kill -INT $!; wait'
```

## Benchmark suite (`bench/`)

The `bench/` directory holds a suite of Python benchmarks that build, launch, and drive
things for you, split into two families:

| Script | Measures | Currently Usable |
|---|---|------------------|
| `saturation` | Peak sustained req/s, sweeping client count (closed-loop) | Native only      |
| `latency_curve` | End-to-end latency percentiles vs offered load | Native only      |
| `stage_breakdown` | Per-stage latency histograms (`DIAGNOSTICS=1`) | Native only      |
| `regression` | Fixed-seed run checked against a stored baseline | Native only      |
| `ring_buffer` | `RingBuffer<T>` push/pop latency + throughput, in isolation | Yes              |
| `orderbook_perf` | Cache/branch-miss rate for `Orderbook::process()`, in isolation | Yes              |
| `perf_on_executable` | Generic `perf stat` wrapper for any executable | Yes              |

The exchange/client scripts run the pair over the tap path: as root directly, otherwise
inside `unshare -rnm`. The Docker backend can't build the exchange yet (its image has no
DPDK).

```bash
python3 -m bench.scripts.saturation --clients 1,2,4,8 --duration 10
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
| `DIAGNOSTICS` | `1` | Print per-stage latency histograms at shutdown (the end-to-end histogram is always printed). Every thread's tid and name is printed at startup either way |
| `TESTING` | `0` | Expose `Engine`/`Orderbook` inspection accessors and `Server` internals to tests, and leave threads unpinned (overridden to `1` for `engine-tests`, `server-tests`, `server-tap-tests` and `orderbook-bench`) |
| `PREFETCH` | `1` | Software prefetch hints on the matching hot path; set `0` to compile them out |
| `MIN_PRICE` | `1` | Lowest price the book holds, in cents (its tick); OUCH prices outside the band, or sub-penny, are rejected |
| `MAX_PRICE` | `100000` | Highest price the book holds; $1000.00 = 100000 cents |
| `BOOK_SYMBOL` | `"SIM"` | Symbol the single book publishes ITCH under |
| `BOOK_STOCK_LOCATE` | `1` | ITCH stock locate for that book |
| `MATCHING_CORE` | `1` | CPU core the matching thread is pinned to |
| `INBOUND_CORE` | `2` | CPU core the inbound gateway thread is pinned to |
| `OUTBOUND_CORE` | `3` | CPU core the outbound gateway thread is pinned to |
| `EXCHANGE_NETWORK_TAP` | `1` | `1`: run on a `net_tap` port the kernel can reach; `0`: on a DPDK-bound NIC |
| `EXCHANGE_TAP_IFACE` | `"dpdktap0"` | Name of the kernel interface the tap port creates |
| `EXCHANGE_KERNEL_IPV4` | `10, 77, 0, 1` | The kernel side's address (`/24`) |
| `EXCHANGE_IPV4` | `10, 77, 0, 2` | The exchange's address |
| `EXCHANGE_PORT` | `4000` | Port the exchange listens on |
| `SOUP_SESSION` | `"SIM"` | SoupBinTCP session clients log into (any username/password is accepted) |
| `MAX_CLIENTS` | `64` | Usernames (sessions) accepted in a day; logins for new ones beyond this are rejected |
| `SOUP_REPLAY_BYTES` | `16 MiB` | Sequenced Data each session keeps for re-login replay (power of two), allocated on its first login. Also its send buffer, so a connected client this far behind is disconnected |
| `SOUP_IDLE_TIMEOUT_MS` | `15000` | A logged-in client that sends nothing, heartbeats included, for this long is disconnected |
| `SOUP_LOGIN_TIMEOUT_MS` | `30000` | A connection that hasn't logged in by then is dropped |
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
| `EXCHANGE_HOST` | `"10.77.0.2"` | Exchange address (its tap address; `client-tests` overrides it to loopback) |
| `EXCHANGE_PORT` | `4000` | Exchange port |
| `CLIENT_CORE` | `4` | CPU core the client event loop is pinned to |
| `SOUP_USERNAME_PREFIX` / `SOUP_PASSWORD` | `"LG"` / `"LOADGEN"` | SoupBinTCP login: connection *n* logs in as the prefix plus *n*, e.g. `LG0001` |
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
| `gateway-tests` | `OrderEntry` (validation, OUCH → engine requests, OrderId assignment), `OuchResponder` (engine events → OUCH) and `SessionLog` (the replay ring) | — |
| `engine-tests` | Matching through the events and ITCH it emits: aggressor-first matching, time priority, IOC/market orders, Cancel/Replace rules | — |
| `packet-factory-tests` | Ethernet/IPv4/TCP/UDP packet building and parsing | software-only EAL |
| `server-tests` | The real `Server` over a `net_ring0` vdev, with the test playing the engine and the clients: scripted TCP connections (`tests/tcp_peer.hpp`) speaking SoupBinTCP. Login, reassembly, timeouts, re-login replay, slow consumers. Built with a 4 KiB replay log and 2 s timeouts | software-only EAL |
| `server-tap-tests` | The `Server` on a `net_tap` port plus the real `Engine`, against kernel sockets: matching, replay after reconnect, thousands of pipelined orders over real Linux TCP. Label `privileged`; skipped (exit 77) without the capability. Run it with `sudo` or `unshare -rnm` (see `tests/tap_e2e_test.cpp`) | `CAP_NET_ADMIN` |
| `client-tests` | The real `LoadGenerator` against an in-test SoupBinTCP/OUCH exchange on loopback, including reconnects that resume the session | `EXCHANGE_PORT` free |
