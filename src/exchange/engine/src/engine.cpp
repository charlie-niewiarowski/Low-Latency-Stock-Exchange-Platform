//
// Created by charl on 11/25/2025.
//

#include "../include/engine.hpp"
#include "perf.hpp"

#include <immintrin.h>
#include <iostream>
#include <thread>

//=============================================================================
// Special Member Functions
//=============================================================================
Engine::Engine(InboundRing& in_ring, OutboundRing& out_ring, MarketDataRing& market_data_ring,
               std::atomic<bool>& stop)
    : book_(out_ring, market_data_ring), in_ring_(in_ring), out_ring_(out_ring), stop_(stop) {}

//=============================================================================
// run()
//=============================================================================
void Engine::run() {
    matching_thread_ = std::jthread([&]() {
        #if DIAGNOSTICS
        std::cerr << "matching tid: " << gettid() << std::endl;
        #endif

        pin_to_core(MATCHING_CORE);
        handleMatching();
    });
}

//=============================================================================
// step()  — test interface, processes all pending inbound messages once
//=============================================================================
#if TESTING
void Engine::step() {
    while (auto msg = in_ring_.pop()) {
        executeRequest(*msg);
    }
}
#endif

//=============================================================================
// run() Helper Suite
//=============================================================================
static std::atomic<uint64_t> g_dbg_iters_match{0};

void Engine::handleMatching() {
    // 1-ahead pipeline: grab the next request and prefetch the book structures it
    // will touch while we process the current one, hiding the price-ladder /
    // orders_ miss behind the current request's work.
    std::optional<EngineRequest> cur, nxt;

    while (!stop_.load(std::memory_order_relaxed)) {
        g_dbg_iters_match.fetch_add(1, std::memory_order_relaxed);
        if (!cur) {
            cur = in_ring_.pop();
            if (!cur) continue;
        }

        nxt = in_ring_.pop();
        if (nxt) book_.prefetch(*nxt);

        executeRequest(*cur);

        cur = std::move(nxt);
    }
    std::cerr << "DBG handleMatching total iterations=" << g_dbg_iters_match.load() << "\n";
}

//=============================================================================
// request routing
//=============================================================================
//
// The book does all the work -- matching, every resulting EngineEvent, and the
// ITCH for it (see orderbook.hpp); the engine only stamps when it took the
// request off the ring.
void Engine::executeRequest(const EngineRequest &req) {
    const Timestamp engine_pop_tsc = __rdtsc(); // t3
    book_.process(req, engine_pop_tsc);
}
