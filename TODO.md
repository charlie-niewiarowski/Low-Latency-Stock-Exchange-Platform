# TODO

Ranked by priority, highest first. Time-frames are rough estimates of focused work,
including tests and docs.

| # | Item | Time-frame |
|---|---|---|
| 1 | [Market-data dissemination](#1-market-data-dissemination) | 1–2 weeks |
| 2 | [Cancel on disconnect](#2-cancel-on-disconnect) | 2–3 days |
| 3 | [Outbound-thread performance pass](#3-outbound-thread-performance-pass) | 3–5 days |
| 4 | [DPDK in the Docker image](#4-dpdk-in-the-docker-image) | 0.5–1 day |
| 5 | [Multiple symbols](#5-multiple-symbols) | ~1 week |
| 6 | [Durable, full-day replay journal](#6-durable-full-day-replay-journal) | 3–5 days |
| 7 | [TCP retransmission and reassembly in the DPDK library](#7-tcp-retransmission-and-reassembly-in-the-dpdk-library) | 1–2 weeks |
| 8 | [SoupBinTCP odds and ends](#8-soupbintcp-odds-and-ends) | ≤ 1 day |

## 1. Market-data dissemination

The engine already publishes every book change as ITCH 5.0 onto `MarketDataRing`; nothing
reads it, and it drops once full. Build the component that does:

- a thread draining the ring into MoldUDP64 packets (session, sequence number, message
  count), sent as UDP multicast from a DPDK tx queue (`dpdk::ipv4_multicast_mac` gives the
  destination MAC; no ARP needed)
- replace the stale `market_feed` code
- a client-side consumer (or extend the load generator) that decodes the feed, checks
  sequence gaps, and rebuilds the book, as an end-to-end correctness check
- later: a MoldUDP64 retransmission request server, and snapshots for late joiners

## 2. Cancel on disconnect

Today a disconnected session's orders keep resting and trading, and the fills go into its
replay log. Nasdaq offers cancel-on-disconnect per port: when the session's connection is
lost, its resting orders are canceled (best effort; a Logout Request doesn't trigger it).

- a config flag (and later per-username)
- on `DISCONNECTED` (not a logout), the gateway cancels the session's live orders with a
  new engine request ("cancel everything for ClientId"). Only the engine knows which
  orders are live: the gateway keeps no per-order state on the inbound side
- the cancels land in the session's log, so a later re-login replays them as Order Canceled
- tests: in `server-tests` (engine played by the test) and `server-tap-tests` (real engine)

## 3. Outbound-thread performance pass

At saturation the outbound thread is now the bottleneck. In a closed-loop run, "engine
push → server pop" is ~200 µs at p50: replies queue in the engine's outbound ring while
the outbound thread works through the previous batch. Places to look:

- `tcp_sender::send()` per connection per flush: segment building, software TCP checksums
- the inbound side's software checksum verification (`tcp_params::verify_checksums`). On
  the tap path the kernel has already checked every frame, so it could be off there.
- encoding into scratch and then copying into the `SessionLog`, instead of encoding in place
- done: RPS on the tap (`TAP_RPS_CORE`) and `kOutboundBatch` 32 -> 512, together 607k -> 936k
  req/s as root. Each tap write still costs ~6k kernel cycles; segments now average ~1230
  of 1514 bytes
- done: two unbounded order maps. The responder now retires ignored replacements
  (`REPLACE_IGNORED`), and the engine finds orders by (ClientId, ClientOrderRef), so
  `OrderEntry` keeps no per-order map. Together 871k -> 1.10M req/s as root, RSS flat at
  ~440 MB (was 2.8 GB after 20 s and growing), inbound p50 13 -> 6 µs
- the responder's OrderId -> record map (~22% of the outbound thread): events could
  echo `client_ref`, leaving only a short buffer of recent notes for the OUCH-only fields
- then `stage_breakdown` again, run as root so `SCHED_FIFO` and core pinning are in effect

## 4. DPDK in the Docker image

The `Dockerfile` doesn't install DPDK, so the exchange (and the DPDK-backed tests) haven't
built in Docker since the server moved onto DPDK, and the bench's Docker backend can't run
the pair. Install `dpdk-dev`/`libdpdk-dev`, and give the services `cap_add: [NET_ADMIN]`
and `/dev/net/tun`, so the exchange can create its tap interface inside the container.

## 5. Multiple symbols

Every order trades in the one `BOOK_SYMBOL` book, whatever symbol it names.

- a symbol directory: symbol → stock locate → book
- one `Orderbook` per symbol in the engine; route `EngineRequest`s by locate
- reject unknown symbols ('S')
- ITCH Stock Directory messages at start of day, and per-locate publishing

## 6. Durable, full-day replay journal

SoupBinTCP promises every sequenced message across reconnects. Real venues meet that with
an on-disk, append-only journal for the whole day. Here each session keeps a fixed
in-memory ring (`SessionLog`, `SOUP_REPLAY_BYTES`), so replay covers a short outage, and
anything older gets Login Rejected 'S'. At full load (~435 MB/s of Sequenced Data across
sessions) a full-day journal needs care so the hot path doesn't take page faults or block
on I/O:

- per-session append-only files, written by a separate journal thread fed from the
  outbound thread (or mmap'd and pre-faulted ahead of the write cursor)
- replay serves recent messages from the ring and older ones from the file
- the same interface as `SessionLog`, so the outbound thread doesn't change

## 7. TCP retransmission and reassembly in the DPDK library

The library's TCP assumes a lossless path (tap, one host). For a real NIC it needs, in
its documented order: retransmission (an outbound-owned ring of unacked bytes plus an RTO
timer), then out-of-order reassembly. The work lives in the library repo; here it only
means testing `EXCHANGE_NETWORK_TAP 0` on real hardware.

## 8. SoupBinTCP odds and ends

- send End of Session ('Z') to every connected client when the exchange shuts down
- real credentials: a username/password table instead of accepting anything
- Debug packets over 512 bytes end the connection today; skip them instead
