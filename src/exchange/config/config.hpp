//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_MACROS_H
#define UNTITLED_MACROS_H

//=============================================================================
// user-level macros (feel free to tinker around)
//=============================================================================

#define DIAGNOSTICS 1 // per-stage latency histograms (latency.hpp) at shutdown, on top of
                      // the always-on end-to-end one. (Every thread's tid and name is
                      // printed at startup either way.)

#ifndef TESTING
#define TESTING 0 // overridable at compile time (see tests/CMakeLists.txt), so the
                  // release/debug binaries stay lean while test targets get the
                  // TESTING-gated accessors in engine.hpp / orderbook.hpp
#endif

#define PREFETCH 1  // software prefetch hints on the hot path; set 0 to compile them out

#define MIN_PRICE (1)           // the 100 is to include decimals since we are
#define MAX_PRICE (1'000 * 100) // using uint32_t, so $10.25 = uint32_t(1025)

// Every order lands in this one book regardless of the symbol it was entered
// with (multi-symbol routing comes later); ITCH publishes it under this name.
#define BOOK_SYMBOL "SIM"
#define BOOK_STOCK_LOCATE 1

// The EAL's only lcore: DPDK pins the thread that brings it up (the
// exchange's main thread) here, along with its own service threads.
// Without a core list it claims every core and starts an idle worker
// thread on each.
#define EAL_CORE 0
#define MATCHING_CORE 1
#define INBOUND_CORE 2
#define OUTBOUND_CORE 3

// Where clients reach the exchange. EXCHANGE_NETWORK_TAP 1 runs it on a
// net_tap port: DPDK creates the kernel interface EXCHANGE_TAP_IFACE at
// EXCHANGE_KERNEL_IPV4/24 and answers as EXCHANGE_IPV4, so a kernel-socket
// client on this host connects to EXCHANGE_IPV4:EXCHANGE_PORT. Needs root
// (CAP_NET_ADMIN). 0 runs it on a DPDK-bound NIC instead.
#define EXCHANGE_NETWORK_TAP 1
#define EXCHANGE_TAP_IFACE "dpdktap0"
#define EXCHANGE_KERNEL_IPV4 10, 77, 0, 1 // RTE_IPV4 arguments
#define EXCHANGE_IPV4 10, 77, 0, 2
#define EXCHANGE_PORT 4000

//=============================================================================
// development macros (DO NOT TOUCH UNLESS FOR A REASON)
//=============================================================================

#define COMMUNICATION_RING_COUNT 524'288

#define PREALLOCATION_COUNT 1'000'000

// SoupBinTCP session clients log into. Any username/password is accepted
// for now; a Login Request naming another session is rejected.
#define SOUP_SESSION "SIM"
// Usernames (each its own session and ClientId) the exchange accepts in a day.
#define MAX_CLIENTS 64

// Bytes of Sequenced Data each session keeps for re-login replay (a power of
// two), allocated on its first login; also its send buffer, so a connected
// client this far behind is disconnected. At ~145 bytes per order (ack plus
// a fill on each side), 16 MiB is ~110k orders of history. The test targets
// override these.
#ifndef SOUP_REPLAY_BYTES
#define SOUP_REPLAY_BYTES (16u << 20)
#endif
// A logged-in client that sends nothing (not even a heartbeat) for this long
// is disconnected; so is a connection that doesn't log in within
// SOUP_LOGIN_TIMEOUT_MS. The spec's typical values.
#ifndef SOUP_IDLE_TIMEOUT_MS
#define SOUP_IDLE_TIMEOUT_MS 15'000
#endif
#ifndef SOUP_LOGIN_TIMEOUT_MS
#define SOUP_LOGIN_TIMEOUT_MS 30'000
#endif

// Engine ring capacity server-tests runs with (power of two) -- small, since
// the test plays the engine and drains them itself.
#define RINGBUF_SIZE 512

// Requests answered before latency.hpp starts recording, so cold-start
// effects (page faults, cache warm-up) stay out of the histograms.
#define LATENCY_SAMPLE_DROP 5

#endif //UNTITLED_MACROS_H