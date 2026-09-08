//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_MACROS_H
#define UNTITLED_MACROS_H

//=============================================================================
// user-level macros (feel free to tinker around)
//=============================================================================

#define LOGGING 0
#define DIAGNOSTICS 1

#ifndef TESTING
#define TESTING 0 // overridable at compile time (see tests/CMakeLists.txt), so the
                  // release/debug binaries stay lean while test targets get the
                  // TESTING-gated accessors in engine.hpp / orderbook.hpp
#endif

#define PREFETCH 1  // software prefetch hints on the hot path; set 0 to compile them out

#define MIN_PRICE (1)           // the 100 is to include decimals since we are
#define MAX_PRICE (1'000 * 100) // using uint32_t, so $10.25 = uint32_t(1025)

#define MATCHING_CORE 1
#define INBOUND_CORE 2
#define OUTBOUND_CORE 3

//=============================================================================
// development macros (DO NOT TOUCH UNLESS FOR A REASON)
//=============================================================================

#define COMMUNICATION_RING_COUNT 524'288

#define PREALLOCATION_COUNT 1'000'000
#define TRADE_RING_COUNT COMMUNICATION_RING_COUNT // only relevant for logging == 1

#define PORT "4000"
#define MAX_CLIENTS 64

#define PIPELINE_DEPTH 32

// Per-connection outbound staging ring capacity (must be a power of two).
// Sized well above PIPELINE_DEPTH: a single incoming order can fan out into
// many MATCH messages for one connection (one per resting order it sweeps),
// so this has to absorb that burst, not just the request/response pipeline
// depth. Overflowing this now condemns the connection as a slow consumer
// (see Server::routeOutboundMessages) rather than silently dropping
// messages, so it should only ever be hit by a genuinely stuck client.
#define RINGBUF_SIZE 512

#define LATENCY_SAMPLE_DROP 5
#define LATENCY_SAMPLE_COUNT (2000 + LATENCY_SAMPLE_DROP)
                                                              // get the desired number of samples
#endif //UNTITLED_MACROS_H