//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_MACROS_H
#define UNTITLED_MACROS_H

//=============================================================================
// user-level macros (feel free to tinker around)
//=============================================================================

#define DIAGNOSTICS 1 // print thread ids at startup, and per-stage latency histograms
                      // (latency.hpp) at shutdown on top of the always-on end-to-end one

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

#define MATCHING_CORE 1
#define INBOUND_CORE 2
#define OUTBOUND_CORE 3

//=============================================================================
// development macros (DO NOT TOUCH UNLESS FOR A REASON)
//=============================================================================

#define COMMUNICATION_RING_COUNT 524'288

#define PREALLOCATION_COUNT 1'000'000

// SoupBinTCP session clients log into. Any username/password is accepted
// for now; a Login Request naming another session is rejected.
#define SOUP_SESSION "SIM"
#define MAX_CLIENTS 64

// Engine ring capacity server-tests runs with (power of two) -- small, since
// the test plays the engine and drains them itself.
#define RINGBUF_SIZE 512

// Requests answered before latency.hpp starts recording, so cold-start
// effects (page faults, cache warm-up) stay out of the histograms.
#define LATENCY_SAMPLE_DROP 5

#endif //UNTITLED_MACROS_H